/*
** sa-tromp: Standalone Equihash 192,7 GPU miner with local verification
** 
** Purpose: Verify GPU-generated solutions pass Equihash verification
**          before integrating into pool mining (sa-solver)
**
** Usage: ./sa-tromp [nonces]
**        Default: 100000 nonces (200000 hashes)
**        Recommended: 1000000-2000000 nonces for solution probability
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <endian.h>
#include <CL/cl.h>

// Type definitions needed by param.h
typedef uint8_t uchar;
typedef uint32_t uint;

#include "blake.h"
#include "param.h"
#include "_kernel.h"

#define PARAM_N 192
#define PARAM_K 7
#define ZCASH_HASH_LEN 48
#define RESTBITS 4
#define BUCKBITS (24-RESTBITS)
#define NBUCKETS (1<<BUCKBITS)  // 1M buckets
#define NSLOTS 40
#define SLOTBITS 6    // log2(64); 6 bits holds 0-63
#define HASHBYTES_STAGE0 24

/* Define htole32 for little-endian conversion if not available */
#ifndef htole32
#define htole32(x) ((uint32_t)(x))
#endif

// Solution extraction structures (from solution_extraction.c)
#include "solution_extraction.c"

// Verification function using Tromp's blake2b
#include "blake/blake2.h"

/* Define htole32 for little-endian conversion if not available */
#ifndef htole32
#define htole32(x) ((uint32_t)(x))
#endif

static void eh_genhash(const blake2b_state_t *ctx, uint32_t idx, uint32_t nonce, uint8_t *hash)
{
    /* Match GPU kernel_round0_gen hash generation exactly.
     * Block 2 = nonce(m[0] low32) || index<<32(m[1] high32) || zeros.
     * ctx = h after block1 (bytes 0-127, no nonce). bytes = 128. */
    blake2b_state_t st = *ctx;
    const uint32_t hashes_per_blake = 512 / PARAM_N;   /* = 2 */
    const uint32_t hash_bytes = PARAM_N / 8;            /* = 24 */
    uint8_t full_hash[ZCASH_HASH_LEN];
    uint64_t message[16] = {0};
    uint32_t g = idx / hashes_per_blake;
    message[0] = (uint64_t)nonce;          /* m[0] low32 = nonce */
    message[1] = (uint64_t)g << 32;        /* m[1] high32 = blake-call index */
    st.bytes = 128;                        /* initial state was built after block1 (128 bytes) */
    zcash_blake2b_update(&st, (const uint8_t *)message, 2 * sizeof(uint64_t), 1);
    zcash_blake2b_final(&st, full_hash, ZCASH_HASH_LEN);
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

static int verbose = 0;

static uint32_t eh_verifyrec(const blake2b_state_t *ctx, uint32_t *indices, uint8_t *hash, int r, uint32_t nonce)
{
    const uint32_t hash_bytes = PARAM_N / 8;

    if (r == 0) {
        eh_genhash(ctx, *indices, nonce, hash);
        if (verbose) {
            printf("  [r=0] idx=%u hash: %02x%02x%02x%02x%02x%02x\n", 
                   *indices, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5]);
        }
        return 1;
    }

    uint32_t *indices1 = indices + (1 << (r - 1));
    if (*indices >= *indices1) {
        if (verbose) {
            printf("  [r=%d] FAIL: ordering violation indices[0]=%u >= indices[%d]=%u\n",
                   r, *indices, (1 << (r - 1)), *indices1);
        }
        return 0;
    }

    uint8_t hash0[hash_bytes], hash1[hash_bytes];
    if (!eh_verifyrec(ctx, indices, hash0, r - 1, nonce)) {
        if (verbose) printf("  [r=%d] FAIL: left subtree failed\n", r);
        return 0;
    }
    if (!eh_verifyrec(ctx, indices1, hash1, r - 1, nonce)) {
        if (verbose) printf("  [r=%d] FAIL: right subtree failed\n", r);
        return 0;
    }

    for (uint32_t i = 0; i < hash_bytes; i++)
        hash[i] = hash0[i] ^ hash1[i];

    if (verbose && r == 1) {
        printf("  [r=1] XOR result: %02x%02x%02x%02x%02x%02x (from %02x%02x%02x ^ %02x%02x%02x)\n",
               hash[0], hash[1], hash[2], hash[3], hash[4], hash[5],
               hash0[0], hash0[1], hash0[2], hash1[0], hash1[1], hash1[2]);
    }

    int b = r < PARAM_K ? r * PREFIX : PARAM_N;
    int i;
    for (i = 0; i < b / 8; i++) {
        if (hash[i]) {
            if (verbose) {
                printf("  [r=%d] FAIL: XOR byte %d is %02x (expected 00), need %d zero bits\n",
                       r, i, hash[i], b);
            }
            return 0;
        }
    }
    if ((b % 8) && (hash[i] >> (8 - (b % 8)))) {
        if (verbose) printf("  [r=%d] FAIL: nonzero partial byte %d: %02x\n", r, i, hash[i]);
        return 0;
    }
    return 1;
}

static uint32_t verify_equihash_full(uint32_t *indices, const blake2b_state_t *blake_ctx,
                                      uint32_t nonce, int verbose_mode)
{
    verbose = verbose_mode;
    uint8_t hash[PARAM_N / 8];
    int result = eh_verifyrec(blake_ctx, indices, hash, PARAM_K, nonce);
    
    if (result && verbose) {
        printf("  ✓ All 7 stages have 24-bit collisions\n");
        printf("  ✓ Final XOR = all zeros\n");
        printf("  ✓ Solution is VALID\n");
    }
    
    return result;
}

// OpenCL context
cl_platform_id platform;
cl_device_id device;
cl_context context;
cl_command_queue queue;
cl_program program;
cl_kernel kernels[7];
cl_kernel kernel_round0_gen;
cl_kernel kernel_extract_attrs_k;

void check_error(cl_int err, const char *operation) {
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Error during %s: %d\n", operation, err);
        exit(1);
    }
}

void init_opencl(void) {
    // Static counter to force unique builds (workaround for Beignet kernel caching)
    static int build_id = 0;
    build_id++;
    
    cl_int err;
    
    err = clGetPlatformIDs(1, &platform, NULL);
    check_error(err, "clGetPlatformIDs");
    
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);
    check_error(err, "clGetDeviceIDs");
    
    context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    check_error(err, "clCreateContext");
    
    queue = clCreateCommandQueue(context, device, 0, &err);
    check_error(err, "clCreateCommandQueue");
    
    program = clCreateProgramWithSource(context, 1, &ocl_code, NULL, &err);
    check_error(err, "clCreateProgramWithSource");
    
    // Add unique build ID to force recompilation (workaround for Beignet caching)
    char build_opts[256];
    snprintf(build_opts, sizeof(build_opts), "-DPARAM_N=192 -DPARAM_K=7 -DBUILD_ID=%d", build_id);
    err = clBuildProgram(program, 1, &device, build_opts, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t log_size;
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char *log = malloc(log_size);
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
        fprintf(stderr, "Build error:\n%s\n", log);
        free(log);
        exit(1);
    }
    
    const char *kernel_names[] = {
        "kernel_stage1_collisions", "kernel_stage2_collisions", "kernel_stage3_collisions",
        "kernel_stage4_collisions", "kernel_stage5_collisions", "kernel_stage6_collisions",
        "kernel_stage7_collisions"
    };
    
    for (int i = 0; i < 7; i++) {
        kernels[i] = clCreateKernel(program, kernel_names[i], &err);
        check_error(err, kernel_names[i]);
    }
    kernel_round0_gen = clCreateKernel(program, "kernel_round0_gen", &err);
    check_error(err, "kernel_round0_gen");
    kernel_extract_attrs_k = clCreateKernel(program, "kernel_extract_attrs", &err);
    check_error(err, "kernel_extract_attrs");
}

void cleanup_opencl(void) {
    for (int i = 0; i < 7; i++) clReleaseKernel(kernels[i]);
    clReleaseKernel(kernel_round0_gen);
    clReleaseKernel(kernel_extract_attrs_k);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
}

void generate_round0_hashes(unsigned char *hashes, uint32_t nonces, uint8_t *header, uint32_t nonce_offset) {
    blake2b_state_t blake_base, blake;
    
    zcash_blake2b_init(&blake_base, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    zcash_blake2b_update(&blake_base, header, 128, 0);
    
    uint32_t num_hashes = nonces * 2;
    for (uint32_t idx = 0; idx < num_hashes; idx++) {
        blake = blake_base;
        uint32_t g = nonce_offset + (idx / 2);
        
        // Debug: print first and last few nonces
        if (idx < 4 || idx >= num_hashes - 2) {
            printf("  [DEBUG] idx=%u -> nonce g=%u\n", idx, g);
        }
        
        zcash_blake2b_update(&blake, (uint8_t*)&g, sizeof(g), 0);
        
        uint8_t blakehash[48];
        zcash_blake2b_final(&blake, blakehash, 48);
        
        uint32_t offset = (idx % 2) * 24;
        memcpy(hashes + idx * HASHBYTES_STAGE0, blakehash + offset, HASHBYTES_STAGE0);
    }
}

/*
 * mine_batch: run one complete Equihash 192,7 mining attempt.
 *
 * Phase 1: kernel_round0_gen fills tree0 with 2^25 hashes (2^24 work items,
 *          batched at 2^18 to stay within Beignet's NDRange limit).
 * Phase 2: 7-stage collision cascade — each stage dispatched in 2^18-sized
 *          batches.  Tree buffers are allocated/freed progressively to fit
 *          within the ~5.8 GB shared memory budget.
 *
 * NOTE: solution extraction via mine_batch_extract() (defined below).
 */
int mine_batch(uint32_t nonce_idx, uint8_t *header, int show_progress) {
    cl_int err;
    /* Beignet safe dispatch size: 2^20 work items per clEnqueueNDRangeKernel */
    const size_t DISPATCH = (size_t)(1 << 20);
    const size_t tree_size = (size_t)NBUCKETS * NSLOTS;
    uint32_t *cpu_attrs[8] = {NULL};
    /* GPU slot sizes with 4-byte alignment padding (matches Beignet OpenCL C sizeof).
     * Stages 2,3,5,6,7 have uchar hash[] that doesn't fill to 4-byte boundary,
     * so Beignet adds tail padding. */
    const size_t _slot_sz[8] = {28,28,24,20,16,16,12,8};

    if (show_progress)
        printf("\n--- Mining nonce %u ---\n", nonce_idx);

    /* ── Phase 1: GPU hash generation ────────────────────────────────────── */
    /* Blake2b state: compress block 1 (bytes 0-127 of headernonce, no nonce).
     * GPU block 2 = nonce(m[0] low32) || index<<32(m[1] high32) || zeros.
     * v[12] ^= 144 = 140+4; nonce passed as kernel arg 3. */
    uint8_t hdr128[128] = {0};
    memcpy(hdr128, header, 108);  /* header is ≤108 bytes; rest zero */

    blake2b_param P = {0};
    P.digest_length = ZCASH_HASH_LEN;
    P.fanout = 1;
    P.depth = 1;
    char personals[16];
    memcpy(personals, "ZERO_PoW", 8);
    uint32_t le_N = htole32(PARAM_N);
    uint32_t le_K = htole32(PARAM_K);
    memcpy(personals + 8, &le_N, 4);
    memcpy(personals + 12, &le_K, 4);
    memcpy(P.personal, personals, 16);

    blake2b_state tromp_st;
    blake2b_init_param(&tromp_st, &P);
    blake2b_update(&tromp_st, hdr128, 128);  /* compress block 1; nonce NOT here */

    blake2b_state_t blake_gen;
    memcpy(blake_gen.h, tromp_st.h, 8 * sizeof(uint64_t));
    blake_gen.bytes = 128;

    cl_mem buf_blake_st = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         8 * sizeof(uint64_t), blake_gen.h, &err);
    check_error(err, "buf_blake_st");

    /* tree0: NBUCKETS * NSLOTS * sizeof(stage0_slot_t) = 1M * 32 * 28 = 860 MB */
    cl_mem buf_tree0 = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                      tree_size * sizeof(stage0_slot_t), NULL, &err);
    check_error(err, "buf_tree0");

    cl_mem buf_t0_cnt = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                       NBUCKETS * sizeof(uint32_t), NULL, &err);
    check_error(err, "buf_t0_cnt");
    {
        uint32_t *z = calloc(NBUCKETS, sizeof(uint32_t));
        clEnqueueWriteBuffer(queue, buf_t0_cnt, CL_TRUE, 0,
                             NBUCKETS * sizeof(uint32_t), z, 0, NULL, NULL);
        free(z);
    }

    clSetKernelArg(kernel_round0_gen, 0, sizeof(cl_mem), &buf_blake_st);
    clSetKernelArg(kernel_round0_gen, 1, sizeof(cl_mem), &buf_tree0);
    clSetKernelArg(kernel_round0_gen, 2, sizeof(cl_mem), &buf_t0_cnt);
    clSetKernelArg(kernel_round0_gen, 3, sizeof(cl_uint), &nonce_idx);

    {
        /* 2^24 total work items = 2^25 hashes; split into 64 batches of 2^18 */
        const size_t TOTAL_GEN = (size_t)(1 << 24);
        const size_t LWS = 64;
        clock_t t0 = clock();
        for (size_t base = 0; base < TOTAL_GEN; base += DISPATCH) {
            err = clEnqueueNDRangeKernel(queue, kernel_round0_gen, 1,
                                         &base, &DISPATCH, &LWS, 0, NULL, NULL);
            check_error(err, "kernel_round0_gen");
            clFinish(queue);
        }
        /* cpu_attrs[0] readback disabled (tmp OOM at NSLOTS=40) */
        clReleaseMemObject(buf_blake_st);

        uint32_t *cnt = calloc(NBUCKETS, sizeof(uint32_t));
        clEnqueueReadBuffer(queue, buf_t0_cnt, CL_TRUE, 0,
                            NBUCKETS * sizeof(uint32_t), cnt, 0, NULL, NULL);
        uint64_t tot = 0; uint32_t mx = 0, ov = 0;
        for (uint32_t b = 0; b < NBUCKETS; b++) {
            tot += cnt[b];
            if (cnt[b] > mx) mx = cnt[b];
            if (cnt[b] > NSLOTS) ov++;
        }
        free(cnt);
        if (show_progress)
            printf("  Stage 0: %.2fs  %llu hashes  avg=%.1f/bucket  max=%u  overflow=%u\n",
                   (double)(clock() - t0) / CLOCKS_PER_SEC,
                   (unsigned long long)tot, (double)tot/NBUCKETS, mx, ov);
    }

    /* ── Phase 2: 7-stage collision cascade ─────────────────────────────── */

    /* Allocate all 7 count buffers (4 MB each = 28 MB total) */
    cl_mem buf_counts[7];
    {
        uint32_t *z = calloc(NBUCKETS, sizeof(uint32_t));
        for (int i = 0; i < 7; i++) {
            buf_counts[i] = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                           NBUCKETS * sizeof(uint32_t), NULL, &err);
            check_error(err, "buf_counts");
            clEnqueueWriteBuffer(queue, buf_counts[i], CL_TRUE, 0,
                                 NBUCKETS * sizeof(uint32_t), z, 0, NULL, NULL);
        }
        free(z);
    }

    const size_t STAGE1_GPU_BYTES = 28; /* uint32_t attr(4) + hash[21] + pad[3] */
    cl_mem buf_tree1 = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                      tree_size * STAGE1_GPU_BYTES, NULL, &err);
    check_error(err, "buf_tree1");

    clSetKernelArg(kernels[0], 0, sizeof(cl_mem), &buf_tree0);
    clSetKernelArg(kernels[0], 1, sizeof(cl_mem), &buf_t0_cnt);
    clSetKernelArg(kernels[0], 2, sizeof(cl_mem), &buf_tree1);
    clSetKernelArg(kernels[0], 3, sizeof(cl_mem), &buf_counts[0]);

    {
        clock_t ts = clock();
        for (size_t base = 0; base < NBUCKETS; base += DISPATCH) {
            size_t count = (base + DISPATCH <= NBUCKETS) ? DISPATCH : (NBUCKETS - base);
            err = clEnqueueNDRangeKernel(queue, kernels[0], 1,
                                         &base, &count, NULL, 0, NULL, NULL);
            check_error(err, "stage1");
            clFinish(queue);
        }
        /* cpu_attrs[1] readback disabled (tmp OOM at NSLOTS=40) */
        /* tree0 reused as ping-pong buffer — do NOT release here */
        clReleaseMemObject(buf_t0_cnt);

        uint32_t *cnt = calloc(NBUCKETS, sizeof(uint32_t));
        clEnqueueReadBuffer(queue, buf_counts[0], CL_TRUE, 0,
                            NBUCKETS * sizeof(uint32_t), cnt, 0, NULL, NULL);
        uint32_t tot = 0, mx = 0;
        for (uint32_t b = 0; b < NBUCKETS; b++) {
            tot += cnt[b]; if (cnt[b] > mx) mx = cnt[b];
        }
        free(cnt);
        if (show_progress)
            printf("  Stage 1: %.2fs  %u collisions  avg=%.1f/bucket  max=%u\n",
                   (double)(clock() - ts) / CLOCKS_PER_SEC,
                   tot, (double)tot / NBUCKETS, mx);
    }

    /* Stages 2-7: double-buffer reuse of buf_tree0 / buf_tree1.
     * No new GPU allocations — stages alternate writing into the buffer
     * not currently used as input. Total GPU = 2 × 1.07 GB (constant). */
    cl_mem prev = buf_tree1, curr = NULL;
    {
        uint32_t *cnt = calloc(NBUCKETS, sizeof(uint32_t));
        for (int s = 2; s <= 7; s++) {
            /* Ping-pong: even stages write to tree0, odd stages write to tree1 */
            curr = (s % 2 == 0) ? buf_tree0 : buf_tree1;

            clSetKernelArg(kernels[s-1], 0, sizeof(cl_mem), &prev);
            clSetKernelArg(kernels[s-1], 1, sizeof(cl_mem), &buf_counts[s-2]);
            clSetKernelArg(kernels[s-1], 2, sizeof(cl_mem), &curr);
            clSetKernelArg(kernels[s-1], 3, sizeof(cl_mem), &buf_counts[s-1]);

            clock_t ts = clock();
            /* Stage 7 is O(NSLOTS^2) per WI — use smaller dispatch to avoid watchdog */
            size_t disp = (s == 7) ? (DISPATCH / 4) : DISPATCH;
            for (size_t base = 0; base < NBUCKETS; base += disp) {
                size_t count = (base + disp <= NBUCKETS) ? disp : (NBUCKETS - base);
                err = clEnqueueNDRangeKernel(queue, kernels[s-1], 1,
                                             &base, &count, NULL, 0, NULL, NULL);
                check_error(err, "stage_kernel");
                clFinish(queue);
            }

            /* cpu_attrs[s] readback disabled (tmp OOM at NSLOTS=40) */
            prev = curr;

            clEnqueueReadBuffer(queue, buf_counts[s-1], CL_TRUE, 0,
                                NBUCKETS * sizeof(uint32_t), cnt, 0, NULL, NULL);
            uint32_t tot = 0;
            for (uint32_t b = 0; b < NBUCKETS; b++) tot += cnt[b];
            if (show_progress)
                printf("  Stage %d: %.2fs  %u %s\n", s,
                       (double)(clock() - ts) / CLOCKS_PER_SEC, tot,
                       s == 7 ? "solution candidates" : "collisions");
        }
        free(cnt);
    }

    /* ── Phase 3: Check for solution candidates ──────────────────────────── */
    int valid_solutions = 0;

    if (curr) {
        uint32_t nsol = 0;
        clEnqueueReadBuffer(queue, buf_counts[6], CL_TRUE,
                            0, sizeof(uint32_t), &nsol, 0, NULL, NULL);
        if (nsol > 65536) nsol = 65536;

        if (show_progress)
            printf("  Stage 7: %u solution candidate(s) in bucket 0\n", nsol);

        if (nsol > 0) {
            /* ── Phase 3b: Re-run pipeline with compact attr readback ────────────
             * Strategy: pipeline is deterministic (same blake_gen → same trees).
             * Re-run one tree at a time, read attrs with compact kernel (4B/slot),
             * then release that tree before allocating next. This keeps peak GPU RAM
             * to ~1 tree (1.12GB) + compact buf (160MB) at any one time.
             * CPU: accumulate 8 × tree_size × 4B = 1.28GB total.
             * Combined peak ≈ 1.12 + 0.16 + 1.28 = 2.56GB — fits in 3.1GB. */

            /* Helper: extract attrs from a tree buffer using compact GPU kernel */
            #define EXTRACT_ATTRS(stage_idx, tree_buf, stride_bytes) do { \
                uint32_t _ssz = (uint32_t)(stride_bytes); \
                cpu_attrs[stage_idx] = malloc(tree_size * sizeof(uint32_t)); \
                cl_mem _compact = clCreateBuffer(context, CL_MEM_WRITE_ONLY, \
                    tree_size * sizeof(uint32_t), NULL, &err); \
                check_error(err, "compact_buf"); \
                clSetKernelArg(kernel_extract_attrs_k, 0, sizeof(cl_mem), &(tree_buf)); \
                clSetKernelArg(kernel_extract_attrs_k, 1, sizeof(uint32_t), &_ssz); \
                clSetKernelArg(kernel_extract_attrs_k, 2, sizeof(cl_mem), &_compact); \
                { size_t _gws = tree_size, _lws = 64; \
                  clEnqueueNDRangeKernel(queue, kernel_extract_attrs_k, 1, NULL, &_gws, &_lws, 0, NULL, NULL); } \
                clFinish(queue); \
                clEnqueueReadBuffer(queue, _compact, CL_TRUE, 0, \
                    tree_size * sizeof(uint32_t), cpu_attrs[stage_idx], 0, NULL, NULL); \
                clReleaseMemObject(_compact); \
            } while(0)

            /* Re-run stages 1-7 in sequence, one tree at a time.
             * We reuse buf_counts[] already zeroed; zero them again. */
            {
                uint32_t *z = calloc(NBUCKETS, sizeof(uint32_t));
                for (int i = 0; i < 7; i++)
                    clEnqueueWriteBuffer(queue, buf_counts[i], CL_TRUE, 0,
                        NBUCKETS * sizeof(uint32_t), z, 0, NULL, NULL);
                free(z);
            }

            /* Release buf_tree1 now — it is not used in the re-run.
             * Must happen before scratch_a/scratch_b allocation to keep
             * peak GPU at 2 trees (scratch_a + scratch_b = 2.24 GB) not 4. */
            clReleaseMemObject(buf_tree1);

            /* Allocate a single scratch tree buffer for the re-run chain.
             * We process one stage at a time: prev = source, scratch = output.
             * After reading attrs, swap prev=scratch for next stage.
             * Peak GPU: buf_tree0 (source stage0) + scratch_a + scratch_b = 3 trees briefly,
             * then buf_tree0 released after EXTRACT_ATTRS(0), leaving 2 trees. */
            /* Allocate scratch_a only — scratch_b deferred until buf_tree0 released.
             * Peak before deferral would be buf_tree0(1.12GB)+scratch_a+scratch_b=3.36GB.
             * After deferral: buf_tree0(1.12GB)+scratch_a(1.12GB)=2.24GB, then release
             * buf_tree0, then alloc scratch_b → peak stays at 2.24GB. */
            cl_mem scratch_a = clCreateBuffer(context, CL_MEM_READ_WRITE,
                tree_size * 28, NULL, &err);  /* max slot size = 28 */
            check_error(err, "scratch_a");
            cl_mem scratch_b = NULL; /* allocated after buf_tree0 released below */

            /* Reinit buf_t0_cnt was already released — need stage0 count for stage1.
             * Re-read it from existing buf_tree0 data: use buf_counts[0] which we just
             * re-zeroed. But stage1 kernel reads buf_t0_cnt (the stage0 count buffer),
             * not buf_counts[0]. We released buf_t0_cnt. Re-create it. */
            cl_mem buf_t0_cnt2 = clCreateBuffer(context, CL_MEM_READ_WRITE,
                NBUCKETS * sizeof(uint32_t), NULL, &err);
            check_error(err, "buf_t0_cnt2");
            /* Re-run kernel_round0_gen into scratch_a to get fresh counts */
            {
                uint32_t *z = calloc(NBUCKETS, sizeof(uint32_t));
                clEnqueueWriteBuffer(queue, buf_t0_cnt2, CL_TRUE, 0,
                    NBUCKETS * sizeof(uint32_t), z, 0, NULL, NULL);
                free(z);
                clSetKernelArg(kernel_round0_gen, 0, sizeof(cl_mem), &buf_blake_st);
                /* buf_blake_st was released after stage0! Need to recreate it. */
            }
            /* buf_blake_st was released at line ~324. We need blake_gen to recreate it. */
            cl_mem buf_blake_st2 = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                8 * sizeof(uint64_t), blake_gen.h, &err);
            check_error(err, "buf_blake_st2");
            {
                uint32_t *z = calloc(NBUCKETS, sizeof(uint32_t));
                clEnqueueWriteBuffer(queue, buf_t0_cnt2, CL_TRUE, 0,
                    NBUCKETS * sizeof(uint32_t), z, 0, NULL, NULL);
                free(z);
                clSetKernelArg(kernel_round0_gen, 0, sizeof(cl_mem), &buf_blake_st2);
                clSetKernelArg(kernel_round0_gen, 1, sizeof(cl_mem), &scratch_a);
                clSetKernelArg(kernel_round0_gen, 2, sizeof(cl_mem), &buf_t0_cnt2);
                clSetKernelArg(kernel_round0_gen, 3, sizeof(cl_uint), &nonce_idx);
                for (size_t base = 0; base < (size_t)(1 << 24); base += DISPATCH) {
                    clEnqueueNDRangeKernel(queue, kernel_round0_gen, 1,
                        &base, &DISPATCH, &(size_t){64}, 0, NULL, NULL);
                    clFinish(queue);
                }
                clReleaseMemObject(buf_blake_st2);
            }
            /* scratch_a now has fresh stage0 data.
             * Read stage0 attrs from scratch_a (ensures same run as stages 1-7).
             * Release buf_tree0 immediately after — frees 1.12 GB before stages 1-7. */
            EXTRACT_ATTRS(0, scratch_a, _slot_sz[0]);
            clReleaseMemObject(buf_tree0);
            /* buf_tree1 already released above (before scratch alloc).
             * buf_tree0 now released — safe to alloc scratch_b (peak stays 2.24GB). */
            scratch_b = clCreateBuffer(context, CL_MEM_READ_WRITE,
                tree_size * 28, NULL, &err);
            check_error(err, "scratch_b");

            /* Run stages 1-7, ping-ponging scratch_a / scratch_b */
            cl_mem sp = scratch_a, sc = NULL;
            for (int s = 1; s <= 7; s++) {
                sc = (s % 2 == 1) ? scratch_b : scratch_a;
                cl_mem cnt_in  = (s == 1) ? buf_t0_cnt2 : buf_counts[s-2];
                cl_mem cnt_out = buf_counts[s-1];

                clSetKernelArg(kernels[s-1], 0, sizeof(cl_mem), &sp);
                clSetKernelArg(kernels[s-1], 1, sizeof(cl_mem), &cnt_in);
                clSetKernelArg(kernels[s-1], 2, sizeof(cl_mem), &sc);
                clSetKernelArg(kernels[s-1], 3, sizeof(cl_mem), &cnt_out);

                size_t disp2 = (s == 7) ? (DISPATCH / 4) : DISPATCH;
                for (size_t base = 0; base < NBUCKETS; base += disp2) {
                    size_t count = (base + disp2 <= NBUCKETS) ? disp2 : (NBUCKETS - base);
                    clEnqueueNDRangeKernel(queue, kernels[s-1], 1,
                        &base, &count, NULL, 0, NULL, NULL);
                    clFinish(queue);
                }

                EXTRACT_ATTRS(s, sc, _slot_sz[s]);
                /* Debug: dump first slots of current stage */
                if (s == 7) {
                    uint8_t raw[32];
                    clEnqueueReadBuffer(queue, sc, CL_TRUE, 0, 32, raw, 0, NULL, NULL);
                    printf("  DBG scratch_b raw[0..31]:");
                    for (int _d=0; _d<32; _d++) printf(" %02x", raw[_d]);
                    printf("\n");
                    printf("  DBG cpu_attrs[7][0..3]: %08x %08x %08x %08x\n",
                           cpu_attrs[7][0], cpu_attrs[7][1], cpu_attrs[7][2], cpu_attrs[7][3]);
                }
                if (s == 1) {
                    printf("  DBG cpu_attrs[0][0..3]: %08x %08x %08x %08x\n",
                           cpu_attrs[0][0], cpu_attrs[0][1], cpu_attrs[0][2], cpu_attrs[0][3]);
                    printf("  DBG cpu_attrs[1][0..3]: %08x %08x %08x %08x\n",
                           cpu_attrs[1][0], cpu_attrs[1][1], cpu_attrs[1][2], cpu_attrs[1][3]);
                    /* Verify xi[0] and xi[1] actually collide on 24 bits */
                    uint32_t xi0 = cpu_attrs[0][0], xi1 = cpu_attrs[0][1];
                    uint8_t h0[24], h1[24];
                    eh_genhash(&blake_gen, xi0, nonce_idx, h0);
                    eh_genhash(&blake_gen, xi1, nonce_idx, h1);
                    printf("  DBG xi0=%u hash: %02x%02x%02x  xi1=%u hash: %02x%02x%02x  XOR: %02x%02x%02x\n",
                           xi0, h0[0],h0[1],h0[2], xi1, h1[0],h1[1],h1[2],
                           h0[0]^h1[0], h0[1]^h1[1], h0[2]^h1[2]);
                    /* Read the actual stage0 GPU slot 0 raw bytes to compare */
                    uint8_t s0raw[28];
                    clEnqueueReadBuffer(queue, sp, CL_TRUE, 0, 28, s0raw, 0, NULL, NULL);
                    printf("  DBG stage0 slot0 raw:");
                    for (int _d=0; _d<28; _d++) printf(" %02x", s0raw[_d]);
                    printf("\n");
                    /* Print blake state h[0..7] */
                    printf("  DBG blake_gen.h:");
                    for (int _d=0; _d<8; _d++) printf(" %016llx", (unsigned long long)blake_gen.h[_d]);
                    printf("  bytes=%llu\n", (unsigned long long)blake_gen.bytes);
                    /* Read back buf_blake_st2 to see what GPU actually received */
                    uint64_t gpu_h[8];
                    clEnqueueReadBuffer(queue, buf_blake_st2, CL_TRUE, 0, 64, gpu_h, 0, NULL, NULL);
                    printf("  DBG GPU blake_state:");
                    for (int _d=0; _d<8; _d++) printf(" %016llx", (unsigned long long)gpu_h[_d]);
                    printf("\n");
                    /* Compute xi=20196 hash manually step by step */
                    {
                        uint32_t _xi = 20196;
                        blake2b_state_t _st = blake_gen;
                        uint64_t _msg[16] = {0};
                        _msg[0] = (uint64_t)nonce_idx;
                        _msg[1] = (uint64_t)(_xi/2) << 32;
                        uint8_t _h[48];
                        zcash_blake2b_update(&_st, (const uint8_t*)_msg, 16, 1);
                        zcash_blake2b_final(&_st, _h, 48);
                        printf("  DBG xi=%u manual hash: %02x%02x%02x  bytes_after_upd=%llu\n",
                               _xi, _h[0], _h[1], _h[2], (unsigned long long)_st.bytes);
                    }
                }
                sp = sc;
            }
            clReleaseMemObject(buf_t0_cnt2);
            clReleaseMemObject(scratch_a);
            clReleaseMemObject(scratch_b);
            #undef EXTRACT_ATTRS

            /* Re-read nsol from re-run's Stage 7 output (buf_counts[6]).
             * The first-pass nsol is stale — re-run has different slot ordering. */
            clEnqueueReadBuffer(queue, buf_counts[6], CL_TRUE,
                                0, sizeof(uint32_t), &nsol, 0, NULL, NULL);
            if (nsol > 65536) nsol = 65536;
            if (show_progress)
                printf("  Stage 7 (re-run): %u solution candidate(s)\n", nsol);

            /* ── Phase 3c: Extract and verify solutions ── */
            uint32_t tree_sz = (uint32_t)tree_size;
            int n_extracted = 0, n_verified = 0;
            /* Debug: dump attr chain for first candidate */
            if (nsol > 0) {
                uint32_t a7 = cpu_attrs[7][0];
                uint32_t b6 = a7 >> 12, si6 = (a7>>6)&0x3F, sj6 = a7&0x3F;
                uint32_t flat6i = b6*NSLOTS + si6, flat6j = b6*NSLOTS + sj6;
                printf("  DBG cand0: attr7=0x%08x  b6=%u si=%u sj=%u  flat6i=%u flat6j=%u tree_sz=%u\n",
                       a7, b6, si6, sj6, flat6i, flat6j, tree_sz);
                if (flat6i < tree_sz && flat6j < tree_sz) {
                    uint32_t a6i = cpu_attrs[6][flat6i], a6j = cpu_attrs[6][flat6j];
                    printf("  DBG attr6[i]=0x%08x  attr6[j]=0x%08x\n", a6i, a6j);
                }
            }
            for (uint32_t s = 0; s < nsol; s++) {
                uint32_t indices[PROOFSIZE];
                if (!extract_solution(cpu_attrs, s, indices, tree_sz)) continue;
                n_extracted++;
                canonical_sort(indices, PARAM_K);
                if (verify_equihash_full(indices, &blake_gen, nonce_idx, n_extracted == 1 ? 1 : 0)) {
                    n_verified++;
                    printf("  SOLUTION nonce=%u:", nonce_idx);
                    for (int i = 0; i < PROOFSIZE; i++) printf(" %08x", indices[i]);
                    printf("\n");
                    printf("  VERIFIED OK\n");
                    printf("Solution");
                    for (int i = 0; i < PROOFSIZE; i++) printf(" %x", indices[i]);
                    printf("\n");
                    valid_solutions++;
                }
            }

            if (show_progress)
                printf("  Extraction: %u candidates → %d extracted (distinct) → %d verified\n",
                       nsol, n_extracted, n_verified);
            /* tree0 and tree1 already released above */
            for (int i = 0; i < 7; i++) clReleaseMemObject(buf_counts[i]);
            for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
            return valid_solutions;
        }
    }

    /* Release the two shared tree buffers */
    clReleaseMemObject(buf_tree0);
    clReleaseMemObject(buf_tree1);
    for (int i = 0; i < 7; i++) clReleaseMemObject(buf_counts[i]);
    for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
    return valid_solutions;
}
int main(int argc, char *argv[]) {
    uint32_t total_nonces = 100000;

    if (argc > 1) {
        total_nonces = atoi(argv[1]);
        if (total_nonces < 1 || total_nonces > 100000000) {
            fprintf(stderr, "Usage: %s [nonces]\n", argv[0]);
            return 1;
        }
    }
    printf("sa-tromp Equihash 192,7 GPU Miner\n");
    printf("NBUCKETS=%u  NSLOTS=%u  BUCKBITS=%u  RESTBITS=%u\n",
           NBUCKETS, NSLOTS, BUCKBITS, RESTBITS);
    printf("Mining nonces: %u\n\n", total_nonces);
    fflush(stdout);

    init_opencl();
    printf("OpenCL ready\n\n"); fflush(stdout);

    uint8_t header[108] = {0};  /* 108-byte header prefix; nonce embedded in mine_batch */

    clock_t overall_start = clock();
    int total_solutions = 0;

    for (uint32_t n = 0; n < total_nonces; n++) {
        int solutions = mine_batch(n, header, 1);
        total_solutions += solutions;
        if (solutions > 0)
            printf("VALID SOLUTION(S) FOUND in nonce %u!\n", n);

        /* Reinit OpenCL between attempts (Beignet stability) */
        if (n + 1 < total_nonces) {
            cleanup_opencl();
            init_opencl();
        }
    }

    double total_time = (double)(clock() - overall_start) / CLOCKS_PER_SEC;
    printf("\n=== DONE: %u nonce(s) in %.2fs, %d valid solution(s) ===\n",
           total_nonces, total_time, total_solutions);

    cleanup_opencl();
    return (total_solutions > 0) ? 0 : 1;
}

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
#define NSLOTS 64
#define SLOTBITS 6    // log2(64)
#define HASHBYTES_STAGE0 24

/* Define htole32 for little-endian conversion if not available */
#ifndef htole32
#define htole32(x) ((uint32_t)(x))
#endif

// Solution extraction structures (from solution_extraction.c)
#include "solution_extraction.c"

// Verification function using Tromp's blake2b
#include "equihash_tromp/blake/blake2.h"

/* Define htole32 for little-endian conversion if not available */
#ifndef htole32
#define htole32(x) ((uint32_t)(x))
#endif

static void eh_genhash(const blake2b_state *ctx, uint32_t idx, uint8_t *hash)
{
    blake2b_state state = *ctx;
    const uint32_t hashes_per_blake = 512 / PARAM_N;
    const uint32_t hash_bytes = PARAM_N / 8;
    uint8_t full_hash[ZCASH_HASH_LEN];
    
    uint32_t leb = htole32(idx / hashes_per_blake);
    blake2b_update(&state, (uchar *)&leb, sizeof(uint32_t));
    blake2b_final(&state, full_hash, ZCASH_HASH_LEN);
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

static int verbose = 0;

static uint32_t eh_verifyrec(const blake2b_state *ctx, uint32_t *indices, uint8_t *hash, int r)
{
    const uint32_t hash_bytes = PARAM_N / 8;

    if (r == 0) {
        eh_genhash(ctx, *indices, hash);
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
    if (!eh_verifyrec(ctx, indices, hash0, r - 1)) {
        if (verbose) printf("  [r=%d] FAIL: left subtree failed\n", r);
        return 0;
    }
    if (!eh_verifyrec(ctx, indices1, hash1, r - 1)) {
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

static uint32_t verify_equihash_full(uint32_t *indices, uint8_t *header, int verbose_mode)
{
    verbose = verbose_mode;
    blake2b_state ctx;
    uint8_t hash[PARAM_N / 8];
    
    char personals[16];
    memcpy(personals + 0, "ZERO_PoW", 8);
    uint32_t le_N = htole32(PARAM_N);
    memcpy(personals + 8, &le_N, 4);
    uint32_t le_K = htole32(PARAM_K);
    memcpy(personals + 12, &le_K, 4);
    
    blake2b_param P;
    memset(&P, 0, sizeof(blake2b_param));
    P.digest_length = ZCASH_HASH_LEN;
    P.fanout = 1;
    P.depth = 1;
    memcpy(P.personal, (const uint8_t *)personals, 16);
    
    blake2b_init_param(&ctx, &P);
    blake2b_update(&ctx, header, ZCASH_BLOCK_HEADER_LEN);

    int result = eh_verifyrec(&ctx, indices, hash, PARAM_K);
    
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
}

void cleanup_opencl(void) {
    for (int i = 0; i < 7; i++) clReleaseKernel(kernels[i]);
    clReleaseKernel(kernel_round0_gen);
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
 * NOTE: solution extraction is deferred to Step 5.
 */
int mine_batch(uint32_t nonce_idx, uint8_t *header, int show_progress) {
    cl_int err;
    /* Beignet safe dispatch size: 2^18 work items per clEnqueueNDRangeKernel */
    const size_t DISPATCH = (size_t)(1 << 18);
    const size_t tree_size = (size_t)NBUCKETS * NSLOTS;

    if (show_progress)
        printf("\n--- Mining nonce %u ---\n", nonce_idx);

    /* ── Phase 1: GPU hash generation ────────────────────────────────────── */
    blake2b_state_t blake_gen;
    zcash_blake2b_init(&blake_gen, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    zcash_blake2b_update(&blake_gen, header, 128, 0);

    cl_mem buf_blake_st = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         8 * sizeof(uint64_t), blake_gen.h, &err);
    check_error(err, "buf_blake_st");

    /* tree0: 1M buckets x 64 slots x 28 bytes = 1.75 GB */
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

    {
        /* 2^24 total work items = 2^25 hashes; split into 64 batches of 2^18 */
        const size_t TOTAL_GEN = (size_t)(1 << 24);
        const size_t LWS = 64;
        clock_t t0 = clock();
        if (show_progress)
            printf("  round0_gen: %zu work items in %zu batches of %zu...\n",
                   TOTAL_GEN, TOTAL_GEN / DISPATCH, DISPATCH);
        for (size_t base = 0; base < TOTAL_GEN; base += DISPATCH) {
            err = clEnqueueNDRangeKernel(queue, kernel_round0_gen, 1,
                                         &base, &DISPATCH, &LWS, 0, NULL, NULL);
            check_error(err, "kernel_round0_gen");
            clFinish(queue);
        }
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
            printf("  tree0: %.2fs  %llu hashes  avg=%.1f/bucket  max=%u  overflow=%u\n",
                   (double)(clock() - t0) / CLOCKS_PER_SEC,
                   (unsigned long long)tot, (double)tot / NBUCKETS, mx, ov);
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

    /* Stage 1: tree0 (stage0_slot_t 28B) -> tree1 (stage1_slot_t 28B GPU)
     *
     * IMPORTANT: solution_extraction.c defines stage1_slot_t with uint64_t
     * attr (32 bytes), but the GPU kernel uses uint32_t attr (28 bytes).
     * Use the GPU's 28-byte size to match the kernel's pointer strides.
     * Fixed properly in Step 5.
     */
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
        /* tree0 no longer needed */
        clReleaseMemObject(buf_tree0);
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

    /* Stages 2-7: progressive alloc/free.
     * sizeof() on the C structs from solution_extraction.c gives the correct
     * GPU sizes for stages 2-7 (all use uint32_t attr). */
    const size_t slot_sz[8] = {
        0,
        STAGE1_GPU_BYTES,
        sizeof(stage2_slot_t),
        sizeof(stage3_slot_t),
        sizeof(stage4_slot_t),
        sizeof(stage5_slot_t),
        sizeof(stage6_slot_t),
        sizeof(stage7_slot_t)
    };

    cl_mem prev = buf_tree1, curr = NULL;
    {
        uint32_t *cnt = calloc(NBUCKETS, sizeof(uint32_t));
        for (int s = 2; s <= 7; s++) {
            curr = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                  tree_size * slot_sz[s], NULL, &err);
            if (err != CL_SUCCESS) {
                printf("  Stage %d: buffer alloc failed (%d), stopping\n", s, err);
                clReleaseMemObject(prev);
                prev = curr = NULL;
                break;
            }

            clSetKernelArg(kernels[s-1], 0, sizeof(cl_mem), &prev);
            clSetKernelArg(kernels[s-1], 1, sizeof(cl_mem), &buf_counts[s-2]);
            clSetKernelArg(kernels[s-1], 2, sizeof(cl_mem), &curr);
            clSetKernelArg(kernels[s-1], 3, sizeof(cl_mem), &buf_counts[s-1]);

            clock_t ts = clock();
            for (size_t base = 0; base < NBUCKETS; base += DISPATCH) {
                size_t count = (base + DISPATCH <= NBUCKETS) ? DISPATCH : (NBUCKETS - base);
                err = clEnqueueNDRangeKernel(queue, kernels[s-1], 1,
                                             &base, &count, NULL, 0, NULL, NULL);
                check_error(err, "stage_kernel");
                clFinish(queue);
            }

            clReleaseMemObject(prev);
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

    /* curr = tree7.  TODO Step 5: solution extraction and verification. */
    if (curr) clReleaseMemObject(curr);
    for (int i = 0; i < 7; i++) clReleaseMemObject(buf_counts[i]);
    return 0;  /* TODO Step 5: return valid_solutions */
}
int main(int argc, char *argv[]) {
    uint32_t total_nonces = 100000;  // Default: 100K nonces
    
    if (argc > 1) {
        total_nonces = atoi(argv[1]);
        if (total_nonces < 1 || total_nonces > 100000000) {
            fprintf(stderr, "Usage: %s [nonces]\n", argv[0]);
            fprintf(stderr, "Nonces must be between 1 and 100,000,000\n");
            return 1;
        }
    }
    
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║         sa-tromp: Equihash 192,7 GPU Miner              ║\n");
    printf("║    Local verification before pool integration testing    ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");
    
    printf("Configuration:\n");
    printf("  Algorithm: Equihash 192,7 (source-bucket GPU kernels)\n");
    printf("  NBUCKETS=%u  NSLOTS=%u  BUCKBITS=%u  RESTBITS=%u\n",
           NBUCKETS, NSLOTS, BUCKBITS, RESTBITS);
    printf("  Mining nonces to attempt: %u\n\n", total_nonces);

    printf("Initializing OpenCL...\n");
    init_opencl();
    printf("OK OpenCL ready\n\n");

    uint8_t header[ZCASH_BLOCK_HEADER_LEN] = {0};
    memcpy(header, "test_block_header_data_192_7", 28);
    printf("Test header: \"test_block_header_data_192_7\"\n");

    clock_t overall_start = clock();
    int total_solutions = 0;

    for (uint32_t n = 0; n < total_nonces; n++) {
        printf("\n=== Mining nonce %u/%u ===\n", n + 1, total_nonces);
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

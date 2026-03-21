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

#define _DEFAULT_SOURCE   /* for usleep */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <endian.h>
#include <unistd.h>
#include <signal.h>
#include <CL/cl.h>

// Type definitions needed by param.h
typedef uint8_t uchar;
typedef uint32_t uint;

#include <pthread.h>
#include "blake.h"
#include "param.h"
#include "_kernel.h"
#include "compress_sol.h"
#include "stratum.h"
#include "sha256.h"

#define PARAM_N 192
#define PARAM_K 7
#define ZCASH_HASH_LEN 48
#define RESTBITS 4
#define BUCKBITS (24-RESTBITS)
#define NBUCKETS (1<<BUCKBITS)  // 1M buckets
#define NSLOTS 64
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
    (void)nonce;  /* nonce is baked into ctx (blake state after block1); not in block2 */
    /* Pool-compatible block 2 layout:
     *   m[0] = 0 (nonce absorbed in block1 state h — nonce at headernonce bytes 108-127)
     *   m[1] high32 = g (blake-call index); low32 from buffered nonce bytes 128-131 = 0
     * ctx = h after block1 (bytes 0-127, includes nonce bytes 108-127). bytes=128. */
    blake2b_state_t st = *ctx;
    const uint32_t hashes_per_blake = 512 / PARAM_N;   /* = 2 */
    const uint32_t hash_bytes = PARAM_N / 8;            /* = 24 */
    uint8_t full_hash[ZCASH_HASH_LEN];
    uint64_t message[16] = {0};
    uint32_t g = idx / hashes_per_blake;
    message[0] = 0;                        /* m[0] = 0 (nonce in block1, bytes 128-131 = 0) */
    message[1] = (uint64_t)g << 32;        /* m[1] high32 = blake-call index g */
    st.bytes = 128;                        /* initial state was built after block1 (128 bytes) */
    zcash_blake2b_update(&st, (const uint8_t *)message, 2 * sizeof(uint64_t), 1);
    zcash_blake2b_final(&st, full_hash, ZCASH_HASH_LEN);
    memcpy(hash, full_hash + (idx % hashes_per_blake) * hash_bytes, hash_bytes);
}

static int verbose = 0;
volatile int g_cancel_mining = 0;
volatile int g_shutdown = 0;
volatile int g_disconnected = 0;
static void sigint_handler(int s) { (void)s; g_shutdown = 1; g_cancel_mining = 1; }

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
static int g_platform_idx = 0;
cl_platform_id platform;
cl_device_id device;
cl_context context;
cl_command_queue queue;
cl_program program;
cl_kernel kernels[7];
cl_kernel kernel_round0_gen;
cl_kernel kernel_extract_attrs_k;

/* OpenCL driver detection — set once in init_opencl(), used throughout */
typedef enum { DRIVER_BEIGNET, DRIVER_NEO } opencl_driver_t;
static opencl_driver_t g_driver = DRIVER_BEIGNET;  /* safe default */

/* Persistent GPU buffers — allocated once in init_opencl, reused every nonce.
 * Avoids Beignet OOM: clReleaseMemObject doesn't actually free shared RAM. */
static cl_mem g_buf_tree0   = NULL;
static cl_mem g_buf_tree1   = NULL;
static cl_mem g_buf_t0_cnt  = NULL;
static cl_mem g_buf_counts[7];
static cl_mem g_buf_extract = NULL;  /* persistent attr extraction buffer — 256 MB */

void check_error(cl_int err, const char *operation) {
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Error during %s: %d\n", operation, err);
        exit(1);
    }
}

void init_opencl(void) {
    /* Static counter to force unique builds (workaround for Beignet kernel caching).
     * NEO caches correctly — no need to bust. */
    static int build_id = 0;
    if (g_driver == DRIVER_BEIGNET) build_id++;
    
    cl_int err;
    
    cl_uint num_platforms = 0;
    clGetPlatformIDs(0, NULL, &num_platforms);
    if (num_platforms == 0) { fprintf(stderr, "No OpenCL platforms found\n"); exit(1); }
    cl_platform_id platforms[num_platforms];
    err = clGetPlatformIDs(num_platforms, platforms, NULL);
    check_error(err, "clGetPlatformIDs");
    if (g_platform_idx < 0 || (cl_uint)g_platform_idx >= num_platforms) {
        fprintf(stderr, "Platform index %d out of range (0..%u)\n", g_platform_idx, num_platforms-1);
        for (cl_uint p = 0; p < num_platforms; p++) {
            char name[128] = {0};
            clGetPlatformInfo(platforms[p], CL_PLATFORM_NAME, sizeof(name), name, NULL);
            fprintf(stderr, "  [%u] %s\n", p, name);
        }
        exit(1);
    }
    platform = platforms[g_platform_idx];
    { char name[128] = {0};
      char version[128] = {0};
      clGetPlatformInfo(platform, CL_PLATFORM_NAME,    sizeof(name),    name,    NULL);
      clGetPlatformInfo(platform, CL_PLATFORM_VERSION, sizeof(version), version, NULL);
      /* Detect driver: Beignet identifies itself in the platform name */
      /* Build lowercase of name+version for Beignet detection.
       * Beignet reports: name="Intel Gen OCL Driver", version="OpenCL 2.0 beignet 1.x"
       * NEO reports:     name="Intel(R) OpenCL HD Graphics", version="OpenCL 3.0 ..." */
      char combined[256];
      snprintf(combined, sizeof(combined), "%s %s", name, version);
      char name_lower[256];
      for (int i = 0; combined[i] && i < 255; i++)
          name_lower[i] = (combined[i] >= 'A' && combined[i] <= 'Z') ? combined[i] + 32 : combined[i];
      name_lower[strlen(combined)] = '\0';
      if (strstr(name_lower, "beignet")) {
          g_driver = DRIVER_BEIGNET;
          fprintf(stderr, "[opencl] Beignet (%s)\n", version);
      } else {
          g_driver = DRIVER_NEO;
          fprintf(stderr, "[opencl] Intel NEO (%s)\n", version);
      }
      (void)name; }

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
    if (g_driver == DRIVER_NEO)
        snprintf(build_opts, sizeof(build_opts), "-DPARAM_N=192 -DPARAM_K=7 -DBUILD_ID=%d -cl-fast-relaxed-math -cl-mad-enable", build_id);
    else
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

    /* Allocate persistent GPU buffers — reused across all nonces */
    {
        const size_t tree_size = (size_t)NBUCKETS * NSLOTS;
        g_buf_tree0 = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                     tree_size * 28, NULL, &err);
        check_error(err, "g_buf_tree0");
        g_buf_tree1 = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                     tree_size * 28, NULL, &err);
        check_error(err, "g_buf_tree1");
        g_buf_t0_cnt = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                      NBUCKETS * sizeof(uint32_t), NULL, &err);
        check_error(err, "g_buf_t0_cnt");
        for (int i = 0; i < 7; i++) {
            g_buf_counts[i] = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                             NBUCKETS * sizeof(uint32_t), NULL, &err);
            check_error(err, "g_buf_counts");
        }
        /* Persistent attr extraction buffer — reused every stage instead of alloc/free.
         * Size: tree_size * 4 bytes = 256 MB. One-time cost at init. */
        g_buf_extract = clCreateBuffer(context, CL_MEM_READ_WRITE,
                                       tree_size * sizeof(uint32_t), NULL, &err);
        check_error(err, "g_buf_extract");
    }
}

void cleanup_opencl(void) {
    clReleaseMemObject(g_buf_tree0);
    clReleaseMemObject(g_buf_tree1);
    clReleaseMemObject(g_buf_t0_cnt);
    for (int i = 0; i < 7; i++) clReleaseMemObject(g_buf_counts[i]);
    clReleaseMemObject(g_buf_extract);
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
int mine_batch(uint32_t nonce_idx, const uint8_t *nonce32, uint8_t *header, int show_progress,
               void (*solution_cb)(const uint32_t *indices, uint32_t nonce_idx, void *ud),
               void *ud) {
    cl_int err;
    /* Dispatch batch size per clEnqueueNDRangeKernel.
     * Beignet: 2^18 — larger dispatches hang the GPU (no watchdog).
     * NEO: 2^20 — no hang limit; 4x fewer kernel launches per stage. */
    const size_t DISPATCH = (g_driver == DRIVER_BEIGNET) ? (size_t)(1 << 18) : (size_t)(1 << 20);
    const size_t tree_size = (size_t)NBUCKETS * NSLOTS;
    uint32_t *cpu_attrs[8] = {NULL};
    /* GPU slot sizes with 4-byte alignment padding (matches Beignet OpenCL C sizeof).
     * Stages 2,3,5,6,7 have uchar hash[] that doesn't fill to 4-byte boundary,
     * so Beignet adds tail padding. */
    const size_t _slot_sz[8] = {28,28,24,20,16,16,12,8};

    if (show_progress)
        printf("\n--- Mining nonce %u ---\n", nonce_idx);

    /* ── Phase 1: GPU hash generation ────────────────────────────────────── */
    /* Pool-compatible convention: 140-byte headernonce, nonce at bytes 108-139.
     * blake2b_update(headernonce, 140) compresses bytes 0-127 as block1 (t=128)
     * — nonce bytes 108-127 are IN block1 (state is nonce-dependent).
     * Buffered bytes 128-139 (last 12 of nonce) + per-index g = 16-byte block2, t=144.
     * GPU block2 = {0(×12), g(×4)}: word0=0, word1=g<<32 (nonce absorbed in state h). */
    uint8_t headernonce[140] = {0};
    memcpy(headernonce, header, 108);  /* header is ≤108 bytes; rest zero */
    if (nonce32) {
        /* Stratum mode: pool nonce is nonce1_bytes || nonce2_bytes (32 bytes, big-endian) */
        memcpy(headernonce + 108, nonce32, 32);
    } else {
        /* Solo mode: simple incrementing nonce at bytes 108-111 LE */
        ((uint32_t *)headernonce)[27] = htole32(nonce_idx);
    }

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
    blake2b_update(&tromp_st, headernonce, 140);
    /* After 140-byte update: bytes 0-127 compressed (t=128), bytes 128-139 buffered.
     * tromp_st.h = h after block1 (what GPU needs). */

    blake2b_state_t blake_gen;
    memcpy(blake_gen.h, tromp_st.h, 8 * sizeof(uint64_t));
    blake_gen.bytes = 128;
    cl_mem buf_blake_st = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         8 * sizeof(uint64_t), blake_gen.h, &err);
    check_error(err, "buf_blake_st");

    /* Use persistent global buffers — zero them for this nonce */
    cl_mem buf_tree0 = g_buf_tree0;
    cl_mem buf_t0_cnt = g_buf_t0_cnt;
    { uint32_t zero = 0;
      clEnqueueFillBuffer(queue, buf_t0_cnt, &zero, sizeof(zero),
                          0, NBUCKETS * sizeof(uint32_t), 0, NULL, NULL);
      clFinish(queue); }

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
            if (g_cancel_mining) {
                for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
                clReleaseMemObject(buf_blake_st);
                return -1;
            }
        }
        /* Extract attrs[0]: use persistent g_buf_extract (no alloc/free per nonce) */
        { uint32_t stride = (uint32_t)_slot_sz[0];
          clSetKernelArg(kernel_extract_attrs_k, 0, sizeof(cl_mem), &buf_tree0);
          clSetKernelArg(kernel_extract_attrs_k, 1, sizeof(uint32_t), &stride);
          clSetKernelArg(kernel_extract_attrs_k, 2, sizeof(cl_mem), &g_buf_extract);
          for (size_t base = 0; base < tree_size; base += DISPATCH) {
              size_t d = (base + DISPATCH <= tree_size) ? DISPATCH : (tree_size - base);
              uint32_t base_u = (uint32_t)base;
              clSetKernelArg(kernel_extract_attrs_k, 3, sizeof(uint32_t), &base_u);
              clEnqueueNDRangeKernel(queue, kernel_extract_attrs_k, 1, NULL, &d, NULL, 0, NULL, NULL);
              clFinish(queue);
          }
          cpu_attrs[0] = malloc(tree_size * sizeof(uint32_t));
          if (!cpu_attrs[0]) { fprintf(stderr, "OOM cpu_attrs[0]\n"); exit(1); }
          clEnqueueReadBuffer(queue, g_buf_extract, CL_TRUE, 0,
                              tree_size * sizeof(uint32_t), cpu_attrs[0], 0, NULL, NULL); }
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

    /* Use persistent global count buffers — zero them for this nonce */
    cl_mem *buf_counts = g_buf_counts;
    { uint32_t zero = 0;
      for (int i = 0; i < 7; i++) {
          clEnqueueFillBuffer(queue, buf_counts[i], &zero, sizeof(zero),
                              0, NBUCKETS * sizeof(uint32_t), 0, NULL, NULL);
      }
      clFinish(queue); }

    /* Use persistent tree1 buffer */
    cl_mem buf_tree1 = g_buf_tree1;

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
            if (g_cancel_mining) {
                for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
                return -1;
            }
        }
        /* Extract attrs[1]: use persistent g_buf_extract */
        { uint32_t stride = (uint32_t)_slot_sz[1];
          clSetKernelArg(kernel_extract_attrs_k, 0, sizeof(cl_mem), &buf_tree1);
          clSetKernelArg(kernel_extract_attrs_k, 1, sizeof(uint32_t), &stride);
          clSetKernelArg(kernel_extract_attrs_k, 2, sizeof(cl_mem), &g_buf_extract);
          for (size_t base = 0; base < tree_size; base += DISPATCH) {
              size_t d = (base + DISPATCH <= tree_size) ? DISPATCH : (tree_size - base);
              uint32_t base_u = (uint32_t)base;
              clSetKernelArg(kernel_extract_attrs_k, 3, sizeof(uint32_t), &base_u);
              clEnqueueNDRangeKernel(queue, kernel_extract_attrs_k, 1, NULL, &d, NULL, 0, NULL, NULL);
              clFinish(queue);
          }
          cpu_attrs[1] = malloc(tree_size * sizeof(uint32_t));
          if (!cpu_attrs[1]) { fprintf(stderr, "OOM cpu_attrs[1]\n"); exit(1); }
          clEnqueueReadBuffer(queue, g_buf_extract, CL_TRUE, 0,
                              tree_size * sizeof(uint32_t), cpu_attrs[1], 0, NULL, NULL); }
        /* tree0 and buf_t0_cnt are persistent globals — do NOT release here */

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
            int cancelled = 0;
            for (size_t base = 0; base < NBUCKETS; base += disp) {
                size_t count = (base + disp <= NBUCKETS) ? disp : (NBUCKETS - base);
                err = clEnqueueNDRangeKernel(queue, kernels[s-1], 1,
                                             &base, &count, NULL, 0, NULL, NULL);
                check_error(err, "stage_kernel");
                clFinish(queue);
                if (g_cancel_mining) { cancelled = 1; break; }
            }
            if (cancelled) {
                for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
                free(cnt);
                return -1;
            }

            /* Extract attrs[s]: use persistent g_buf_extract */
            { uint32_t stride = (uint32_t)_slot_sz[s];
              clSetKernelArg(kernel_extract_attrs_k, 0, sizeof(cl_mem), &curr);
              clSetKernelArg(kernel_extract_attrs_k, 1, sizeof(uint32_t), &stride);
              clSetKernelArg(kernel_extract_attrs_k, 2, sizeof(cl_mem), &g_buf_extract);
              for (size_t base = 0; base < tree_size; base += DISPATCH) {
                  size_t d = (base + DISPATCH <= tree_size) ? DISPATCH : (tree_size - base);
                  uint32_t base_u = (uint32_t)base;
                  clSetKernelArg(kernel_extract_attrs_k, 3, sizeof(uint32_t), &base_u);
                  clEnqueueNDRangeKernel(queue, kernel_extract_attrs_k, 1, NULL, &d, NULL, 0, NULL, NULL);
                  clFinish(queue);
              }
              cpu_attrs[s] = malloc(tree_size * sizeof(uint32_t));
              if (!cpu_attrs[s]) { fprintf(stderr, "OOM cpu_attrs[%d]\n", s); exit(1); }
              clEnqueueReadBuffer(queue, g_buf_extract, CL_TRUE, 0,
                                  tree_size * sizeof(uint32_t), cpu_attrs[s], 0, NULL, NULL); }
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


        if (nsol > 0) {
            /* cpu_attrs[0..7] already populated during pipeline (immediate readback) */
            uint32_t tree_sz = (uint32_t)tree_size;
            int n_extracted = 0, n_verified = 0;
            for (uint32_t s = 0; s < nsol; s++) {
                uint32_t indices[PROOFSIZE];
                if (!extract_solution(cpu_attrs, s, indices, tree_sz)) continue;
                n_extracted++;
                canonical_sort(indices, PARAM_K);
                if (verify_equihash_full(indices, &blake_gen, nonce_idx, 0)) {
                    n_verified++;
                    valid_solutions++;
                    if (solution_cb) {
                        solution_cb(indices, nonce_idx, ud);
                    }
                }
            }

            /* buf_tree0/tree1/counts are persistent globals — do NOT release here */
            for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
            return valid_solutions;
        }
    }

    /* buf_tree0/tree1/counts are persistent globals — do NOT release here */
    for (int r = 0; r < 8; r++) { free(cpu_attrs[r]); cpu_attrs[r] = NULL; }
    return valid_solutions;
}

/* ── Stratum pool mining ─────────────────────────────────────────────────── */

typedef struct {
    stratum_ctx_t *ctx;
    char           job_id[STRATUM_JOB_ID_LEN];
    char           ntime[STRATUM_NTIME_LEN];
    uint32_t       nonce2;
    uint8_t        headernonce[140];   /* for sha256d target check */
} stratum_cb_arg_t;

/* Returns 1 if sha256d(headernonce[0..139]) <= target[0..31] (big-endian), 0 otherwise.
 * If hash_out != NULL, writes the 32-byte sha256d result to it. */
static int sha256d_check_target(const uint8_t *headernonce, const uint8_t *target,
                                uint8_t *hash_out)
{
    uint8_t h1[SHA256_DIGEST_SIZE], h2[SHA256_DIGEST_SIZE];
    Sha256_Onestep(headernonce, 140, h1);
    Sha256_Onestep(h1, SHA256_DIGEST_SIZE, h2);
    if (hash_out) memcpy(hash_out, h2, SHA256_DIGEST_SIZE);
    for (int i = 0; i < 32; i++) {
        if (h2[i] < target[i]) return 1;
        if (h2[i] > target[i]) return 0;
    }
    return 1;  /* equal: exactly meets target */
}

/* Compute share difficulty matching s-nomp formula:
 * diff1 = 0x0007ffff...ff (252-bit value), shareDiff = diff1 / hash_as_bignum.
 * We approximate using the leading 8 bytes of the hash as a double. */
static double share_diff(const uint8_t *hash)
{
    /* diff1 upper 8 bytes: 0x0007ffffffffffff */
    static const double diff1_hi = (double)0x0007ffffffffffffULL;
    /* hash as double using leading 8 bytes (big-endian) */
    uint64_t hash_hi = 0;
    for (int i = 0; i < 8; i++)
        hash_hi = (hash_hi << 8) | hash[i];
    if (hash_hi == 0) return 0.0;
    return diff1_hi / (double)hash_hi;
}

static void stratum_solution_cb(const uint32_t *indices, uint32_t nonce_idx,
                                 void *ud)
{
    (void)nonce_idx;
    stratum_cb_arg_t *a = (stratum_cb_arg_t *)ud;

    uint8_t compressed[COMPRESSED_SOL_SIZE];
    if (get_minimal_from_indices(indices, COMPRESS_PROOFSIZE,
                                 compressed, sizeof(compressed)) != 0) {
        fprintf(stderr, "[stratum] compress failed\n");
        return;
    }

    /* Pool expects Bitcoin-serialized vector: compact size prefix + solution bytes.
     * 400 bytes → compact size = fd 90 01 (3 bytes) → total 806 hex chars. */
    char sol_hex[6 + COMPRESSED_SOL_SIZE * 2 + 1];
    strcpy(sol_hex, "fd9001");
    for (int i = 0; i < COMPRESSED_SOL_SIZE; i++)
        sprintf(sol_hex + 6 + i * 2, "%02x", compressed[i]);
    sol_hex[6 + COMPRESSED_SOL_SIZE * 2] = '\0';

    /* Only submit if sha256d(headernonce) meets pool difficulty target */
    uint8_t share_hash[32];
    {
        uint8_t target[32];
        pthread_mutex_lock(&a->ctx->job_mutex);
        memcpy(target, a->ctx->target, 32);
        pthread_mutex_unlock(&a->ctx->job_mutex);
        if (!sha256d_check_target(a->headernonce, target, share_hash)) {
            return;
        }
    }

    stratum_submit_diff(a->ctx, a->job_id, a->ntime, a->nonce2, sol_hex, share_diff(share_hash));
}

static void *stratum_recv_thread(void *arg)
{
    stratum_ctx_t *ctx = (stratum_ctx_t *)arg;
    while (!g_shutdown) {
        if (stratum_recv_line(ctx) < 0) {
            fprintf(stderr, "[stratum] Disconnected\n");
            g_disconnected = 1;
            g_cancel_mining = 1;
            break;
        }
        /* If a clean new job arrived, cancel current mining batch */
        if (ctx->cancel)
            g_cancel_mining = 1;
    }
    return NULL;
}


static void run_stratum_mode(const char *host, const char *port,
                             const char *user, const char *pass)
{
    init_opencl();

    stratum_ctx_t ctx;
    stratum_init(&ctx, host, port, user, pass);

    struct timespec rate_start;
    clock_gettime(CLOCK_MONOTONIC, &rate_start);
    int rate_solutions = 0;
    int rate_nonces = 0;

reconnect:
    if (g_shutdown) goto done;

    if (stratum_connect(&ctx) < 0) {
        fprintf(stderr, "[stratum] Failed to connect, retrying in 5s...\n");
        sleep(5);
        goto reconnect;
    }

    g_disconnected = 0;
    g_cancel_mining = 0;

    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, stratum_recv_thread, &ctx);

    uint32_t nonce2 = 0;
    while (!g_shutdown && !g_disconnected) {
        stratum_job_t job;
        if (!stratum_get_job(&ctx, &job)) {
            usleep(100000);
            continue;
        }

        /* Build 32-byte pool nonce: nonce1_bytes || nonce2_bytes (big-endian, as pool does) */
        uint8_t nonce32[32] = {0};
        memcpy(nonce32, ctx.nonce1, ctx.nonce1_len);
        /* nonce2 stored little-endian after nonce1 (matches hex submitted to pool) */
        int n2off = ctx.nonce1_len;
        nonce32[n2off + 0] = (nonce2      ) & 0xFF;
        nonce32[n2off + 1] = (nonce2 >>  8) & 0xFF;
        nonce32[n2off + 2] = (nonce2 >> 16) & 0xFF;
        nonce32[n2off + 3] = (nonce2 >> 24) & 0xFF;
        /* remaining bytes stay 0 */

        /* nonce_idx for GPU kernel: first 4 bytes of nonce32 as uint32 LE */
        uint32_t nonce_val = ((uint32_t)nonce32[0] << 24) | ((uint32_t)nonce32[1] << 16) |
                             ((uint32_t)nonce32[2] <<  8) |  (uint32_t)nonce32[3];

        stratum_cb_arg_t cb_arg;
        cb_arg.ctx   = &ctx;
        cb_arg.nonce2 = nonce2;
        strncpy(cb_arg.job_id, job.job_id, sizeof(cb_arg.job_id) - 1);
        cb_arg.job_id[sizeof(cb_arg.job_id) - 1] = '\0';
        strncpy(cb_arg.ntime,  job.ntime,  sizeof(cb_arg.ntime) - 1);
        cb_arg.ntime[sizeof(cb_arg.ntime) - 1] = '\0';

        /* headernonce for sha256d target check: header + full 32-byte pool nonce */
        memcpy(cb_arg.headernonce, job.header, 108);
        memcpy(cb_arg.headernonce + 108, nonce32, 32);

        g_cancel_mining = 0;
        ctx.cancel = 0;  /* consumed — will be re-set if another clean job arrives */
        clFinish(queue);  /* flush any pending OpenCL ops before starting new batch */
        int r = mine_batch(nonce_val, nonce32, job.header, 0,
                           stratum_solution_cb, &cb_arg);
        if (r == -1) {
            /* Interrupted by new job or disconnect — reset nonce2 for fresh job */
            nonce2 = 0;
            continue;
        }
        rate_solutions += r;
        nonce2++;  /* advance nonce2 so next batch has a different nonce */
        rate_nonces++;
        if (rate_nonces % 10 == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - rate_start.tv_sec) +
                             (now.tv_nsec - rate_start.tv_nsec) / 1e9;
            printf("[pool] Rate: %.4f sol/s (%d solutions in %.1fs, %d nonces)\n",
                   elapsed > 0 ? rate_solutions / elapsed : 0.0,
                   rate_solutions, elapsed, rate_nonces);
            fflush(stdout);
        }
    }

    stratum_disconnect(&ctx);
    pthread_join(recv_tid, NULL);

    if (g_disconnected && !g_shutdown) {
        fprintf(stderr, "[stratum] Reconnecting in 5s...\n");
        sleep(5);
        goto reconnect;
    }

done:
    cleanup_opencl();
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);
    uint32_t total_nonces = 100000;
    uint32_t start_nonce = 0;
    char stratum_url[256] = {0};
    char stratum_user[256] = {0};
    char stratum_pass[256] = "x";

    /* Parse args: [-p platform] [-o stratum+tcp://host:port] [-u user] [-P pass] [nonces] */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_platform_idx = atoi(argv[i + 1]); i += 2;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            strncpy(stratum_url, argv[i + 1], sizeof(stratum_url) - 1); i += 2;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            strncpy(stratum_user, argv[i + 1], sizeof(stratum_user) - 1); i += 2;
        } else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
            strncpy(stratum_pass, argv[i + 1], sizeof(stratum_pass) - 1); i += 2;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            start_nonce = (uint32_t)strtoul(argv[i + 1], NULL, 0); i += 2;
        } else {
            total_nonces = (uint32_t)atoi(argv[i]); i++;
        }
    }

    printf("silentarmy-tromp Equihash 192,7 Intel OpenCL Miner\n");
    fflush(stdout);

    /* Stratum pool mode */
    if (stratum_url[0]) {
        /* Parse stratum+tcp://host:port */
        const char *url = stratum_url;
        if (strncmp(url, "stratum+tcp://", 14) == 0) url += 14;
        char host[256] = {0};
        char port[16]  = "2222";
        const char *colon = strrchr(url, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - url);
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, url, hlen);
            host[hlen] = '\0';
            snprintf(port, sizeof(port), "%s", colon + 1);
        } else {
            snprintf(host, sizeof(host), "%s", url);
        }
        if (!stratum_user[0]) {
            fprintf(stderr, "Usage: %s -o stratum+tcp://host:port -u user [-P pass]\n", argv[0]);
            return 1;
        }
        printf("Pool: %s:%s  User: %s\n", host, port, stratum_user);
        run_stratum_mode(host, port, stratum_user, stratum_pass);
        return 0;
    }

    /* Solo mode */
    init_opencl();
    printf("Mining %u nonce(s)\n\n", total_nonces);
    fflush(stdout);

    uint8_t header[108] = {0};  /* 108-byte header prefix; nonce embedded in mine_batch */

    clock_t overall_start = clock();
    int total_solutions = 0;

    for (uint32_t n = start_nonce; n < start_nonce + total_nonces; n++) {
        int solutions = mine_batch(n, NULL, header, 0, NULL, NULL);
        total_solutions += solutions;

        /* Reinit OpenCL between attempts (Beignet stability) */
        /* NOTE: disabled — cleanup_opencl hangs on Beignet after multi-nonce runs */
        /* if (n + 1 < total_nonces) { cleanup_opencl(); init_opencl(); } */
    }

    double total_time = (double)(clock() - overall_start) / CLOCKS_PER_SEC;
    printf("\n=== DONE: %u nonce(s) in %.2fs, %d valid solution(s), %.4f sol/s ===\n",
           total_nonces, total_time, total_solutions,
           total_time > 0 ? total_solutions / total_time : 0.0);

    cleanup_opencl();
    return (total_solutions > 0) ? 0 : 1;
}

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
#define NBUCKETS (1<<20)
#define NSLOTS 32
#define SLOTBITS 5
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

void check_error(cl_int err, const char *operation) {
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Error during %s: %d\n", operation, err);
        exit(1);
    }
}

void init_opencl(void) {
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
    
    err = clBuildProgram(program, 1, &device, "-DPARAM_N=192 -DPARAM_K=7", NULL, NULL);
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
}

void cleanup_opencl(void) {
    for (int i = 0; i < 7; i++) clReleaseKernel(kernels[i]);
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

int mine_batch(uint32_t nonces, uint8_t *header, uint32_t nonce_offset, int show_progress) {
    uint32_t num_hashes = nonces * 2;
    
    if (show_progress) {
        printf("\n--- Mining batch: %u nonces (%u hashes) starting at nonce %u ---\n", 
               nonces, num_hashes, nonce_offset);
    }
    
    // Generate Round 0 hashes
    unsigned char *round0_hashes = malloc(num_hashes * HASHBYTES_STAGE0);
    if (!round0_hashes) {
        fprintf(stderr, "Failed to allocate Round 0 hashes\n");
        return 0;
    }
    generate_round0_hashes(round0_hashes, nonces, header, nonce_offset);
    
    // Debug: print first hash bytes
    printf("  [DEBUG] First hash bytes: %02x %02x %02x %02x\n", 
           round0_hashes[0], round0_hashes[1], round0_hashes[2], round0_hashes[3]);
    
    // Allocate GPU buffers
    cl_int err;
    size_t tree_size = NBUCKETS * NSLOTS;
    
    cl_mem buf_round0 = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       num_hashes * HASHBYTES_STAGE0, round0_hashes, &err);
    check_error(err, "buf_round0");
    clFinish(queue);  // Ensure buffer upload completes
    
    cl_mem buf_tree1 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage1_slot_t), NULL, &err);
    check_error(err, "buf_tree1");
    cl_mem buf_tree2 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage2_slot_t), NULL, &err);
    check_error(err, "buf_tree2");
    cl_mem buf_tree3 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage3_slot_t), NULL, &err);
    check_error(err, "buf_tree3");
    cl_mem buf_tree4 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage4_slot_t), NULL, &err);
    check_error(err, "buf_tree4");
    cl_mem buf_tree5 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage5_slot_t), NULL, &err);
    check_error(err, "buf_tree5");
    cl_mem buf_tree6 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage6_slot_t), NULL, &err);
    check_error(err, "buf_tree6");
    cl_mem buf_tree7 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage7_slot_t), NULL, &err);
    check_error(err, "buf_tree7");
    
    cl_mem buf_counts[7];
    for (int i = 0; i < 7; i++) {
        buf_counts[i] = clCreateBuffer(context, CL_MEM_READ_WRITE, NBUCKETS * sizeof(uint32_t), NULL, &err);
        check_error(err, "buf_counts");
    }
    
    // Initialize count buffers to zero (critical for correct collision counting)
    uint32_t *zeros = calloc(NBUCKETS, sizeof(uint32_t));
    for (int i = 0; i < 7; i++) {
        clEnqueueWriteBuffer(queue, buf_counts[i], CL_TRUE, 0, NBUCKETS * sizeof(uint32_t), zeros, 0, NULL, NULL);
    }
    free(zeros);
    
    // Run all 7 stages
    size_t global_work_size = NBUCKETS;
    uint32_t *slot_counts = calloc(NBUCKETS, sizeof(uint32_t));
    
    if (show_progress) {
        printf("Running GPU stages 1-7...\n");
    }
    
    clock_t start = clock();
    
    // Stage 1
    clSetKernelArg(kernels[0], 0, sizeof(cl_mem), &buf_round0);
    clSetKernelArg(kernels[0], 1, sizeof(cl_mem), &buf_tree1);
    clSetKernelArg(kernels[0], 2, sizeof(cl_mem), &buf_counts[0]);
    clSetKernelArg(kernels[0], 3, sizeof(uint32_t), &num_hashes);
    err = clEnqueueNDRangeKernel(queue, kernels[0], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "stage1");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[0], CL_TRUE, 0, NBUCKETS * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    uint32_t total = 0;
    for (uint32_t i = 0; i < NBUCKETS; i++) total += slot_counts[i];
    if (show_progress) printf("  Stage 1: %u collisions\n", total);
    
    // Stages 2-7
    cl_mem stage_inputs[] = {buf_tree1, buf_tree2, buf_tree3, buf_tree4, buf_tree5, buf_tree6};
    cl_mem stage_outputs[] = {buf_tree2, buf_tree3, buf_tree4, buf_tree5, buf_tree6, buf_tree7};
    
    for (int stage = 1; stage < 7; stage++) {
        clSetKernelArg(kernels[stage], 0, sizeof(cl_mem), &stage_inputs[stage-1]);
        clSetKernelArg(kernels[stage], 1, sizeof(cl_mem), &buf_counts[stage-1]);
        clSetKernelArg(kernels[stage], 2, sizeof(cl_mem), &stage_outputs[stage-1]);
        clSetKernelArg(kernels[stage], 3, sizeof(cl_mem), &buf_counts[stage]);
        err = clEnqueueNDRangeKernel(queue, kernels[stage], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
        check_error(err, "stage_kernel");
        clFinish(queue);
        
        clEnqueueReadBuffer(queue, buf_counts[stage], CL_TRUE, 0, NBUCKETS * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
        total = 0;
        for (uint32_t i = 0; i < NBUCKETS; i++) total += slot_counts[i];
        if (show_progress) {
            printf("  Stage %d: %u %s\n", stage + 1, total, 
                   stage == 6 ? "solution candidates" : "collisions");
        }
    }
    
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    
    if (show_progress) {
        printf("GPU mining complete: %.2f seconds\n", elapsed);
    }
    
    int valid_solutions = 0;
    
    if (total == 0) {
        if (show_progress) {
            printf("\nNo solution candidates found (need more nonces)\n");
        }
        goto cleanup;
    }
    
    if (show_progress) {
        printf("\nExtracting and verifying %u candidate(s)...\n", total);
    }
    
    // Read all tree buffers from GPU
    tree_store_t trees;
    trees.trees1_stage1 = malloc(tree_size * sizeof(stage1_slot_t));
    trees.trees0_stage2 = malloc(tree_size * sizeof(stage2_slot_t));
    trees.trees1_stage3 = malloc(tree_size * sizeof(stage3_slot_t));
    trees.trees0_stage4 = malloc(tree_size * sizeof(stage4_slot_t));
    trees.trees1_stage5 = malloc(tree_size * sizeof(stage5_slot_t));
    trees.trees0_stage6 = malloc(tree_size * sizeof(stage6_slot_t));
    trees.trees1_stage7 = malloc(tree_size * sizeof(stage7_slot_t));
    
    clEnqueueReadBuffer(queue, buf_tree1, CL_TRUE, 0, tree_size * sizeof(stage1_slot_t), trees.trees1_stage1, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, buf_tree2, CL_TRUE, 0, tree_size * sizeof(stage2_slot_t), trees.trees0_stage2, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, buf_tree3, CL_TRUE, 0, tree_size * sizeof(stage3_slot_t), trees.trees1_stage3, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, buf_tree4, CL_TRUE, 0, tree_size * sizeof(stage4_slot_t), trees.trees0_stage4, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, buf_tree5, CL_TRUE, 0, tree_size * sizeof(stage5_slot_t), trees.trees1_stage5, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, buf_tree6, CL_TRUE, 0, tree_size * sizeof(stage6_slot_t), trees.trees0_stage6, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, buf_tree7, CL_TRUE, 0, tree_size * sizeof(stage7_slot_t), trees.trees1_stage7, 0, NULL, NULL);
    
    // Extract and verify solutions
    uint32_t candidates_checked = 0;
    
    for (uint32_t bucketid = 0; bucketid < NBUCKETS && candidates_checked < total; bucketid++) {
        uint32_t bucket_count = slot_counts[bucketid];
        if (bucket_count == 0) continue;
        
        stage7_slot_t *bucket = &trees.trees1_stage7[bucketid * NSLOTS];
        
        for (uint32_t slotid = 0; slotid < bucket_count && slotid < NSLOTS; slotid++) {
            candidates_checked++;
            uint32_t solution_indices[128];
            
            if (!extract_solution(&trees, bucket[slotid].attr, solution_indices)) {
                if (show_progress) {
                    printf("  Candidate %u: ✗ Duplicate indices (skipped)\n", candidates_checked);
                }
                continue;
            }
            
            // Verify with full Equihash verification
            if (verify_equihash_full(solution_indices, header, 0)) {
                valid_solutions++;
                printf("\n✅ VALID SOLUTION #%d FOUND!\n", valid_solutions);
                printf("Indices: ");
                for (int i = 0; i < 128; i++) {
                    printf("%08x%s", solution_indices[i], (i < 127) ? " " : "\n");
                }
                printf("\n");
                
                // Verify with verbose output
                printf("Verification details:\n");
                verify_equihash_full(solution_indices, header, 1);
            } else {
                if (show_progress) {
                    printf("  Candidate %u: ✗ Failed Equihash verification\n", candidates_checked);
                }
            }
        }
    }
    
    if (show_progress) {
        printf("\nBatch results: %d valid solution(s) from %u candidates\n", 
               valid_solutions, total);
    }
    
    // Cleanup trees
    free(trees.trees1_stage1);
    free(trees.trees0_stage2);
    free(trees.trees1_stage3);
    free(trees.trees0_stage4);
    free(trees.trees1_stage5);
    free(trees.trees0_stage6);
    free(trees.trees1_stage7);
    
cleanup:
    clReleaseMemObject(buf_round0);
    clReleaseMemObject(buf_tree1);
    clReleaseMemObject(buf_tree2);
    clReleaseMemObject(buf_tree3);
    clReleaseMemObject(buf_tree4);
    clReleaseMemObject(buf_tree5);
    clReleaseMemObject(buf_tree6);
    clReleaseMemObject(buf_tree7);
    for (int i = 0; i < 7; i++) clReleaseMemObject(buf_counts[i]);
    
    free(slot_counts);
    free(round0_hashes);
    
    return valid_solutions;
}

int main(int argc, char *argv[]) {
    uint32_t total_nonces = 100000;  // Default: 100K nonces
    
    if (argc > 1) {
        total_nonces = atoi(argv[1]);
        if (total_nonces < 1000 || total_nonces > 100000000) {
            fprintf(stderr, "Usage: %s [nonces]\n", argv[0]);
            fprintf(stderr, "Nonces must be between 1,000 and 100,000,000\n");
            fprintf(stderr, "Recommended: 1000000-2000000 for solution probability\n");
            return 1;
        }
    }
    
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║         sa-tromp: Equihash 192,7 GPU Miner              ║\n");
    printf("║    Local verification before pool integration testing    ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");
    
    printf("Configuration:\n");
    printf("  Algorithm: Equihash 192,7 (Tromp bucket-based)\n");
    printf("  Collision detection: 24-bit per stage (NBUCKETS=1M, NSLOTS=32)\n");
    printf("  Total nonces: %u (%u hashes)\n", total_nonces, total_nonces * 2);
    printf("  Memory: ~5.8GB GPU allocation\n\n");
    
    // Initialize OpenCL
    printf("Initializing OpenCL...\n");
    init_opencl();
    printf("✓ OpenCL ready\n\n");
    
    // Create test header
    uint8_t header[ZCASH_BLOCK_HEADER_LEN] = {0};
    memcpy(header, "test_block_header_data_192_7", 28);
    
    printf("Mining with test header: \"test_block_header_data_192_7\"\n");
    
    // Mine in batches (Beignet driver limit: ~1.1M nonces max per batch)
    uint32_t batch_size = 1000000;  // 1M nonces per batch (safe limit)
    uint32_t num_batches = (total_nonces + batch_size - 1) / batch_size;
    
    printf("\nProcessing in %u batch(es) of up to %u nonces each\n", num_batches, batch_size);
    printf("(Beignet driver limitation: max ~1.1M nonces per batch)\n");
    
    clock_t overall_start = clock();
    
    int total_solutions = 0;
    for (uint32_t batch = 0; batch < num_batches; batch++) {
        uint32_t batch_nonces = (batch == num_batches - 1) ? 
            (total_nonces - batch * batch_size) : batch_size;
        uint32_t nonce_start = batch * batch_size;
        
        printf("\n═══ Batch %u/%u: nonces %u-%u (%u nonces) ═══\n", 
               batch + 1, num_batches, nonce_start, nonce_start + batch_nonces - 1, batch_nonces);
        int solutions = mine_batch(batch_nonces, header, nonce_start, 1);
        total_solutions += solutions;
        
        if (solutions > 0) {
            printf("✅ Found %d valid solution(s) in this batch!\n", solutions);
        }
    }
    
    clock_t overall_end = clock();
    double total_time = (double)(overall_end - overall_start) / CLOCKS_PER_SEC;
    
    printf("\n╔═══════════════════════════════════════════════════════════╗\n");
    printf("║                    MINING COMPLETE                        ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");
    
    printf("Results:\n");
    printf("  Total nonces processed: %u\n", total_nonces);
    printf("  Total mining time: %.2f seconds\n", total_time);
    printf("  Hash rate: %.2f Sol/s\n", total_nonces / total_time);
    printf("  Valid solutions found: %d\n\n", total_solutions);
    
    if (total_solutions > 0) {
        printf("✅ SUCCESS!\n");
        printf("   GPU implementation produces VALID Equihash 192,7 solutions!\n");
        printf("   24-bit collision enforcement is working correctly.\n");
        printf("   Ready for integration into sa-solver (pool mining).\n\n");
    } else {
        printf("⚠️  No valid solutions found\n");
        if (total_nonces < 1000000) {
            printf("   Try more nonces (recommended: 1-2M for high probability)\n");
            printf("   Example: ./sa-tromp 1000000\n\n");
        } else {
            printf("   This may indicate an issue with collision detection.\n");
            printf("   Check GPU kernel implementation.\n\n");
        }
    }
    
    cleanup_opencl();
    
    return (total_solutions > 0) ? 0 : 1;
}

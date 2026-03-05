/*
** Test solution extraction with large nonce count
** This test allocates significant GPU memory - use with caution
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <CL/cl.h>
#include "blake.h"
#include "_kernel.h"
#include "solution_extraction.c"

#define PARAM_N 192
#define PARAM_K 7
#define ZCASH_HASH_LEN 48
#define NBUCKETS (1<<20)
#define NSLOTS 96
#define HASHBYTES_STAGE0 24

// OpenCL setup (simplified from test_all_stages.c)
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

// Generate Round 0 hashes (matches CPU baseline)
void generate_round0_hashes(unsigned char *hashes, uint32_t nonces) {
    blake2b_state_t blake_base, blake;
    uint8_t header[140] = {0};
    memcpy(header, "TestBlock", 9);
    
    zcash_blake2b_init(&blake_base, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    zcash_blake2b_update(&blake_base, header, 128, 0);
    
    uint32_t num_hashes = nonces * 2;
    for (uint32_t idx = 0; idx < num_hashes; idx++) {
        blake = blake_base;
        uint32_t g = idx / 2;
        zcash_blake2b_update(&blake, (uint8_t*)&g, sizeof(g), 0);
        
        uint8_t blakehash[48];
        zcash_blake2b_final(&blake, blakehash, 48);
        
        uint32_t offset = (idx % 2) * 24;
        memcpy(hashes + idx * HASHBYTES_STAGE0, blakehash + offset, HASHBYTES_STAGE0);
    }
}

int main(int argc, char *argv[]) {
    uint32_t nonces = 50000;  // Default: 50K nonces
    
    if (argc > 1) {
        nonces = atoi(argv[1]);
        if (nonces < 1000 || nonces > 10000000) {
            fprintf(stderr, "Nonces must be between 1000 and 10M\n");
            return 1;
        }
    }
    
    uint32_t num_hashes = nonces * 2;
    printf("\n=== Full Pipeline Solution Test ===\n");
    printf("Testing with %u nonces (%u hashes)\n\n", nonces, num_hashes);
    
    init_opencl();
    printf("✓ OpenCL initialized\n");
    
    // Generate Round 0 hashes
    printf("Generating Round 0 hashes...\n");
    unsigned char *round0_hashes = malloc(num_hashes * HASHBYTES_STAGE0);
    generate_round0_hashes(round0_hashes, nonces);
    printf("✓ Generated %u hashes\n", num_hashes);
    
    // Allocate GPU buffers
    printf("\nAllocating GPU buffers (~17.5 GB total)...\n");
    cl_int err;
    
    size_t tree_size = NBUCKETS * NSLOTS;
    cl_mem buf_round0 = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       num_hashes * HASHBYTES_STAGE0, round0_hashes, &err);
    check_error(err, "buf_round0");
    
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
    
    printf("✓ GPU buffers allocated\n");
    
    // Run all 7 stages
    size_t global_work_size = NBUCKETS;
    uint32_t *slot_counts = calloc(NBUCKETS, sizeof(uint32_t));
    
    printf("\nRunning all 7 stages...\n");
    
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
    printf("  Stage 1: %u collisions\n", total);
    
    // Stages 2-7 (similar pattern)
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
        printf("  Stage %d: %u %s\n", stage + 1, total, stage == 6 ? "solution candidates" : "collisions");
    }
    
    printf("\n✓ All stages complete\n");
    
    if (total == 0) {
        printf("\nℹ️  No solution candidates found\n");
        printf("   Try more nonces (e.g., 100K-1M for better probability)\n");
        goto cleanup;
    }
    
    printf("\n=== Extracting Solutions ===\n");
    printf("Found %u solution candidate(s)\n", total);
    
    // Read all tree buffers from GPU
    printf("Reading tree buffers from GPU...\n");
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
    
    printf("✓ Tree buffers read (%.1f MB)\n", 
           (tree_size * (sizeof(stage1_slot_t) + sizeof(stage2_slot_t) + sizeof(stage3_slot_t) + 
            sizeof(stage4_slot_t) + sizeof(stage5_slot_t) + sizeof(stage6_slot_t) + 
            sizeof(stage7_slot_t))) / (1024.0 * 1024.0));
    
    // Extract solutions from Stage 7 candidates
    printf("\nExtracting solution indices...\n");
    uint32_t valid_solutions = 0;
    uint32_t candidates_checked = 0;
    
    for (uint32_t bucketid = 0; bucketid < NBUCKETS && candidates_checked < total; bucketid++) {
        uint32_t bucket_count = slot_counts[bucketid];
        if (bucket_count == 0) continue;
        
        stage7_slot_t *bucket = &trees.trees1_stage7[bucketid * NSLOTS];
        
        for (uint32_t slotid = 0; slotid < bucket_count && slotid < NSLOTS; slotid++) {
            candidates_checked++;
            uint32_t solution_indices[128];
            
            if (extract_solution(&trees, bucket[slotid].attr, solution_indices)) {
                valid_solutions++;
                printf("  Solution %u: ✓ Valid (128 unique indices)\n", valid_solutions);
                
                // Print first 8 indices as sample
                printf("    Indices[0-7]: ");
                for (int i = 0; i < 8; i++) {
                    printf("%u ", solution_indices[i]);
                }
                printf("...\n");
            } else {
                printf("  Candidate %u: ✗ Duplicate indices (invalid)\n", candidates_checked);
            }
        }
    }
    
    printf("\n=== Results ===\n");
    printf("Solution candidates: %u\n", total);
    printf("Valid solutions: %u\n", valid_solutions);
    
    if (valid_solutions > 0) {
        printf("\n✅ SUCCESS: Extracted %u valid solution(s)!\n", valid_solutions);
        printf("Next step: Verify solutions with Blake2b hash\n");
    } else {
        printf("\n⚠️  All candidates had duplicate indices\n");
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
    cleanup_opencl();
    
    printf("\n✓ Test complete\n");
    return 0;
}

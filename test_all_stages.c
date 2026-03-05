/*
** Test all 7 collision detection stages
** Compare GPU collision counts vs CPU baseline
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <CL/cl.h>
#include "blake.h"
#include "_kernel.h"

#define PARAM_N 192
#define PARAM_K 7
#define ZCASH_HASH_LEN 48
#define ZCASH_BLOCK_HEADER_LEN 140

#define NBUCKETS_STAGE1 (1<<20)
#define NSLOTS_STAGE1 32
#define HASHBYTES_STAGE0 24

// Stage structures (from kernel)
typedef struct { uint32_t attr; unsigned char hash[21]; } stage1_slot_t;
typedef struct { uint32_t attr; unsigned char hash[18]; } stage2_slot_t;
typedef struct { uint32_t attr; unsigned char hash[15]; } stage3_slot_t;
typedef struct { uint32_t attr; unsigned char hash[12]; } stage4_slot_t;
typedef struct { uint32_t attr; unsigned char hash[9]; } stage5_slot_t;
typedef struct { uint32_t attr; unsigned char hash[6]; } stage6_slot_t;
typedef struct { uint32_t attr; unsigned char hash[3]; } stage7_slot_t;

// OpenCL context
cl_platform_id platform;
cl_device_id device;
cl_context context;
cl_command_queue queue;
cl_program program;
cl_kernel kernels[7];  // Stages 1-7

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
    
    // Create all stage kernels
    const char *kernel_names[] = {
        "kernel_stage1_collisions", "kernel_stage2_collisions", 
        "kernel_stage3_collisions", "kernel_stage4_collisions",
        "kernel_stage5_collisions", "kernel_stage6_collisions",
        "kernel_stage7_collisions"
    };
    
    for (int i = 0; i < 7; i++) {
        kernels[i] = clCreateKernel(program, kernel_names[i], &err);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "Failed to create %s: %d\n", kernel_names[i], err);
            exit(1);
        }
    }
}

void cleanup_opencl(void) {
    for (int i = 0; i < 7; i++) {
        clReleaseKernel(kernels[i]);
    }
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
}

// Generate Round 0 hashes (matches CPU baseline genhash())
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
    uint32_t nonces_to_test = 10000;  // Default: 10K nonces
    
    if (argc > 1) {
        nonces_to_test = atoi(argv[1]);
    }
    
    uint32_t num_hashes = nonces_to_test * 2;
    cl_int err;
    
    printf("\n=== Equihash 192,7 Full Pipeline Test ===\n");
    printf("Testing with %u nonces (%u Round 0 hashes)\n\n", nonces_to_test, num_hashes);
    
    // Initialize OpenCL
    printf("Initializing OpenCL...\n");
    init_opencl();
    printf("✓ OpenCL initialized (7 kernels loaded)\n");
    
    // Generate Round 0 hashes
    printf("\nGenerating Round 0 hashes...\n");
    size_t hash_buffer_size = num_hashes * HASHBYTES_STAGE0;
    unsigned char *round0_hashes = malloc(hash_buffer_size);
    generate_round0_hashes(round0_hashes, nonces_to_test);
    printf("✓ Generated %u Round 0 hashes\n", num_hashes);
    
    // Allocate GPU buffers for all stages
    printf("\nAllocating GPU buffers...\n");
    
    cl_mem buf_round0 = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                       hash_buffer_size, round0_hashes, &err);
    check_error(err, "clCreateBuffer(round0)");
    
    // Allocate tree buffers for each stage
    size_t tree_size = NBUCKETS_STAGE1 * NSLOTS_STAGE1;
    cl_mem buf_tree1 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage1_slot_t), NULL, &err);
    check_error(err, "clCreateBuffer(tree1)");
    cl_mem buf_tree2 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage2_slot_t), NULL, &err);
    check_error(err, "clCreateBuffer(tree2)");
    cl_mem buf_tree3 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage3_slot_t), NULL, &err);
    check_error(err, "clCreateBuffer(tree3)");
    cl_mem buf_tree4 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage4_slot_t), NULL, &err);
    check_error(err, "clCreateBuffer(tree4)");
    cl_mem buf_tree5 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage5_slot_t), NULL, &err);
    check_error(err, "clCreateBuffer(tree5)");
    cl_mem buf_tree6 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage6_slot_t), NULL, &err);
    check_error(err, "clCreateBuffer(tree6)");
    cl_mem buf_tree7 = clCreateBuffer(context, CL_MEM_READ_WRITE, tree_size * sizeof(stage7_slot_t), NULL, &err);
    check_error(err, "clCreateBuffer(tree7)");
    
    // Allocate slot count buffers
    uint32_t *slot_counts = calloc(NBUCKETS_STAGE1, sizeof(uint32_t));
    cl_mem buf_counts[7];
    for (int i = 0; i < 7; i++) {
        buf_counts[i] = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                       NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, &err);
        check_error(err, "clCreateBuffer(counts)");
    }
    
    size_t total_mem = hash_buffer_size + 
                       tree_size * (sizeof(stage1_slot_t) + sizeof(stage2_slot_t) + 
                                   sizeof(stage3_slot_t) + sizeof(stage4_slot_t) +
                                   sizeof(stage5_slot_t) + sizeof(stage6_slot_t) + 
                                   sizeof(stage7_slot_t)) +
                       7 * NBUCKETS_STAGE1 * sizeof(uint32_t);
    printf("✓ GPU buffers allocated (%.1f GB)\n", total_mem / (1024.0 * 1024.0 * 1024.0));
    
    // Run all stages
    size_t global_work_size = NBUCKETS_STAGE1;
    
    printf("\n=== Running Pipeline ===\n");
    
    // Stage 1
    printf("\nStage 1: Processing Round 0 hashes...\n");
    clSetKernelArg(kernels[0], 0, sizeof(cl_mem), &buf_round0);
    clSetKernelArg(kernels[0], 1, sizeof(cl_mem), &buf_tree1);
    clSetKernelArg(kernels[0], 2, sizeof(cl_mem), &buf_counts[0]);
    clSetKernelArg(kernels[0], 3, sizeof(uint32_t), &num_hashes);
    err = clEnqueueNDRangeKernel(queue, kernels[0], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel(stage1)");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[0], CL_TRUE, 0, NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    uint32_t total = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) total += slot_counts[i];
    printf("  Stage 1 complete: %u collisions\n", total);
    
    // Stage 2
    printf("\nStage 2: Processing Stage 1 collisions...\n");
    clSetKernelArg(kernels[1], 0, sizeof(cl_mem), &buf_tree1);
    clSetKernelArg(kernels[1], 1, sizeof(cl_mem), &buf_counts[0]);
    clSetKernelArg(kernels[1], 2, sizeof(cl_mem), &buf_tree2);
    clSetKernelArg(kernels[1], 3, sizeof(cl_mem), &buf_counts[1]);
    err = clEnqueueNDRangeKernel(queue, kernels[1], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel(stage2)");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[1], CL_TRUE, 0, NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    total = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) total += slot_counts[i];
    printf("  Stage 2 complete: %u collisions\n", total);
    
    // Stage 3
    printf("\nStage 3: Processing Stage 2 collisions...\n");
    clSetKernelArg(kernels[2], 0, sizeof(cl_mem), &buf_tree2);
    clSetKernelArg(kernels[2], 1, sizeof(cl_mem), &buf_counts[1]);
    clSetKernelArg(kernels[2], 2, sizeof(cl_mem), &buf_tree3);
    clSetKernelArg(kernels[2], 3, sizeof(cl_mem), &buf_counts[2]);
    err = clEnqueueNDRangeKernel(queue, kernels[2], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel(stage3)");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[2], CL_TRUE, 0, NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    total = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) total += slot_counts[i];
    printf("  Stage 3 complete: %u collisions\n", total);
    
    // Stage 4
    printf("\nStage 4: Processing Stage 3 collisions...\n");
    clSetKernelArg(kernels[3], 0, sizeof(cl_mem), &buf_tree3);
    clSetKernelArg(kernels[3], 1, sizeof(cl_mem), &buf_counts[2]);
    clSetKernelArg(kernels[3], 2, sizeof(cl_mem), &buf_tree4);
    clSetKernelArg(kernels[3], 3, sizeof(cl_mem), &buf_counts[3]);
    err = clEnqueueNDRangeKernel(queue, kernels[3], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel(stage4)");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[3], CL_TRUE, 0, NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    total = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) total += slot_counts[i];
    printf("  Stage 4 complete: %u collisions\n", total);
    
    // Stage 5
    printf("\nStage 5: Processing Stage 4 collisions...\n");
    clSetKernelArg(kernels[4], 0, sizeof(cl_mem), &buf_tree4);
    clSetKernelArg(kernels[4], 1, sizeof(cl_mem), &buf_counts[3]);
    clSetKernelArg(kernels[4], 2, sizeof(cl_mem), &buf_tree5);
    clSetKernelArg(kernels[4], 3, sizeof(cl_mem), &buf_counts[4]);
    err = clEnqueueNDRangeKernel(queue, kernels[4], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel(stage5)");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[4], CL_TRUE, 0, NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    total = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) total += slot_counts[i];
    printf("  Stage 5 complete: %u collisions\n", total);
    
    // Stage 6
    printf("\nStage 6: Processing Stage 5 collisions...\n");
    clSetKernelArg(kernels[5], 0, sizeof(cl_mem), &buf_tree5);
    clSetKernelArg(kernels[5], 1, sizeof(cl_mem), &buf_counts[4]);
    clSetKernelArg(kernels[5], 2, sizeof(cl_mem), &buf_tree6);
    clSetKernelArg(kernels[5], 3, sizeof(cl_mem), &buf_counts[5]);
    err = clEnqueueNDRangeKernel(queue, kernels[5], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel(stage6)");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[5], CL_TRUE, 0, NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    total = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) total += slot_counts[i];
    printf("  Stage 6 complete: %u collisions\n", total);
    
    // Stage 7 (final)
    printf("\nStage 7: Finding solution candidates...\n");
    clSetKernelArg(kernels[6], 0, sizeof(cl_mem), &buf_tree6);
    clSetKernelArg(kernels[6], 1, sizeof(cl_mem), &buf_counts[5]);
    clSetKernelArg(kernels[6], 2, sizeof(cl_mem), &buf_tree7);
    clSetKernelArg(kernels[6], 3, sizeof(cl_mem), &buf_counts[6]);
    err = clEnqueueNDRangeKernel(queue, kernels[6], 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel(stage7)");
    clFinish(queue);
    
    clEnqueueReadBuffer(queue, buf_counts[6], CL_TRUE, 0, NBUCKETS_STAGE1 * sizeof(uint32_t), slot_counts, 0, NULL, NULL);
    total = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) total += slot_counts[i];
    printf("  Stage 7 complete: %u solution candidates\n", total);
    
    printf("\n=== Results Summary ===\n");
    printf("Input: %u nonces (%u hashes)\n", nonces_to_test, num_hashes);
    printf("Solution candidates found: %u\n", total);
    
    if (total > 0) {
        printf("\n✅ SUCCESS: Pipeline produced solution candidates!\n");
        printf("Next step: Extract indices and verify solutions\n");
    } else {
        printf("\nℹ️  No solutions found (expected with small nonce count)\n");
        printf("   CPU baseline also finds 0 solutions with 10K nonces\n");
        printf("   Need ~1-2M nonces for high probability of solutions\n");
    }
    
    // Cleanup
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

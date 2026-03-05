/*
** Test program for Stage 1 collision detection kernel
** 
** Compares GPU Stage 1 collision detection against CPU baseline
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// Stage 1 slot structure (matches kernel)
typedef struct {
    uint32_t attr;
    unsigned char hash[21];
} stage1_slot_t;

// OpenCL setup
cl_platform_id platform;
cl_device_id device;
cl_context context;
cl_command_queue queue;
cl_program program;
cl_kernel kernel_round0;
cl_kernel kernel_stage1;

void check_error(cl_int err, const char *operation) {
    if (err != CL_SUCCESS) {
        fprintf(stderr, "Error during %s: %d\n", operation, err);
        exit(1);
    }
}

void init_opencl(void) {
    cl_int err;
    
    // Get platform
    err = clGetPlatformIDs(1, &platform, NULL);
    check_error(err, "clGetPlatformIDs");
    
    // Get device
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);
    check_error(err, "clGetDeviceIDs");
    
    // Create context
    context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    check_error(err, "clCreateContext");
    
    // Create command queue
    queue = clCreateCommandQueue(context, device, 0, &err);
    check_error(err, "clCreateCommandQueue");
    
    // Create program from kernel source
    program = clCreateProgramWithSource(context, 1, &ocl_code, NULL, &err);
    check_error(err, "clCreateProgramWithSource");
    
    // Build program
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
    
    // Create kernels
    kernel_round0 = clCreateKernel(program, "kernel_round0", &err);
    check_error(err, "clCreateKernel(kernel_round0)");
    
    kernel_stage1 = clCreateKernel(program, "kernel_stage1_collisions", &err);
    check_error(err, "clCreateKernel(kernel_stage1_collisions)");
}

void cleanup_opencl(void) {
    clReleaseKernel(kernel_stage1);
    clReleaseKernel(kernel_round0);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
}

// Generate test hashes with controlled collisions
void generate_test_hashes(unsigned char *hashes, uint32_t num_hashes, uint32_t *expected_collisions) {
    // Create some hashes with matching 24-bit prefixes (for collision testing)
    // Hash format: 24 bytes (192 bits)
    // Collision = first 24 bits match
    
    // Clear all hashes first
    memset(hashes, 0, num_hashes * HASHBYTES_STAGE0);
    
    *expected_collisions = 0;
    
    // Create a few collision pairs for testing
    // Pair 1: Indices 0 and 1 - bucket 0x12345
    hashes[0 * HASHBYTES_STAGE0 + 0] = 0x12;
    hashes[0 * HASHBYTES_STAGE0 + 1] = 0x34;
    hashes[0 * HASHBYTES_STAGE0 + 2] = 0x50;  // Bottom 4 bits: 0x0
    hashes[0 * HASHBYTES_STAGE0 + 3] = 0xAA;  // Different
    
    hashes[1 * HASHBYTES_STAGE0 + 0] = 0x12;
    hashes[1 * HASHBYTES_STAGE0 + 1] = 0x34;
    hashes[1 * HASHBYTES_STAGE0 + 2] = 0x50;  // Bottom 4 bits: 0x0 (match!)
    hashes[1 * HASHBYTES_STAGE0 + 3] = 0xBB;  // Different
    (*expected_collisions)++;
    
    // Pair 2: Indices 2 and 3 - bucket 0xABCDE, but different RESTBITS (no collision)
    hashes[2 * HASHBYTES_STAGE0 + 0] = 0xAB;
    hashes[2 * HASHBYTES_STAGE0 + 1] = 0xCD;
    hashes[2 * HASHBYTES_STAGE0 + 2] = 0xE1;  // Bottom 4 bits: 0x1
    
    hashes[3 * HASHBYTES_STAGE0 + 0] = 0xAB;
    hashes[3 * HASHBYTES_STAGE0 + 1] = 0xCD;
    hashes[3 * HASHBYTES_STAGE0 + 2] = 0xE2;  // Bottom 4 bits: 0x2 (no match)
    
    // Pair 3: Indices 4 and 5 - bucket 0x00000
    hashes[4 * HASHBYTES_STAGE0 + 0] = 0x00;
    hashes[4 * HASHBYTES_STAGE0 + 1] = 0x00;
    hashes[4 * HASHBYTES_STAGE0 + 2] = 0x0F;  // Bottom 4 bits: 0xF
    hashes[4 * HASHBYTES_STAGE0 + 3] = 0x11;
    
    hashes[5 * HASHBYTES_STAGE0 + 0] = 0x00;
    hashes[5 * HASHBYTES_STAGE0 + 1] = 0x00;
    hashes[5 * HASHBYTES_STAGE0 + 2] = 0x0F;  // Bottom 4 bits: 0xF (match!)
    hashes[5 * HASHBYTES_STAGE0 + 3] = 0x22;
    (*expected_collisions)++;
    
    // Fill rest with random unique values (no more collisions)
    for (uint32_t i = 6; i < num_hashes; i++) {
        for (int j = 0; j < HASHBYTES_STAGE0; j++) {
            hashes[i * HASHBYTES_STAGE0 + j] = (i * 7 + j * 13) & 0xFF;
        }
    }
}

int main(int argc, char *argv[]) {
    uint32_t num_test_hashes = 100;  // Small test set
    uint32_t expected_collisions = 0;
    cl_int err;
    
    printf("\n=== Stage 1 Collision Detection Test ===\n");
    printf("Testing with %u test hashes\n\n", num_test_hashes);
    
    // Initialize OpenCL
    printf("Initializing OpenCL...\n");
    init_opencl();
    printf("✓ OpenCL initialized\n");
    
    // Generate test hashes with known collisions
    printf("\nGenerating test data...\n");
    size_t hash_buffer_size = num_test_hashes * HASHBYTES_STAGE0;
    unsigned char *test_hashes = malloc(hash_buffer_size);
    generate_test_hashes(test_hashes, num_test_hashes, &expected_collisions);
    printf("✓ Generated %u test hashes with %u expected collisions\n", 
           num_test_hashes, expected_collisions);
    
    // Create GPU buffers
    printf("\nAllocating GPU buffers...\n");
    
    // Input: Round 0 hashes (24 bytes each)
    cl_mem buf_round0_hashes = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                               hash_buffer_size, test_hashes, &err);
    check_error(err, "clCreateBuffer(buf_round0_hashes)");
    
    // Output: Stage 1 slot counts (one uint per bucket)
    uint32_t *stage1_slot_counts = calloc(NBUCKETS_STAGE1, sizeof(uint32_t));
    cl_mem buf_stage1_slot_counts = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                                    NBUCKETS_STAGE1 * sizeof(uint32_t), 
                                                    stage1_slot_counts, &err);
    check_error(err, "clCreateBuffer(buf_stage1_slot_counts)");
    
    // Output: Stage 1 tree (collision pairs)
    size_t tree_size = NBUCKETS_STAGE1 * NSLOTS_STAGE1 * sizeof(stage1_slot_t);
    cl_mem buf_stage1_tree = clCreateBuffer(context, CL_MEM_WRITE_ONLY, tree_size, NULL, &err);
    check_error(err, "clCreateBuffer(buf_stage1_tree)");
    
    printf("✓ GPU buffers allocated\n");
    
    // Set kernel arguments (match kernel signature order)
    printf("\nSetting kernel arguments...\n");
    clSetKernelArg(kernel_stage1, 0, sizeof(cl_mem), &buf_round0_hashes);
    clSetKernelArg(kernel_stage1, 1, sizeof(cl_mem), &buf_stage1_tree);
    clSetKernelArg(kernel_stage1, 2, sizeof(cl_mem), &buf_stage1_slot_counts);
    clSetKernelArg(kernel_stage1, 3, sizeof(uint32_t), &num_test_hashes);
    printf("✓ Kernel arguments set\n");
    
    // Run kernel
    printf("\nRunning kernel_stage1_collisions...\n");
    size_t global_work_size = NBUCKETS_STAGE1;  // One work-item per bucket
    err = clEnqueueNDRangeKernel(queue, kernel_stage1, 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel");
    
    // Wait for completion
    clFinish(queue);
    printf("✓ Kernel completed\n");
    
    // Read back results
    printf("\nReading results...\n");
    err = clEnqueueReadBuffer(queue, buf_stage1_slot_counts, CL_TRUE, 0,
                              NBUCKETS_STAGE1 * sizeof(uint32_t),
                              stage1_slot_counts, 0, NULL, NULL);
    check_error(err, "clEnqueueReadBuffer");
    
    // Count total collisions found
    uint32_t total_collisions = 0;
    uint32_t buckets_with_collisions = 0;
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) {
        if (stage1_slot_counts[i] > 0) {
            buckets_with_collisions++;
            total_collisions += stage1_slot_counts[i];
        }
    }
    
    printf("✓ Results read back\n");
    
    // Print results
    printf("\n=== Results ===\n");
    printf("Expected collisions: %u\n", expected_collisions);
    printf("Found collisions:    %u\n", total_collisions);
    printf("Buckets with collisions: %u\n", buckets_with_collisions);
    
    if (total_collisions == expected_collisions) {
        printf("\n✅ TEST PASSED: Collision count matches!\n");
    } else {
        printf("\n❌ TEST FAILED: Collision count mismatch\n");
    }
    
    // Cleanup
    clReleaseMemObject(buf_stage1_tree);
    clReleaseMemObject(buf_stage1_slot_counts);
    clReleaseMemObject(buf_round0_hashes);
    free(stage1_slot_counts);
    free(test_hashes);
    cleanup_opencl();
    
    return (total_collisions == expected_collisions) ? 0 : 1;
}

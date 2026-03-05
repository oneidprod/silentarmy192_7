/*
** Test Stage 1 kernel with real Round 0 hashes (matches CPU baseline)
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
#define NSLOTS_STAGE1 96
#define HASHBYTES_STAGE0 24

// Stage 1 slot structure (matches kernel)
typedef struct {
    uint32_t attr;
    unsigned char hash[21];
} stage1_slot_t;

// Blake2b state (from CPU baseline)
typedef struct {
    uint64_t h[8];
    uint64_t t[2];
    uint64_t f[2];
    uint8_t buf[128];
    size_t buflen;
} blake2b_state_s;

// OpenCL setup
cl_platform_id platform;
cl_device_id device;
cl_context context;
cl_command_queue queue;
cl_program program;
cl_kernel kernel_stage1;

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
    
    kernel_stage1 = clCreateKernel(program, "kernel_stage1_collisions", &err);
    check_error(err, "clCreateKernel(kernel_stage1_collisions)");
}

void cleanup_opencl(void) {
    clReleaseKernel(kernel_stage1);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
}

// Generate Round 0 hashes using Blake2b (matches CPU baseline genhash())
void generate_round0_hashes(unsigned char *hashes, uint32_t nonces, uint32_t start_nonce) {
    blake2b_state_s blake_base, blake;
    
    // Use a fixed header for deterministic testing
    uint8_t header[140] = {0};
    memcpy(header, "TestBlock", 9);
    
   // Init base Blake2b state with header
    zcash_blake2b_init(&blake_base, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
    zcash_blake2b_update(&blake_base, header, 128, 0);
    
    uint32_t num_hashes = nonces * 2;
    
    for (uint32_t idx = 0; idx < num_hashes; idx++) {
        // Copy base state
        blake = blake_base;
        
        // Hash g = idx / 2 (HASHESPERBLAKE = 2)
        uint32_t g = idx / 2;
        zcash_blake2b_update(&blake, (uint8_t*)&g, sizeof(g), 0);
        
        // Get 48-byte hash
        uint8_t blakehash[48];
        zcash_blake2b_final(&blake, blakehash, 48);
        
        // Extract 24 bytes from appropriate position
        // idx%2==0 -> bytes 0-23, idx%2==1 -> bytes 24-47
        uint32_t offset = (idx % 2) * 24;
        memcpy(hashes + idx * HASHBYTES_STAGE0, blakehash + offset, HASHBYTES_STAGE0);
    }
}

int main(int argc, char *argv[]) {
    uint32_t nonces_to_test = 1000;  // Default: 1K nonces = 2K hashes
    
    if (argc > 1) {
        nonces_to_test = atoi(argv[1]);
    }
    
    uint32_t num_hashes = nonces_to_test * 2;
    cl_int err;
    
    printf("\n=== Stage 1 Test with Real Round 0 Hashes ===\n");
    printf("Testing with %u nonces (%u hashes)\n\n", nonces_to_test, num_hashes);
    
    // Initialize OpenCL
    printf("Initializing OpenCL...\n");
    init_opencl();
    printf("✓ OpenCL initialized\n");
    
    // Generate Round 0 hashes using Blake2b
    printf("\nGenerating Round 0 hashes...\n");
    size_t hash_buffer_size = num_hashes * HASHBYTES_STAGE0;
    unsigned char *round0_hashes = malloc(hash_buffer_size);
    generate_round0_hashes(round0_hashes, nonces_to_test, 0);
    printf("✓ Generated %u Round 0 hashes\n", num_hashes);
    
    // Create GPU buffers
    printf("\nAllocating GPU buffers...\n");
    
    cl_mem buf_round0_hashes = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                               hash_buffer_size, round0_hashes, &err);
    check_error(err, "clCreateBuffer(buf_round0_hashes)");
    
    uint32_t *stage1_slot_counts = calloc(NBUCKETS_STAGE1, sizeof(uint32_t));
    cl_mem buf_stage1_slot_counts = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                                    NBUCKETS_STAGE1 * sizeof(uint32_t), 
                                                    stage1_slot_counts, &err);
    check_error(err, "clCreateBuffer(buf_stage1_slot_counts)");
    
    size_t tree_size = NBUCKETS_STAGE1 * NSLOTS_STAGE1 * sizeof(stage1_slot_t);
    cl_mem buf_stage1_tree = clCreateBuffer(context, CL_MEM_WRITE_ONLY, tree_size, NULL, &err);
    check_error(err, "clCreateBuffer(buf_stage1_tree)");
    
    printf("✓ GPU buffers allocated (%.1f MB)\n", 
           (hash_buffer_size + tree_size + NBUCKETS_STAGE1 * sizeof(uint32_t)) / (1024.0 * 1024.0));
    
    // Set kernel arguments
    clSetKernelArg(kernel_stage1, 0, sizeof(cl_mem), &buf_round0_hashes);
    clSetKernelArg(kernel_stage1, 1, sizeof(cl_mem), &buf_stage1_tree);
    clSetKernelArg(kernel_stage1, 2, sizeof(cl_mem), &buf_stage1_slot_counts);
    clSetKernelArg(kernel_stage1, 3, sizeof(uint32_t), &num_hashes);
    
    // Run kernel
    printf("\nRunning kernel_stage1_collisions...\n");
    size_t global_work_size = NBUCKETS_STAGE1;
    err = clEnqueueNDRangeKernel(queue, kernel_stage1, 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    check_error(err, "clEnqueueNDRangeKernel");
    
    clFinish(queue);
    printf("✓ Kernel completed\n");
    
    // Read results
    printf("\nReading results...\n");
    err = clEnqueueReadBuffer(queue, buf_stage1_slot_counts, CL_TRUE, 0,
                              NBUCKETS_STAGE1 * sizeof(uint32_t),
                              stage1_slot_counts, 0, NULL, NULL);
    check_error(err, "clEnqueueReadBuffer");
    
    // Count collisions
    uint32_t total_collisions = 0;
    uint32_t buckets_with_collisions = 0;
    uint32_t max_collisions_in_bucket = 0;
    
    for (uint32_t i = 0; i < NBUCKETS_STAGE1; i++) {
        if (stage1_slot_counts[i] > 0) {
            buckets_with_collisions++;
            total_collisions += stage1_slot_counts[i];
            if (stage1_slot_counts[i] > max_collisions_in_bucket) {
                max_collisions_in_bucket = stage1_slot_counts[i];
            }
        }
    }
    
    printf("✓ Results analyzed\n");
    
    // Print results
    printf("\n=== GPU Results ===\n");
    printf("Input hashes:            %u\n", num_hashes);
    printf("Total collisions found:  %u\n", total_collisions);
    printf("Buckets with collisions: %u\n", buckets_with_collisions);
    printf("Max collisions/bucket:   %u\n", max_collisions_in_bucket);
    
    // Cleanup
    clReleaseMemObject(buf_stage1_tree);
    clReleaseMemObject(buf_stage1_slot_counts);
    clReleaseMemObject(buf_round0_hashes);
    free(stage1_slot_counts);
    free(round0_hashes);
    cleanup_opencl();
    
    printf("\n✓ Test complete\n");
    
    return 0;
}

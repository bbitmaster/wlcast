#define CL_TARGET_OPENCL_VERSION 120
#include "CL/cl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    cl_platform_id platform;
    cl_device_id device;
    cl_uint num_platforms, num_devices;
    cl_int err;
    char name[256];

    /* Get platform */
    err = clGetPlatformIDs(1, &platform, &num_platforms);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clGetPlatformIDs failed: %d\n", err);
        return 1;
    }
    printf("Found %u OpenCL platform(s)\n", num_platforms);

    clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(name), name, NULL);
    printf("Platform: %s\n", name);

    clGetPlatformInfo(platform, CL_PLATFORM_VERSION, sizeof(name), name, NULL);
    printf("Version: %s\n", name);

    /* Get device */
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, &num_devices);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clGetDeviceIDs failed: %d\n", err);
        return 1;
    }
    printf("Found %u GPU device(s)\n", num_devices);

    clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(name), name, NULL);
    printf("Device: %s\n", name);

    cl_uint compute_units;
    clGetDeviceInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(compute_units), &compute_units, NULL);
    printf("Compute units: %u\n", compute_units);

    size_t max_work_group;
    clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(max_work_group), &max_work_group, NULL);
    printf("Max work group size: %zu\n", max_work_group);

    cl_ulong global_mem;
    clGetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(global_mem), &global_mem, NULL);
    printf("Global memory: %lu MB\n", (unsigned long)(global_mem / 1024 / 1024));

    /* Create context */
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateContext failed: %d\n", err);
        return 1;
    }
    printf("Context created successfully\n");

    /* Simple kernel test */
    const char *kernel_src =
        "__kernel void test(__global int *out) {\n"
        "    int gid = get_global_id(0);\n"
        "    out[gid] = gid * 2;\n"
        "}\n";

    cl_program program = clCreateProgramWithSource(context, 1, &kernel_src, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateProgramWithSource failed: %d\n", err);
        return 1;
    }

    err = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        char log[4096];
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
        fprintf(stderr, "clBuildProgram failed: %d\n%s\n", err, log);
        return 1;
    }
    printf("Kernel compiled successfully\n");

    cl_kernel kernel = clCreateKernel(program, "test", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateKernel failed: %d\n", err);
        return 1;
    }

    /* Create command queue */
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateCommandQueue failed: %d\n", err);
        return 1;
    }

    /* Create buffer and run kernel */
    int output[16] = {0};
    cl_mem buf = clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(output), NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clCreateBuffer failed: %d\n", err);
        return 1;
    }

    clSetKernelArg(kernel, 0, sizeof(cl_mem), &buf);

    size_t global_size = 16;
    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_size, NULL, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clEnqueueNDRangeKernel failed: %d\n", err);
        return 1;
    }

    clEnqueueReadBuffer(queue, buf, CL_TRUE, 0, sizeof(output), output, 0, NULL, NULL);
    clFinish(queue);

    printf("Kernel output: ");
    for (int i = 0; i < 16; i++) {
        printf("%d ", output[i]);
    }
    printf("\n");

    /* Cleanup */
    clReleaseMemObject(buf);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    printf("OpenCL test PASSED!\n");
    return 0;
}

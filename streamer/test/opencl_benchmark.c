#define CL_TARGET_OPENCL_VERSION 120
#include "CL/cl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <linux/dma-heap.h>

typedef intptr_t cl_import_properties_arm;
#define CL_IMPORT_TYPE_ARM           0x40B2
#define CL_IMPORT_TYPE_DMA_BUF_ARM   0x40B4

typedef cl_mem (*clImportMemoryARM_fn)(cl_context, cl_mem_flags,
    const cl_import_properties_arm*, void*, size_t, cl_int*);

static const char *kernel_src =
"__kernel void xrgb_to_yuyv(__global const uchar4 *input,\n"
"                           __global uchar4 *output,\n"
"                           int width, int height) {\n"
"    int x = get_global_id(0);\n"
"    int y = get_global_id(1);\n"
"    if (x >= width/2 || y >= height) return;\n"
"    int idx = y * width + x * 2;\n"
"    uchar4 p0 = input[idx];\n"
"    uchar4 p1 = input[idx + 1];\n"
"    float r0 = (float)p0.z, g0 = (float)p0.y, b0 = (float)p0.x;\n"
"    float r1 = (float)p1.z, g1 = (float)p1.y, b1 = (float)p1.x;\n"
"    float y0 = 16.0f + 0.257f * r0 + 0.504f * g0 + 0.098f * b0;\n"
"    float y1 = 16.0f + 0.257f * r1 + 0.504f * g1 + 0.098f * b1;\n"
"    float r_avg = (r0 + r1) * 0.5f, g_avg = (g0 + g1) * 0.5f, b_avg = (b0 + b1) * 0.5f;\n"
"    float u = 128.0f - 0.148f * r_avg - 0.291f * g_avg + 0.439f * b_avg;\n"
"    float v = 128.0f + 0.439f * r_avg - 0.368f * g_avg - 0.071f * b_avg;\n"
"    uchar4 out;\n"
"    out.x = (uchar)clamp(y0, 0.0f, 255.0f);\n"
"    out.y = (uchar)clamp(u,  0.0f, 255.0f);\n"
"    out.z = (uchar)clamp(y1, 0.0f, 255.0f);\n"
"    out.w = (uchar)clamp(v,  0.0f, 255.0f);\n"
"    output[y * (width/2) + x] = out;\n"
"}\n";

static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int allocate_dmabuf(size_t size, int *fd_out) {
    int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) return -1;
    struct dma_heap_allocation_data data = {0};
    data.len = size;
    data.fd_flags = O_CLOEXEC | O_RDWR;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
        close(heap_fd);
        return -1;
    }
    close(heap_fd);
    *fd_out = (int)data.fd;
    return 0;
}

int main(int argc, char **argv) {
    int width = 640, height = 480;
    int iterations = 100;
    if (argc > 2) { width = atoi(argv[1]); height = atoi(argv[2]); }
    if (argc > 3) iterations = atoi(argv[3]);

    size_t input_size = (size_t)width * height * 4;
    size_t output_size = (size_t)width * height * 2;

    printf("Zero-copy OpenCL benchmark: %dx%d, %d iterations\n\n", width, height, iterations);

    cl_platform_id platform;
    cl_device_id device;
    cl_int err;

    clGetPlatformIDs(1, &platform, NULL);
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);

    clImportMemoryARM_fn clImportMemoryARM = (clImportMemoryARM_fn)
        clGetExtensionFunctionAddressForPlatform(platform, "clImportMemoryARM");

    cl_program program = clCreateProgramWithSource(context, 1, &kernel_src, NULL, &err);
    clBuildProgram(program, 1, &device, "-cl-fast-relaxed-math", NULL, NULL);
    cl_kernel kernel = clCreateKernel(program, "xrgb_to_yuyv", &err);

    size_t global_size[2] = { (size_t)(width / 2), (size_t)height };
    size_t local_size[2] = { 16, 16 };

    /* Allocate dmabufs */
    int input_fd, output_fd;
    allocate_dmabuf(input_size, &input_fd);
    allocate_dmabuf(output_size, &output_fd);

    /* mmap for verification and to simulate what capture gives us */
    uint8_t *input_map = mmap(NULL, input_size, PROT_READ | PROT_WRITE, MAP_SHARED, input_fd, 0);
    memset(input_map, 0x55, input_size);

    /* Import dmabufs */
    cl_import_properties_arm props[] = { CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM, 0 };
    cl_mem input_buf = clImportMemoryARM(context, CL_MEM_READ_ONLY, props, &input_fd, input_size, &err);
    cl_mem output_buf = clImportMemoryARM(context, CL_MEM_WRITE_ONLY, props, &output_fd, output_size, &err);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), &input_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &output_buf);
    clSetKernelArg(kernel, 2, sizeof(int), &width);
    clSetKernelArg(kernel, 3, sizeof(int), &height);

    /* Warmup */
    clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
    clFinish(queue);

    /* Benchmark ZERO-COPY path */
    double start = get_time_ms();
    for (int i = 0; i < iterations; i++) {
        clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
    }
    clFinish(queue);
    double end = get_time_ms();

    double zero_copy_ms = (end - start) / iterations;
    printf("ZERO-COPY (dmabuf → kernel → dmabuf):\n");
    printf("  %.3f ms/frame = %.1f FPS potential\n\n", zero_copy_ms, 1000.0 / zero_copy_ms);

    /* Benchmark WITH CPU COPIES (simulating upload + download) */
    cl_mem cpu_input = clCreateBuffer(context, CL_MEM_READ_ONLY, input_size, NULL, &err);
    cl_mem cpu_output = clCreateBuffer(context, CL_MEM_WRITE_ONLY, output_size, NULL, &err);
    uint8_t *output_cpu = malloc(output_size);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), &cpu_input);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &cpu_output);

    /* Warmup */
    clEnqueueWriteBuffer(queue, cpu_input, CL_FALSE, 0, input_size, input_map, 0, NULL, NULL);
    clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, cpu_output, CL_TRUE, 0, output_size, output_cpu, 0, NULL, NULL);

    start = get_time_ms();
    for (int i = 0; i < iterations; i++) {
        clEnqueueWriteBuffer(queue, cpu_input, CL_FALSE, 0, input_size, input_map, 0, NULL, NULL);
        clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
        clEnqueueReadBuffer(queue, cpu_output, CL_TRUE, 0, output_size, output_cpu, 0, NULL, NULL);
    }
    end = get_time_ms();

    double with_copy_ms = (end - start) / iterations;
    printf("WITH CPU COPIES (upload + kernel + download):\n");
    printf("  %.3f ms/frame = %.1f FPS potential\n\n", with_copy_ms, 1000.0 / with_copy_ms);

    printf("SPEEDUP: %.1fx faster with zero-copy!\n", with_copy_ms / zero_copy_ms);

    /* Cleanup */
    free(output_cpu);
    munmap(input_map, input_size);
    close(input_fd);
    close(output_fd);
    clReleaseMemObject(input_buf);
    clReleaseMemObject(output_buf);
    clReleaseMemObject(cpu_input);
    clReleaseMemObject(cpu_output);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    return 0;
}

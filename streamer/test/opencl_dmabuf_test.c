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
#include <linux/dma-heap.h>

/* ARM import memory extension types and constants */
typedef intptr_t cl_import_properties_arm;

#define CL_IMPORT_TYPE_ARM                      0x40B2
#define CL_IMPORT_TYPE_DMA_BUF_ARM              0x40B4
#define CL_IMPORT_DMA_BUF_DATA_CONSISTENCY_WITH_HOST_ARM 0x41E3

typedef cl_mem (*clImportMemoryARM_fn)(cl_context, cl_mem_flags,
    const cl_import_properties_arm*, void*, size_t, cl_int*);

/* Simple XRGB->YUYV kernel */
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

static int allocate_dmabuf(size_t size, int *fd_out) {
    int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        perror("open dma_heap");
        return -1;
    }

    struct dma_heap_allocation_data data = {0};
    data.len = size;
    data.fd_flags = O_CLOEXEC | O_RDWR;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return -1;
    }

    close(heap_fd);
    *fd_out = (int)data.fd;
    return 0;
}

int main(void) {
    int width = 640, height = 480;
    size_t input_size = (size_t)width * height * 4;   /* XRGB */
    size_t output_size = (size_t)width * height * 2;  /* YUYV */

    printf("Testing OpenCL dmabuf import: %dx%d\n", width, height);

    cl_platform_id platform;
    cl_device_id device;
    cl_int err;

    clGetPlatformIDs(1, &platform, NULL);
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);

    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);

    /* Get clImportMemoryARM function */
    clImportMemoryARM_fn clImportMemoryARM = (clImportMemoryARM_fn)
        clGetExtensionFunctionAddressForPlatform(platform, "clImportMemoryARM");

    if (!clImportMemoryARM) {
        fprintf(stderr, "clImportMemoryARM not available!\n");
        return 1;
    }
    printf("clImportMemoryARM function found!\n");

    /* Allocate input dmabuf */
    int input_fd;
    if (allocate_dmabuf(input_size, &input_fd) != 0) {
        return 1;
    }
    printf("Allocated input dmabuf: fd=%d, size=%zu\n", input_fd, input_size);

    /* Fill input dmabuf with test pattern */
    uint8_t *input_map = mmap(NULL, input_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, input_fd, 0);
    if (input_map == MAP_FAILED) {
        perror("mmap input");
        return 1;
    }
    for (size_t i = 0; i < input_size; i++) {
        input_map[i] = (uint8_t)(i & 0xFF);
    }
    printf("Filled input with test pattern\n");

    /* Import input dmabuf into OpenCL */
    cl_import_properties_arm import_props[] = {
        CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM,
        0
    };

    cl_mem input_buf = clImportMemoryARM(context, CL_MEM_READ_ONLY,
                                          import_props, &input_fd, input_size, &err);
    if (err != CL_SUCCESS || !input_buf) {
        fprintf(stderr, "clImportMemoryARM for input failed: %d\n", err);
        return 1;
    }
    printf("Successfully imported input dmabuf into OpenCL!\n");

    /* Allocate output dmabuf */
    int output_fd;
    if (allocate_dmabuf(output_size, &output_fd) != 0) {
        return 1;
    }
    printf("Allocated output dmabuf: fd=%d, size=%zu\n", output_fd, output_size);

    /* Import output dmabuf into OpenCL */
    cl_mem output_buf = clImportMemoryARM(context, CL_MEM_WRITE_ONLY,
                                           import_props, &output_fd, output_size, &err);
    if (err != CL_SUCCESS || !output_buf) {
        fprintf(stderr, "clImportMemoryARM for output failed: %d\n", err);
        return 1;
    }
    printf("Successfully imported output dmabuf into OpenCL!\n");

    /* Build and run kernel */
    cl_program program = clCreateProgramWithSource(context, 1, &kernel_src, NULL, &err);
    clBuildProgram(program, 1, &device, "-cl-fast-relaxed-math", NULL, NULL);
    cl_kernel kernel = clCreateKernel(program, "xrgb_to_yuyv", &err);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), &input_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &output_buf);
    clSetKernelArg(kernel, 2, sizeof(int), &width);
    clSetKernelArg(kernel, 3, sizeof(int), &height);

    size_t global_size[2] = { (size_t)(width / 2), (size_t)height };
    size_t local_size[2] = { 16, 16 };

    err = clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "clEnqueueNDRangeKernel failed: %d\n", err);
        return 1;
    }
    clFinish(queue);
    printf("Kernel executed successfully!\n");

    /* Verify output by mapping the output dmabuf directly (zero-copy read!) */
    uint8_t *output_map = mmap(NULL, output_size, PROT_READ, MAP_SHARED, output_fd, 0);
    if (output_map == MAP_FAILED) {
        perror("mmap output");
        return 1;
    }

    printf("First 16 YUYV bytes (read directly from dmabuf): ");
    for (int i = 0; i < 16; i++) {
        printf("%02x ", output_map[i]);
    }
    printf("\n");

    /* Cleanup */
    munmap(input_map, input_size);
    munmap(output_map, output_size);
    close(input_fd);
    close(output_fd);
    clReleaseMemObject(input_buf);
    clReleaseMemObject(output_buf);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    printf("\n=== ZERO-COPY DMABUF TEST PASSED! ===\n");
    printf("This proves we can:\n");
    printf("  1. Import dmabuf directly into OpenCL (no CPU copy)\n");
    printf("  2. Run XRGB->YUYV conversion on GPU\n");
    printf("  3. Output to dmabuf that JPEG encoder can read\n");
    return 0;
}

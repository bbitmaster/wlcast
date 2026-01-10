#define CL_TARGET_OPENCL_VERSION 120
#include "opencl_convert.h"
#include "CL/cl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>

/* ARM import memory extension */
typedef intptr_t cl_import_properties_arm;
#define CL_IMPORT_TYPE_ARM           0x40B2
#define CL_IMPORT_TYPE_DMA_BUF_ARM   0x40B4

typedef cl_mem (*clImportMemoryARM_fn)(cl_context, cl_mem_flags,
    const cl_import_properties_arm*, void*, size_t, cl_int*);

/* XRGB to YUYV conversion kernel
 * Input: XRGB8888 (BGRX in memory) - 4 bytes per pixel
 * Output: YUYV (packed) - 2 bytes per pixel
 *
 * Uses BT.601 coefficients for YUV conversion.
 */
static const char *xrgb_to_yuyv_kernel_src =
"__kernel void xrgb_to_yuyv(__global const uchar4 *input,\n"
"                           __global uchar4 *output,\n"
"                           int width, int height) {\n"
"    int x = get_global_id(0);  /* pixel pair index */\n"
"    int y = get_global_id(1);  /* row */\n"
"    \n"
"    if (x >= width/2 || y >= height) return;\n"
"    \n"
"    /* Read 2 BGRX pixels */\n"
"    int idx = y * width + x * 2;\n"
"    uchar4 p0 = input[idx];\n"
"    uchar4 p1 = input[idx + 1];\n"
"    \n"
"    /* Extract RGB (input is BGRX: B=x, G=y, R=z, X=w) */\n"
"    float r0 = (float)p0.z, g0 = (float)p0.y, b0 = (float)p0.x;\n"
"    float r1 = (float)p1.z, g1 = (float)p1.y, b1 = (float)p1.x;\n"
"    \n"
"    /* BT.601 Y calculation */\n"
"    float y0 = 16.0f + 0.257f * r0 + 0.504f * g0 + 0.098f * b0;\n"
"    float y1 = 16.0f + 0.257f * r1 + 0.504f * g1 + 0.098f * b1;\n"
"    \n"
"    /* Average RGB for chroma subsampling */\n"
"    float r_avg = (r0 + r1) * 0.5f;\n"
"    float g_avg = (g0 + g1) * 0.5f;\n"
"    float b_avg = (b0 + b1) * 0.5f;\n"
"    \n"
"    /* BT.601 U and V calculation */\n"
"    float u = 128.0f - 0.148f * r_avg - 0.291f * g_avg + 0.439f * b_avg;\n"
"    float v = 128.0f + 0.439f * r_avg - 0.368f * g_avg - 0.071f * b_avg;\n"
"    \n"
"    /* Pack as YUYV */\n"
"    uchar4 out;\n"
"    out.x = (uchar)clamp(y0, 0.0f, 255.0f);  /* Y0 */\n"
"    out.y = (uchar)clamp(u,  0.0f, 255.0f);  /* U  */\n"
"    out.z = (uchar)clamp(y1, 0.0f, 255.0f);  /* Y1 */\n"
"    out.w = (uchar)clamp(v,  0.0f, 255.0f);  /* V  */\n"
"    \n"
"    output[y * (width/2) + x] = out;\n"
"}\n";

struct opencl_converter {
    int width;
    int height;
    size_t input_size;
    size_t output_size;

    /* OpenCL objects */
    cl_platform_id platform;
    cl_device_id device;
    cl_context context;
    cl_command_queue queue;
    cl_program program;
    cl_kernel kernel;

    /* ARM import function */
    clImportMemoryARM_fn clImportMemoryARM;

    /* Output dmabuf (reused between frames) */
    int output_dmabuf_fd;
    void *output_map;
    cl_mem output_cl_mem;

    /* Cached input (to avoid reimporting same dmabuf) */
    int last_input_fd;
    cl_mem input_cl_mem;
};

static int allocate_dmabuf(size_t size, int *fd_out) {
    int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        perror("opencl: open dma_heap");
        return -1;
    }

    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = size;
    data.fd_flags = O_CLOEXEC | O_RDWR;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
        perror("opencl: DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return -1;
    }

    close(heap_fd);
    *fd_out = (int)data.fd;
    return 0;
}

struct opencl_converter *opencl_convert_init(int width, int height) {
    struct opencl_converter *conv = calloc(1, sizeof(*conv));
    if (!conv) return NULL;

    conv->width = width;
    conv->height = height;
    conv->input_size = (size_t)width * height * 4;   /* XRGB: 4 bytes/pixel */
    conv->output_size = (size_t)width * height * 2;  /* YUYV: 2 bytes/pixel */
    conv->output_dmabuf_fd = -1;
    conv->last_input_fd = -1;

    cl_int err;

    /* Get platform and device */
    err = clGetPlatformIDs(1, &conv->platform, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl: clGetPlatformIDs failed: %d\n", err);
        goto fail;
    }

    err = clGetDeviceIDs(conv->platform, CL_DEVICE_TYPE_GPU, 1, &conv->device, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl: clGetDeviceIDs failed: %d\n", err);
        goto fail;
    }

    /* Check for ARM import extension */
    char extensions[4096];
    clGetDeviceInfo(conv->device, CL_DEVICE_EXTENSIONS, sizeof(extensions), extensions, NULL);
    if (!strstr(extensions, "cl_arm_import_memory_dma_buf")) {
        fprintf(stderr, "opencl: cl_arm_import_memory_dma_buf not supported\n");
        goto fail;
    }

    /* Get import function */
    conv->clImportMemoryARM = (clImportMemoryARM_fn)
        clGetExtensionFunctionAddressForPlatform(conv->platform, "clImportMemoryARM");
    if (!conv->clImportMemoryARM) {
        fprintf(stderr, "opencl: clImportMemoryARM not found\n");
        goto fail;
    }

    /* Create context and queue */
    conv->context = clCreateContext(NULL, 1, &conv->device, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl: clCreateContext failed: %d\n", err);
        goto fail;
    }

    conv->queue = clCreateCommandQueue(conv->context, conv->device, 0, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl: clCreateCommandQueue failed: %d\n", err);
        goto fail;
    }

    /* Build kernel */
    conv->program = clCreateProgramWithSource(conv->context, 1,
                                               &xrgb_to_yuyv_kernel_src, NULL, &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl: clCreateProgramWithSource failed: %d\n", err);
        goto fail;
    }

    err = clBuildProgram(conv->program, 1, &conv->device, "-cl-fast-relaxed-math", NULL, NULL);
    if (err != CL_SUCCESS) {
        char log[4096];
        clGetProgramBuildInfo(conv->program, conv->device,
                              CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
        fprintf(stderr, "opencl: build failed: %d\n%s\n", err, log);
        goto fail;
    }

    conv->kernel = clCreateKernel(conv->program, "xrgb_to_yuyv", &err);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl: clCreateKernel failed: %d\n", err);
        goto fail;
    }

    /* Allocate output dmabuf */
    if (allocate_dmabuf(conv->output_size, &conv->output_dmabuf_fd) != 0) {
        goto fail;
    }

    /* Map output for CPU access (JPEG encoder may need this) */
    conv->output_map = mmap(NULL, conv->output_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, conv->output_dmabuf_fd, 0);
    if (conv->output_map == MAP_FAILED) {
        perror("opencl: mmap output");
        conv->output_map = NULL;
        goto fail;
    }

    /* Import output dmabuf into OpenCL */
    cl_import_properties_arm props[] = {
        CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM,
        0
    };
    conv->output_cl_mem = conv->clImportMemoryARM(conv->context, CL_MEM_WRITE_ONLY,
                                                   props, &conv->output_dmabuf_fd,
                                                   conv->output_size, &err);
    if (err != CL_SUCCESS || !conv->output_cl_mem) {
        fprintf(stderr, "opencl: import output dmabuf failed: %d\n", err);
        goto fail;
    }

    /* Set static kernel args */
    clSetKernelArg(conv->kernel, 1, sizeof(cl_mem), &conv->output_cl_mem);
    clSetKernelArg(conv->kernel, 2, sizeof(int), &width);
    clSetKernelArg(conv->kernel, 3, sizeof(int), &height);

    char device_name[256];
    clGetDeviceInfo(conv->device, CL_DEVICE_NAME, sizeof(device_name), device_name, NULL);
    fprintf(stderr, "OpenCL converter initialized: %s, %dx%d\n", device_name, width, height);

    return conv;

fail:
    opencl_convert_destroy(conv);
    return NULL;
}

int opencl_convert(struct opencl_converter *conv,
                   int input_dmabuf_fd, size_t input_size,
                   int *output_dmabuf_fd, size_t *output_size) {
    if (!conv) return -1;

    cl_int err;

    /* Check if we need to reimport the input dmabuf */
    if (input_dmabuf_fd != conv->last_input_fd) {
        /* Release previous input if any */
        if (conv->input_cl_mem) {
            clReleaseMemObject(conv->input_cl_mem);
            conv->input_cl_mem = NULL;
        }

        /* Import new input dmabuf */
        cl_import_properties_arm props[] = {
            CL_IMPORT_TYPE_ARM, CL_IMPORT_TYPE_DMA_BUF_ARM,
            0
        };
        conv->input_cl_mem = conv->clImportMemoryARM(conv->context, CL_MEM_READ_ONLY,
                                                      props, &input_dmabuf_fd,
                                                      input_size, &err);
        if (err != CL_SUCCESS || !conv->input_cl_mem) {
            fprintf(stderr, "opencl: import input dmabuf failed: %d\n", err);
            return -1;
        }

        conv->last_input_fd = input_dmabuf_fd;
        clSetKernelArg(conv->kernel, 0, sizeof(cl_mem), &conv->input_cl_mem);
    }

    /* Run kernel */
    size_t global_size[2] = { (size_t)(conv->width / 2), (size_t)conv->height };
    size_t local_size[2] = { 16, 16 };

    err = clEnqueueNDRangeKernel(conv->queue, conv->kernel, 2, NULL,
                                  global_size, local_size, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        fprintf(stderr, "opencl: kernel execution failed: %d\n", err);
        return -1;
    }

    /* Wait for completion */
    clFinish(conv->queue);

    *output_dmabuf_fd = conv->output_dmabuf_fd;
    *output_size = conv->output_size;

    return 0;
}

int opencl_convert_get_output(struct opencl_converter *conv,
                              int *dmabuf_fd, void **mapped_ptr, size_t *size) {
    if (!conv) return -1;

    if (dmabuf_fd) *dmabuf_fd = conv->output_dmabuf_fd;
    if (mapped_ptr) *mapped_ptr = conv->output_map;
    if (size) *size = conv->output_size;

    return 0;
}

void opencl_convert_destroy(struct opencl_converter *conv) {
    if (!conv) return;

    if (conv->input_cl_mem) clReleaseMemObject(conv->input_cl_mem);
    if (conv->output_cl_mem) clReleaseMemObject(conv->output_cl_mem);
    if (conv->kernel) clReleaseKernel(conv->kernel);
    if (conv->program) clReleaseProgram(conv->program);
    if (conv->queue) clReleaseCommandQueue(conv->queue);
    if (conv->context) clReleaseContext(conv->context);

    if (conv->output_map && conv->output_map != MAP_FAILED) {
        munmap(conv->output_map, conv->output_size);
    }
    if (conv->output_dmabuf_fd >= 0) {
        close(conv->output_dmabuf_fd);
    }

    free(conv);
}

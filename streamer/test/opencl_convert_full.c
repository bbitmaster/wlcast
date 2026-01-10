#define CL_TARGET_OPENCL_VERSION 120
#include "CL/cl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

static const char *xrgb_to_yuyv_kernel =
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

int main(int argc, char **argv) {
    int width = 640, height = 480;
    int iterations = 100;

    if (argc > 2) { width = atoi(argv[1]); height = atoi(argv[2]); }
    if (argc > 3) { iterations = atoi(argv[3]); }

    printf("Full round-trip XRGB->YUYV: %dx%d, %d iterations\n", width, height, iterations);

    cl_platform_id platform;
    cl_device_id device;
    cl_int err;

    clGetPlatformIDs(1, &platform, NULL);
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);

    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);

    cl_program program = clCreateProgramWithSource(context, 1, &xrgb_to_yuyv_kernel, NULL, &err);
    clBuildProgram(program, 1, &device, "-cl-fast-relaxed-math", NULL, NULL);
    cl_kernel kernel = clCreateKernel(program, "xrgb_to_yuyv", &err);

    size_t input_size = (size_t)width * height * 4;
    size_t output_size = (size_t)width * height * 2;

    uint8_t *input_data = (uint8_t *)malloc(input_size);
    uint8_t *output_data = (uint8_t *)malloc(output_size);

    /* Fill with test pattern */
    for (size_t i = 0; i < input_size; i++) {
        input_data[i] = (uint8_t)(i & 0xFF);
    }

    cl_mem input_buf = clCreateBuffer(context, CL_MEM_READ_ONLY, input_size, NULL, &err);
    cl_mem output_buf = clCreateBuffer(context, CL_MEM_WRITE_ONLY, output_size, NULL, &err);

    clSetKernelArg(kernel, 0, sizeof(cl_mem), &input_buf);
    clSetKernelArg(kernel, 1, sizeof(cl_mem), &output_buf);
    clSetKernelArg(kernel, 2, sizeof(int), &width);
    clSetKernelArg(kernel, 3, sizeof(int), &height);

    size_t global_size[2] = { (size_t)(width / 2), (size_t)height };
    size_t local_size[2] = { 16, 16 };

    /* Warmup */
    clEnqueueWriteBuffer(queue, input_buf, CL_FALSE, 0, input_size, input_data, 0, NULL, NULL);
    clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
    clEnqueueReadBuffer(queue, output_buf, CL_TRUE, 0, output_size, output_data, 0, NULL, NULL);

    /* Benchmark FULL round-trip: upload + kernel + download */
    double start = get_time_ms();
    for (int i = 0; i < iterations; i++) {
        clEnqueueWriteBuffer(queue, input_buf, CL_FALSE, 0, input_size, input_data, 0, NULL, NULL);
        clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
        clEnqueueReadBuffer(queue, output_buf, CL_TRUE, 0, output_size, output_data, 0, NULL, NULL);
    }
    double end = get_time_ms();

    double total_ms = end - start;
    double per_frame_ms = total_ms / iterations;
    double fps = 1000.0 / per_frame_ms;

    printf("FULL ROUND-TRIP (upload + kernel + download):\n");
    printf("  Total: %.2f ms for %d frames\n", total_ms, iterations);
    printf("  Per frame: %.3f ms (%.1f FPS)\n", per_frame_ms, fps);

    /* Benchmark just upload */
    start = get_time_ms();
    for (int i = 0; i < iterations; i++) {
        clEnqueueWriteBuffer(queue, input_buf, CL_TRUE, 0, input_size, input_data, 0, NULL, NULL);
    }
    end = get_time_ms();
    printf("Upload only: %.3f ms/frame\n", (end - start) / iterations);

    /* Benchmark just download */
    start = get_time_ms();
    for (int i = 0; i < iterations; i++) {
        clEnqueueReadBuffer(queue, output_buf, CL_TRUE, 0, output_size, output_data, 0, NULL, NULL);
    }
    end = get_time_ms();
    printf("Download only: %.3f ms/frame\n", (end - start) / iterations);

    /* Benchmark just kernel */
    start = get_time_ms();
    for (int i = 0; i < iterations; i++) {
        clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global_size, local_size, 0, NULL, NULL);
    }
    clFinish(queue);
    end = get_time_ms();
    printf("Kernel only: %.3f ms/frame\n", (end - start) / iterations);

    free(input_data);
    free(output_data);
    clReleaseMemObject(input_buf);
    clReleaseMemObject(output_buf);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);

    return 0;
}

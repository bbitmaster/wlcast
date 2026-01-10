#define CL_TARGET_OPENCL_VERSION 120
#include "CL/cl.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    cl_platform_id platform;
    cl_device_id device;

    clGetPlatformIDs(1, &platform, NULL);
    clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);

    char extensions[8192] = {0};
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, sizeof(extensions), extensions, NULL);

    printf("=== All OpenCL Extensions ===\n%s\n\n", extensions);

    printf("=== Key extensions for zero-copy ===\n");

    /* Check for important extensions */
    if (strstr(extensions, "cl_arm_import_memory"))
        printf("  cl_arm_import_memory - CAN IMPORT DMABUF DIRECTLY!\n");
    if (strstr(extensions, "cl_khr_egl_image"))
        printf("  cl_khr_egl_image - can use EGL images\n");
    if (strstr(extensions, "cl_khr_gl_sharing"))
        printf("  cl_khr_gl_sharing - OpenGL interop\n");
    if (strstr(extensions, "cl_arm_shared_virtual_memory"))
        printf("  cl_arm_shared_virtual_memory - shared memory\n");
    if (strstr(extensions, "cl_ext_image_requirements_info"))
        printf("  cl_ext_image_requirements_info - image info\n");

    return 0;
}

#ifndef WLCAST_OPENCL_CONVERT_H
#define WLCAST_OPENCL_CONVERT_H

#include <stddef.h>
#include <stdint.h>

struct opencl_converter;

/*
 * Initialize OpenCL converter for XRGB->YUYV conversion.
 * Uses cl_arm_import_memory_dma_buf for zero-copy dmabuf import.
 *
 * Returns: converter handle on success, NULL on failure
 */
struct opencl_converter *opencl_convert_init(int width, int height);

/*
 * Convert XRGB dmabuf to YUYV dmabuf using GPU.
 *
 * input_dmabuf_fd: dmabuf containing XRGB8888 data (e.g., from wlr-export-dmabuf)
 * input_size: size of input dmabuf in bytes
 * output_dmabuf_fd: pointer to receive output YUYV dmabuf fd
 * output_size: pointer to receive output size
 *
 * The output dmabuf is owned by the converter and reused between calls.
 * It remains valid until the next call to opencl_convert() or opencl_convert_destroy().
 *
 * Returns: 0 on success, -1 on failure
 */
int opencl_convert(struct opencl_converter *conv,
                   int input_dmabuf_fd, size_t input_size,
                   int *output_dmabuf_fd, size_t *output_size);

/*
 * Get the output dmabuf fd and mapped pointer for direct access.
 * Useful for passing to JPEG encoder.
 */
int opencl_convert_get_output(struct opencl_converter *conv,
                              int *dmabuf_fd, void **mapped_ptr, size_t *size);

/*
 * Destroy converter and free resources.
 */
void opencl_convert_destroy(struct opencl_converter *conv);

#endif /* WLCAST_OPENCL_CONVERT_H */

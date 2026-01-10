#ifndef WLCAST_V4L2_RGA_H
#define WLCAST_V4L2_RGA_H

#include <stdint.h>

/**
 * RGA (Rockchip Graphics Accelerator) color space converter
 *
 * Uses V4L2 M2M interface at /dev/video2 to convert XRGB8888 to NV12
 * in hardware, eliminating CPU-based color conversion.
 */

struct v4l2_rga_converter {
    int fd;
    int width;
    int height;

    /* Output (input to RGA) - XRGB8888 */
    uint32_t out_format;
    unsigned int out_num_planes;
    unsigned int out_bytesperline[2];
    unsigned int out_plane_size[2];
    void *out_map;
    unsigned int out_map_size;

    /* Capture (output from RGA) - NV12 */
    uint32_t cap_format;
    unsigned int cap_num_planes;
    unsigned int cap_bytesperline[2];
    unsigned int cap_plane_size[2];
    void *cap_map[2];
    unsigned int cap_map_size[2];
    int cap_queued;
};

/**
 * Initialize the RGA converter
 * @param rga Converter context to initialize
 * @param width Frame width
 * @param height Frame height
 * @return 0 on success, -1 on error
 */
int v4l2_rga_init(struct v4l2_rga_converter *rga, int width, int height);

/**
 * Convert a dmabuf frame from XRGB8888 to NV12
 * @param rga Converter context
 * @param dmabuf_fd DMA buffer file descriptor containing XRGB8888 data
 * @param mapped_data Pointer to mapped dmabuf data (used if DMABUF mode fails)
 * @param y_plane Output: pointer to Y plane data
 * @param y_stride Output: Y plane stride
 * @param uv_plane Output: pointer to UV plane data
 * @param uv_stride Output: UV plane stride
 * @return 0 on success, -1 on error
 */
int v4l2_rga_convert_dmabuf(struct v4l2_rga_converter *rga,
                            int dmabuf_fd,
                            void *mapped_data,
                            void **y_plane, unsigned int *y_stride,
                            void **uv_plane, unsigned int *uv_stride);

/**
 * Destroy the RGA converter and free resources
 * @param rga Converter context
 */
void v4l2_rga_destroy(struct v4l2_rga_converter *rga);

#endif /* WLCAST_V4L2_RGA_H */

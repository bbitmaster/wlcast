#include "v4l2_rga.h"
#include "v4l2_common.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

static int debug_enabled(void) {
    return v4l2_debug_enabled("SM_RGA_DEBUG");
}

static void dump_format(const char *label, const struct v4l2_format *fmt) {
    if (!debug_enabled()) return;
    fprintf(stderr, "RGA %s: %s %dx%d planes=%d\n",
            label,
            fourcc_to_str(fmt->fmt.pix_mp.pixelformat),
            fmt->fmt.pix_mp.width,
            fmt->fmt.pix_mp.height,
            fmt->fmt.pix_mp.num_planes);
    for (unsigned int i = 0; i < fmt->fmt.pix_mp.num_planes; ++i) {
        fprintf(stderr, "  plane[%u]: bpl=%u size=%u\n", i,
                fmt->fmt.pix_mp.plane_fmt[i].bytesperline,
                fmt->fmt.pix_mp.plane_fmt[i].sizeimage);
    }
}

static int find_rga_device(void) {
    /* Look for rockchip-rga device */
    const char *devices[] = {"/dev/video2", "/dev/video3", "/dev/video4",
                             "/dev/video5", NULL};

    for (int i = 0; devices[i]; i++) {
        int fd = open(devices[i], O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            if (strcmp((char *)cap.driver, "rockchip-rga") == 0) {
                if (debug_enabled()) {
                    fprintf(stderr, "Found RGA at %s\n", devices[i]);
                }
                return fd;
            }
        }
        close(fd);
    }

    fprintf(stderr, "RGA device not found\n");
    return -1;
}

static int queue_capture(struct v4l2_rga_converter *rga) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[2];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.length = rga->cap_num_planes;
    buf.m.planes = planes;

    if (xioctl(rga->fd, VIDIOC_QBUF, &buf) != 0) {
        perror("RGA VIDIOC_QBUF capture");
        return -1;
    }
    rga->cap_queued = 1;
    return 0;
}

int v4l2_rga_init(struct v4l2_rga_converter *rga, int width, int height) {
    memset(rga, 0, sizeof(*rga));
    rga->fd = find_rga_device();
    if (rga->fd < 0) {
        return -1;
    }

    rga->width = width;
    rga->height = height;

    /* Set output format (input to RGA) - XRGB8888 */
    struct v4l2_format out_fmt;
    memset(&out_fmt, 0, sizeof(out_fmt));
    out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_fmt.fmt.pix_mp.width = width;
    out_fmt.fmt.pix_mp.height = height;
    out_fmt.fmt.pix_mp.pixelformat = DRM_FORMAT_XRGB8888;
    out_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    out_fmt.fmt.pix_mp.num_planes = 1;
    out_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = width * 4;

    if (xioctl(rga->fd, VIDIOC_S_FMT, &out_fmt) != 0) {
        perror("RGA VIDIOC_S_FMT output");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }
    if (xioctl(rga->fd, VIDIOC_G_FMT, &out_fmt) != 0) {
        perror("RGA VIDIOC_G_FMT output");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }

    dump_format("output", &out_fmt);

    rga->out_format = out_fmt.fmt.pix_mp.pixelformat;
    rga->out_num_planes = out_fmt.fmt.pix_mp.num_planes;
    for (unsigned int i = 0; i < rga->out_num_planes; ++i) {
        rga->out_bytesperline[i] = out_fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
        rga->out_plane_size[i] = out_fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
    }

    /* Set capture format (output from RGA) - NV12 */
    struct v4l2_format cap_fmt;
    memset(&cap_fmt, 0, sizeof(cap_fmt));
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_fmt.fmt.pix_mp.width = width;
    cap_fmt.fmt.pix_mp.height = height;
    cap_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    cap_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    cap_fmt.fmt.pix_mp.num_planes = 1;

    if (xioctl(rga->fd, VIDIOC_S_FMT, &cap_fmt) != 0) {
        perror("RGA VIDIOC_S_FMT capture");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }
    if (xioctl(rga->fd, VIDIOC_G_FMT, &cap_fmt) != 0) {
        perror("RGA VIDIOC_G_FMT capture");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }

    dump_format("capture", &cap_fmt);

    rga->cap_format = cap_fmt.fmt.pix_mp.pixelformat;
    rga->cap_num_planes = cap_fmt.fmt.pix_mp.num_planes;
    for (unsigned int i = 0; i < rga->cap_num_planes; ++i) {
        rga->cap_bytesperline[i] = cap_fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
        rga->cap_plane_size[i] = cap_fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
    }

    /* Request output buffers - use MMAP (DMABUF and USERPTR both fail) */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(rga->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
        perror("RGA VIDIOC_REQBUFS output MMAP");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }

    /* Query and mmap output buffer */
    struct v4l2_buffer out_buf;
    struct v4l2_plane out_planes[1];
    memset(&out_buf, 0, sizeof(out_buf));
    memset(out_planes, 0, sizeof(out_planes));
    out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_buf.memory = V4L2_MEMORY_MMAP;
    out_buf.index = 0;
    out_buf.length = 1;
    out_buf.m.planes = out_planes;

    if (xioctl(rga->fd, VIDIOC_QUERYBUF, &out_buf) != 0) {
        perror("RGA VIDIOC_QUERYBUF output");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }

    rga->out_map_size = out_buf.m.planes[0].length;
    rga->out_map = mmap(NULL, rga->out_map_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED,
                        rga->fd, out_buf.m.planes[0].m.mem_offset);
    if (rga->out_map == MAP_FAILED) {
        perror("RGA mmap output");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }

    if (debug_enabled()) {
        fprintf(stderr, "RGA output memory: MMAP (%u bytes)\n", rga->out_map_size);
    }

    /* Request capture buffers - MMAP for output */
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(rga->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
        perror("RGA VIDIOC_REQBUFS capture");
        close(rga->fd);
        rga->fd = -1;
        return -1;
    }

    /* Query and mmap capture buffer */
    struct v4l2_buffer cap_buf;
    struct v4l2_plane cap_planes[2];
    memset(&cap_buf, 0, sizeof(cap_buf));
    memset(cap_planes, 0, sizeof(cap_planes));
    cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    cap_buf.index = 0;
    cap_buf.length = rga->cap_num_planes;
    cap_buf.m.planes = cap_planes;

    if (xioctl(rga->fd, VIDIOC_QUERYBUF, &cap_buf) != 0) {
        perror("RGA VIDIOC_QUERYBUF capture");
        v4l2_rga_destroy(rga);
        return -1;
    }

    for (unsigned int i = 0; i < rga->cap_num_planes; ++i) {
        rga->cap_map_size[i] = cap_buf.m.planes[i].length;
        rga->cap_map[i] = mmap(NULL, rga->cap_map_size[i],
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               rga->fd, cap_buf.m.planes[i].m.mem_offset);
        if (rga->cap_map[i] == MAP_FAILED) {
            perror("RGA mmap capture");
            v4l2_rga_destroy(rga);
            return -1;
        }
    }

    /* Queue capture buffer */
    if (queue_capture(rga) != 0) {
        v4l2_rga_destroy(rga);
        return -1;
    }

    /* Start streaming on both queues */
    enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (xioctl(rga->fd, VIDIOC_STREAMON, &out_type) != 0) {
        perror("RGA VIDIOC_STREAMON output");
        v4l2_rga_destroy(rga);
        return -1;
    }
    if (xioctl(rga->fd, VIDIOC_STREAMON, &cap_type) != 0) {
        perror("RGA VIDIOC_STREAMON capture");
        v4l2_rga_destroy(rga);
        return -1;
    }

    if (debug_enabled()) {
        fprintf(stderr, "RGA initialized: %dx%d %s -> %s\n",
                width, height,
                fourcc_to_str(rga->out_format),
                fourcc_to_str(rga->cap_format));
    }

    return 0;
}

int v4l2_rga_convert_dmabuf(struct v4l2_rga_converter *rga,
                            int dmabuf_fd,
                            void *mapped_data,
                            void **y_plane, unsigned int *y_stride,
                            void **uv_plane, unsigned int *uv_stride) {
    if (!rga || rga->fd < 0) {
        return -1;
    }

    /* Queue capture buffer if not already queued */
    if (!rga->cap_queued) {
        if (queue_capture(rga) != 0) {
            return -1;
        }
    }

    /* Copy input data to MMAP buffer.
     * Note: dmabuf_fd kept in API for potential future DMABUF import support,
     * but RGA V4L2 driver currently requires MMAP, so we copy from mapped_data. */
    (void)dmabuf_fd;
    if (!mapped_data) {
        fprintf(stderr, "RGA: mapped_data is required\n");
        return -1;
    }
    memcpy(rga->out_map, mapped_data, rga->out_plane_size[0]);

    /* Queue output buffer */
    struct v4l2_buffer out_buf;
    struct v4l2_plane out_planes[1];
    memset(&out_buf, 0, sizeof(out_buf));
    memset(out_planes, 0, sizeof(out_planes));

    out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_buf.memory = V4L2_MEMORY_MMAP;
    out_buf.index = 0;
    out_buf.length = 1;
    out_buf.m.planes = out_planes;
    out_planes[0].bytesused = rga->out_plane_size[0];

    if (xioctl(rga->fd, VIDIOC_QBUF, &out_buf) != 0) {
        perror("RGA VIDIOC_QBUF output");
        return -1;
    }

    /* Wait for conversion to complete */
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = rga->fd;
    pfd.events = POLLIN;

    int poll_rc = poll(&pfd, 1, 1000);
    if (poll_rc <= 0) {
        fprintf(stderr, "RGA poll timeout or error\n");
        return -1;
    }

    /* Dequeue capture buffer */
    struct v4l2_buffer cap_buf;
    struct v4l2_plane cap_planes[2];
    memset(&cap_buf, 0, sizeof(cap_buf));
    memset(cap_planes, 0, sizeof(cap_planes));

    cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_buf.memory = V4L2_MEMORY_MMAP;
    cap_buf.index = 0;
    cap_buf.length = rga->cap_num_planes;
    cap_buf.m.planes = cap_planes;

    if (xioctl(rga->fd, VIDIOC_DQBUF, &cap_buf) != 0) {
        perror("RGA VIDIOC_DQBUF capture");
        return -1;
    }
    rga->cap_queued = 0;

    /* Dequeue output buffer */
    struct v4l2_buffer out_done;
    struct v4l2_plane out_done_planes[1];
    memset(&out_done, 0, sizeof(out_done));
    memset(out_done_planes, 0, sizeof(out_done_planes));

    out_done.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_done.memory = V4L2_MEMORY_MMAP;
    out_done.index = 0;
    out_done.length = 1;
    out_done.m.planes = out_done_planes;

    if (xioctl(rga->fd, VIDIOC_DQBUF, &out_done) != 0) {
        perror("RGA VIDIOC_DQBUF output");
        return -1;
    }

    /* Return pointers to NV12 data
     * NV12 layout: Y plane followed by interleaved UV plane */
    *y_plane = rga->cap_map[0];
    *y_stride = rga->cap_bytesperline[0];

    /* For single-plane NV12, UV is after Y in the same buffer */
    if (rga->cap_num_planes == 1) {
        *uv_plane = (uint8_t *)rga->cap_map[0] +
                    rga->cap_bytesperline[0] * rga->height;
        *uv_stride = rga->cap_bytesperline[0];
    } else {
        *uv_plane = rga->cap_map[1];
        *uv_stride = rga->cap_bytesperline[1];
    }

    return 0;
}

void v4l2_rga_destroy(struct v4l2_rga_converter *rga) {
    if (!rga) return;

    if (rga->fd >= 0) {
        enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(rga->fd, VIDIOC_STREAMOFF, &out_type);
        xioctl(rga->fd, VIDIOC_STREAMOFF, &cap_type);
    }

    /* Unmap output buffer */
    if (rga->out_map && rga->out_map != MAP_FAILED) {
        munmap(rga->out_map, rga->out_map_size);
    }

    /* Unmap capture buffers */
    for (unsigned int i = 0; i < 2; ++i) {
        if (rga->cap_map[i] && rga->cap_map[i] != MAP_FAILED) {
            munmap(rga->cap_map[i], rga->cap_map_size[i]);
        }
    }

    if (rga->fd >= 0) {
        close(rga->fd);
    }

    memset(rga, 0, sizeof(*rga));
    rga->fd = -1;
}

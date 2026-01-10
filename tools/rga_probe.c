/* RGA capability probe - enumerate supported formats */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* DRM fourcc codes */
#define DRM_FORMAT_XRGB8888 0x34325258  /* XR24 */
#define DRM_FORMAT_ARGB8888 0x34325241  /* AR24 */

/* V4L2 might not have XRGB32 on older headers */
#ifndef V4L2_PIX_FMT_XRGB32
#define V4L2_PIX_FMT_XRGB32 v4l2_fourcc('X', 'R', '2', '4')
#endif

static const char *fourcc_to_str(uint32_t fmt) {
    static char str[5];
    str[0] = (char)(fmt & 0x7f);
    str[1] = (char)((fmt >> 8) & 0x7f);
    str[2] = (char)((fmt >> 16) & 0x7f);
    str[3] = (char)((fmt >> 24) & 0x7f);
    str[4] = '\0';
    return str;
}

static void enum_formats(int fd, enum v4l2_buf_type type, const char *label) {
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = type;

    printf("\n%s formats:\n", label);
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        printf("  %s (0x%08x) - %s\n",
               fourcc_to_str(fmtdesc.pixelformat),
               fmtdesc.pixelformat,
               fmtdesc.description);
        fmtdesc.index++;
    }
}

static void query_caps(int fd) {
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        printf("Driver: %s\n", cap.driver);
        printf("Card: %s\n", cap.card);
        printf("Bus: %s\n", cap.bus_info);
        printf("Capabilities: 0x%08x\n", cap.capabilities);

        if (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)
            printf("  - VIDEO_M2M_MPLANE\n");
        if (cap.capabilities & V4L2_CAP_VIDEO_M2M)
            printf("  - VIDEO_M2M\n");
        if (cap.capabilities & V4L2_CAP_STREAMING)
            printf("  - STREAMING\n");
    }
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/video2";

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    printf("=== %s ===\n", dev);
    query_caps(fd);

    /* Try both M2M_MPLANE and single-plane types */
    enum_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT_MPLANE (input to RGA)");
    enum_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE_MPLANE (output from RGA)");
    enum_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "OUTPUT (input)");
    enum_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "CAPTURE (output)");

    /* Try setting a test format */
    printf("\n--- Testing XRGB8888 -> NV12 conversion ---\n");

    struct v4l2_format out_fmt, cap_fmt;
    memset(&out_fmt, 0, sizeof(out_fmt));
    memset(&cap_fmt, 0, sizeof(cap_fmt));

    /* Set input (OUTPUT) to XRGB8888 */
    out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_fmt.fmt.pix_mp.width = 640;
    out_fmt.fmt.pix_mp.height = 480;
    out_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_XRGB32;  /* BGRX in memory */
    out_fmt.fmt.pix_mp.num_planes = 1;

    if (ioctl(fd, VIDIOC_S_FMT, &out_fmt) == 0) {
        printf("Set OUTPUT format: %s %dx%d (planes=%d, bpl=%d)\n",
               fourcc_to_str(out_fmt.fmt.pix_mp.pixelformat),
               out_fmt.fmt.pix_mp.width,
               out_fmt.fmt.pix_mp.height,
               out_fmt.fmt.pix_mp.num_planes,
               out_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
    } else {
        perror("S_FMT OUTPUT");
    }

    /* Set output (CAPTURE) to NV12 */
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_fmt.fmt.pix_mp.width = 640;
    cap_fmt.fmt.pix_mp.height = 480;
    cap_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    cap_fmt.fmt.pix_mp.num_planes = 1;

    if (ioctl(fd, VIDIOC_S_FMT, &cap_fmt) == 0) {
        printf("Set CAPTURE format: %s %dx%d (planes=%d, bpl=%d)\n",
               fourcc_to_str(cap_fmt.fmt.pix_mp.pixelformat),
               cap_fmt.fmt.pix_mp.width,
               cap_fmt.fmt.pix_mp.height,
               cap_fmt.fmt.pix_mp.num_planes,
               cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
    } else {
        perror("S_FMT CAPTURE");
    }

    close(fd);
    return 0;
}

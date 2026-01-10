#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *fourcc_to_str(__u32 fmt) {
  static char str[5];
  str[0] = (char)(fmt & 0x7f);
  str[1] = (char)((fmt >> 8) & 0x7f);
  str[2] = (char)((fmt >> 16) & 0x7f);
  str[3] = (char)((fmt >> 24) & 0x7f);
  str[4] = '\0';
  return str;
}

static void print_caps(__u32 caps) {
  struct cap_entry {
    __u32 bit;
    const char *name;
  } entries[] = {
      {V4L2_CAP_VIDEO_CAPTURE, "VIDEO_CAPTURE"},
      {V4L2_CAP_VIDEO_OUTPUT, "VIDEO_OUTPUT"},
      {V4L2_CAP_VIDEO_OVERLAY, "VIDEO_OVERLAY"},
      {V4L2_CAP_VBI_CAPTURE, "VBI_CAPTURE"},
      {V4L2_CAP_VBI_OUTPUT, "VBI_OUTPUT"},
      {V4L2_CAP_SLICED_VBI_CAPTURE, "SLICED_VBI_CAPTURE"},
      {V4L2_CAP_SLICED_VBI_OUTPUT, "SLICED_VBI_OUTPUT"},
      {V4L2_CAP_RDS_CAPTURE, "RDS_CAPTURE"},
      {V4L2_CAP_VIDEO_OUTPUT_OVERLAY, "VIDEO_OUTPUT_OVERLAY"},
      {V4L2_CAP_HW_FREQ_SEEK, "HW_FREQ_SEEK"},
      {V4L2_CAP_RDS_OUTPUT, "RDS_OUTPUT"},
      {V4L2_CAP_VIDEO_CAPTURE_MPLANE, "VIDEO_CAPTURE_MPLANE"},
      {V4L2_CAP_VIDEO_OUTPUT_MPLANE, "VIDEO_OUTPUT_MPLANE"},
      {V4L2_CAP_VIDEO_M2M_MPLANE, "VIDEO_M2M_MPLANE"},
      {V4L2_CAP_VIDEO_M2M, "VIDEO_M2M"},
      {V4L2_CAP_STREAMING, "STREAMING"},
      {V4L2_CAP_READWRITE, "READWRITE"},
      {V4L2_CAP_TOUCH, "TOUCH"},
      {V4L2_CAP_DEVICE_CAPS, "DEVICE_CAPS"},
  };

  int first = 1;
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
    if (caps & entries[i].bit) {
      if (!first) {
        printf(", ");
      }
      printf("%s", entries[i].name);
      first = 0;
    }
  }
  if (first) {
    printf("(none)");
  }
  printf("\n");
}

static void dump_formats(int fd, enum v4l2_buf_type type, const char *label) {
  struct v4l2_fmtdesc fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = type;

  int count = 0;
  for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++) {
    if (count == 0) {
      printf("  %s formats:\n", label);
    }
    printf("    %s (%s)\n", fourcc_to_str(fmt.pixelformat), fmt.description);
    count++;
  }

  if (count == 0) {
    if (errno != EINVAL) {
      perror("VIDIOC_ENUM_FMT");
    }
  }
}

static void probe_device(const char *path) {
  int fd = open(path, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      return;
    }
  }

  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
    close(fd);
    return;
  }

  printf("Device: %s\n", path);
  printf("  name: %s\n", cap.card);
  printf("  driver: %s\n", cap.driver);
  printf("  bus: %s\n", cap.bus_info);
  printf("  version: %u.%u.%u\n", (cap.version >> 16) & 0xff,
         (cap.version >> 8) & 0xff, cap.version & 0xff);

  __u32 effective_caps = cap.capabilities;
  if (cap.capabilities & V4L2_CAP_DEVICE_CAPS) {
    effective_caps = cap.device_caps;
  }
  printf("  caps: ");
  print_caps(effective_caps);

  dump_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "VIDEO_OUTPUT");
  dump_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "VIDEO_OUTPUT_MPLANE");
  dump_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "VIDEO_CAPTURE");
  dump_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
               "VIDEO_CAPTURE_MPLANE");

  printf("\n");
  close(fd);
}

int main(void) {
  int found = 0;
  for (int i = 0; i < 32; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/video%d", i);
    if (access(path, F_OK) != 0) {
      continue;
    }
    found = 1;
    probe_device(path);
  }

  if (!found) {
    fprintf(stderr, "No /dev/video* devices found.\n");
    return 1;
  }

  return 0;
}

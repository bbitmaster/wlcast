CC ?= gcc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

CFLAGS ?= -O2 -g -Wall -Wextra -Wshadow -Wvla -Wconversion
CFLAGS += -I. -I../common -Igen

WAYLAND_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags wayland-client 2>/dev/null)
WAYLAND_LIBS ?= $(shell $(PKG_CONFIG) --libs wayland-client 2>/dev/null)
TURBOJPEG_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libturbojpeg 2>/dev/null)
TURBOJPEG_LIBS ?= $(shell $(PKG_CONFIG) --libs libturbojpeg 2>/dev/null)

CFLAGS += $(WAYLAND_CFLAGS) $(TURBOJPEG_CFLAGS)
LDLIBS += $(WAYLAND_LIBS) $(TURBOJPEG_LIBS) -lrt

# OpenCL support (requires libmali on device)
# Enable with: make OPENCL=1
ifdef OPENCL
CFLAGS += -DHAVE_OPENCL
LDLIBS += -lmali
OPENCL_SRC := opencl_convert.c
else
OPENCL_SRC :=
endif

GEN_DIR := gen

SCREENCOPY_XML := ../protocol/wlr-screencopy-unstable-v1.xml
SCREENCOPY_HEADER := $(GEN_DIR)/wlr-screencopy-unstable-v1-client-protocol.h
SCREENCOPY_CODE := $(GEN_DIR)/wlr-screencopy-unstable-v1-protocol.c

DMABUF_XML := ../protocol/wlr-export-dmabuf-unstable-v1.xml
DMABUF_HEADER := $(GEN_DIR)/wlr-export-dmabuf-unstable-v1-client-protocol.h
DMABUF_CODE := $(GEN_DIR)/wlr-export-dmabuf-unstable-v1-protocol.c

SRC := main.c capture.c capture_dmabuf.c compress.c udp.c v4l2_jpeg.c v4l2_rga.c $(OPENCL_SRC) $(SCREENCOPY_CODE) $(DMABUF_CODE)
OBJ := $(SRC:.c=.o)
BIN := wlcast-stream

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(GEN_DIR):
	mkdir -p $(GEN_DIR)

$(SCREENCOPY_HEADER): $(SCREENCOPY_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(SCREENCOPY_CODE): $(SCREENCOPY_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(DMABUF_HEADER): $(DMABUF_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(DMABUF_CODE): $(DMABUF_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $< $@

capture.o: $(SCREENCOPY_HEADER)
capture_dmabuf.o: $(DMABUF_HEADER)

clean:
	rm -f $(OBJ) $(BIN)
	rm -rf $(GEN_DIR)

CC ?= gcc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -g -Wall -Wextra -Wshadow -Wvla -Wconversion
CFLAGS += -I. -I../common

SDL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS ?= $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
TURBOJPEG_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libturbojpeg 2>/dev/null)
TURBOJPEG_LIBS ?= $(shell $(PKG_CONFIG) --libs libturbojpeg 2>/dev/null)

CFLAGS += $(SDL_CFLAGS) $(TURBOJPEG_CFLAGS)
LDLIBS += $(SDL_LIBS) $(TURBOJPEG_LIBS)

# Audio support (requires libopus)
# Enable with: make AUDIO=1
ifdef AUDIO
OPUS_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags opus 2>/dev/null)
OPUS_LIBS ?= $(shell $(PKG_CONFIG) --libs opus 2>/dev/null)
CFLAGS += -DHAVE_AUDIO $(OPUS_CFLAGS)
LDLIBS += $(OPUS_LIBS)
AUDIO_SRC := audio.c
else
AUDIO_SRC :=
endif

SRC := main.c network.c decode.c $(AUDIO_SRC)
OBJ := $(SRC:.c=.o)
BIN := wlcast-view

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(OBJ) $(BIN)

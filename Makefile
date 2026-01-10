.PHONY: all streamer viewer clean

all: streamer viewer

streamer:
	$(MAKE) -C streamer

viewer:
	$(MAKE) -C viewer

clean:
	$(MAKE) -C streamer clean
	$(MAKE) -C viewer clean

PKG_CONFIG ?= pkg-config
CC ?= cc
SDK ?= /opt/vim4-hdr-encoder
PACKAGES = gstreamer-video-1.0 gstreamer-allocators-1.0
CFLAGS += -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) -I$(SDK)/multiEnc/amvenc_lib/include
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -ldl

libgstvim4hevc10.so: gstvim4hevc10.c gstvim4h265src.c
	$(CC) $(CFLAGS) -shared -o $@ $^ $(LDLIBS)

test-dma-input: ../tests/hevc-main10-dma.c gstvim4hevc10.c gstvim4h265src.c
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-check-1.0) -o $@ $< gstvim4h265src.c $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-check-1.0)

test-h265-source: ../tests/h265-source.c gstvim4h265src.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

check: test-dma-input test-h265-source
	./test-dma-input
	./test-h265-source

clean:
	rm -f libgstvim4hevc10.so test-dma-input test-h265-source
.PHONY: clean check

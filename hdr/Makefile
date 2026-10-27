PKG_CONFIG ?= pkg-config
CC ?= cc
SDK ?= /opt/vim4-hdr-encoder
PACKAGES = gstreamer-video-1.0 gstreamer-allocators-1.0
CFLAGS += -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) -I$(SDK)/multiEnc/amvenc_lib/include
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -ldl

libgstvim4hevc10.so: gstvim4hevc10.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

test-dma-input: ../tests/hevc-main10-dma.c gstvim4hevc10.c
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-check-1.0) -o $@ $< $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-check-1.0)

check: test-dma-input
	./test-dma-input

clean:
	rm -f libgstvim4hevc10.so test-dma-input
.PHONY: clean check

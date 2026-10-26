PKG_CONFIG ?= pkg-config
CC ?= cc
SDK ?= /opt/vim4-hdr-encoder
PACKAGES = gstreamer-video-1.0
CFLAGS += -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) -I$(SDK)/multiEnc/amvenc_lib/include
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -ldl

libgstvim4hevc10.so: gstvim4hevc10.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

clean:
	rm -f libgstvim4hevc10.so
.PHONY: clean

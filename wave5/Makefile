CC ?= cc
PKG_CONFIG ?= pkg-config
PKGS = gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0
CPPFLAGS += -DPACKAGE=\"vim4-kvm\" $(shell $(PKG_CONFIG) --cflags $(PKGS))
CFLAGS += -O2 -g -Wall -Wextra -Werror -fPIC -MMD -MP
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))
SOURCES = gstvim4wave5.c gstvim4v4l2src.c gstvim4wave5enc.c nv12-planes.c

all: libgstvim4wave5.so test-planes test-encoder

libgstvim4wave5.so: $(SOURCES:.c=.o)
	$(CC) -shared -o $@ $^ $(LDLIBS)

test-planes: test-planes.o nv12-planes.o
	$(CC) -o $@ $^ $(LDLIBS)

test-encoder: test-encoder.o nv12-planes.o
	$(CC) -o $@ $^ $(LDLIBS)

test-encoder.o: CFLAGS += -Wno-unused-parameter

test: all
	./test-planes
	./test-encoder
	GST_PLUGIN_PATH=$(CURDIR) gst-inspect-1.0 vim4v4l2src >/dev/null
	GST_PLUGIN_PATH=$(CURDIR) gst-inspect-1.0 vim4wave5enc >/dev/null

clean:
	rm -f *.o *.d libgstvim4wave5.so test-planes test-encoder

-include $(SOURCES:.c=.d) test-planes.d test-encoder.d

.PHONY: all test clean

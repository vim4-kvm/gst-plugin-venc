PKG_CONFIG ?= pkg-config
CC ?= cc
PACKAGES = gstreamer-video-1.0 gstreamer-allocators-1.0
CFLAGS += -O2 -Wall -Wextra -Werror -fPIC $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))

libgstvim4jpegenc.so: gstvim4jpegenc.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

clean:
	rm -f libgstvim4jpegenc.so
.PHONY: clean

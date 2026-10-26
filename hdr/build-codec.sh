#!/bin/sh
# Build an already downloaded, pinned encoder SDK into a private runtime.
set -eu
[ "$#" = 2 ] || { echo "Usage: $0 SDK_DIRECTORY OUTPUT_DIRECTORY" >&2; exit 2; }
original=$(realpath "$1")
mkdir -p "$2/source"
output=$(realpath "$2")
sdk=$output/source
cp -a "$original/." "$sdk/"
patch -d "$sdk" -p1 < "$(dirname "$0")/p010-cpu-input.patch"
make -C "$sdk/multiEnc/vpuapi" CC="${CC:-cc}" -j"${JOBS:-4}"
make -C "$sdk/multiEnc/amvenc_lib" CC="${CC:-cc}" -j"${JOBS:-4}"
cp "$sdk/multiEnc/vpuapi/libamvenc_api.so" "$output/"
# Resolve the extended VPU ABI from this directory in the isolated namespace.
${CC:-cc} -shared -Wl,-soname,libvpcodec.so -Wl,-rpath,'$ORIGIN' \
    -o "$output/libvpcodec.so" "$sdk/multiEnc/amvenc_lib/obj/"*.o \
    -L"$sdk/multiEnc/vpuapi" -lamvenc_api -lge2d -lpthread -lm -lrt

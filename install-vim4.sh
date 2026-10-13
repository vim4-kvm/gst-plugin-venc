#!/bin/sh
set -eu
if [ "$(id -u)" != 0 ] || [ "$#" != 1 ]; then
    echo "usage: sudo $0 PATH_TO_LIBGSTAMLVENC_SO" >&2
    exit 1
fi
test -s "$1"
directory=/usr/local/lib/vim4-kvm/gstreamer-1.0
install -d "$directory" /etc/systemd/system/nanokvm.service.d
install -m 0755 "$1" "$directory/libgstamlvenc.so.new"
mv "$directory/libgstamlvenc.so.new" "$directory/libgstamlvenc.so"
cat > /etc/systemd/system/nanokvm.service.d/encoder.conf <<'EOF'
[Service]
Environment=GST_PLUGIN_PATH=/usr/local/lib/vim4-kvm/gstreamer-1.0
EOF
systemctl daemon-reload

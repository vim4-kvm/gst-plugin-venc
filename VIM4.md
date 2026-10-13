# VIM4 hardware encoder

The `vim4` branch builds the Amlogic encoder plugin from the official Khadas
source history. Retain the existing authors and copyright notices.

## Build and install

Build on the Ubuntu ARM64 board, with the image's libamvenc, GE2D and GStreamer
DRM allocator/bufferpool development files installed:

```sh
make -C amlvenc PKG_CONFIG=pkg-config CC=gcc -j4
sudo sh install-vim4.sh amlvenc/libgstamlvenc.so
sudo systemctl restart nanokvm
```

The service drop-in selects the plugin from
`/usr/local/lib/vim4-kvm/gstreamer-1.0`. The packaged vendor plugin is retained
at its original path. Future system images must include this build and drop-in,
as well as the vfmcapsrc plugin and board capture services. Application-only
updates preserve this platform prerequisite.

## Rate control

The `framerate` property accepts 0–120. Zero selects the negotiated input rate,
rounded to the codec's integer API (60000/1001 becomes 60), or 30 if unknown.
An explicit value sets the codec budget and per-frame metadata consistently.
Changing source caps reinitializes the encoder with the new budget.

vfm_cap currently supplies variable-rate caps. NanoKVM explicitly supplies the
HDMI receiver rate when it starts a pipeline, capped by its drop-only FPS limit.
Changes of HDMI rate without a caps change require reopening the pipeline.
Rate control changes neither capture pacing nor RTP timestamps.

For 360 identical 4K SMPTE test frames at a nominal 60fps and 12Mbps, encoding
with a 30fps budget produced 24.11Mbps; a 60fps budget produced 11.51Mbps.
Periodic keyframes changed from about 86KB to 66KB. Software test-pattern
generation is slower than real time on this board; these figures measure
bit allocation, not live capture throughput or latency.

Reproduce with `GST_PLUGIN_PATH=$PWD/amlvenc python3 tests/encoder-budget.py 30`
and then `60` (or `0` to exercise caps negotiation), with NanoKVM stopped.
The test opens the hardware encoder; do not run it concurrently with a viewer.
Run as khadas to share the encoder mutex namespace, and restart NanoKVM afterward.

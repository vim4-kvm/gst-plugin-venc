# VIM4 mainline GStreamer backend

`vim4v4l2src` discovers the unique `t7-vdin-test` node and exports leased NV12
DMA buffers. Its optional `height` property asks VDIN to downscale while preserving
aspect ratio. `input-rate` reports the HDMI rate in hundredths of a frame per second.

`vim4wave5enc` discovers the Wave5 encoder and selects H.264 or H.265 from downstream
caps. `bitrate` is in kbps; `gop=0` enables cyclic intra refresh, while positive GOP
values retain periodic keyframes. Force-key-unit events reach the V4L2 encoder.
Bitrate and GOP are configured before starting; restart the pipeline to change them.

The encoder views single-buffer NV12 as two DMA planes without copying pixels.
For 1080-line input, the UV import includes the allocator's verified tail padding.
The parent-buffer reference keeps the capture lease until encoding finishes.
Compressed output is copied to cached memory before NAL parsing and network delivery.

Build and offline checks: `make test` on Linux with GStreamer development packages.
Plane tests cover physical UV offsets, allocation bounds, metadata and lease retention.
Hardware tests still need the matched VIM4 kernel, firmware and the private patched
V4L2 plugin built by `packaging/kernel-edge/build-gstreamer.py` in the parent workspace.
Install these plugins in the mainline-only plugin directory; keep the vendor
`vfmcapsrc` / `amlvenc` factories for 5.15. Device numbers must never be hard-coded.

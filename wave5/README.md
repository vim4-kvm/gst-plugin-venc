# VIM4 mainline GStreamer backend

`vim4v4l2src` discovers the unique `t7-vdin-test` node and exports leased NV12
DMA buffers. Its optional `height` property asks VDIN to downscale while preserving
aspect ratio. `input-rate` reports the capture budget in hundredths of a frame per second.
For a fixed input it follows measured DV timings. Gaming VRR uses the read-only
`V4L2_CID_USER_T7_VDIN_VRR_RATE` control (Hz). A locked source with unknown base
uses a local 60 fps budget. Raw VRR caps carry `framerate=0/1` and `max-framerate`.
`max-fps` limits capture output only when needed. At the encoder input the DMA
adapter labels the firmware budget in whole fps while preserving buffer PTS.

`vim4wave5enc` discovers the Wave5 encoder and selects H.264 or H.265 from downstream
caps. `bitrate` is in kbps. At `gop=0`, AVC uses a 45-frame intra refresh period
and HEVC disables cyclic refresh. Both emit IDRs on request. Positive GOP values
retain periodic keyframes. Force-key-unit events reach the V4L2 encoder.
Bitrate and GOP are configured before starting; restart the pipeline to change them.
Encoder discovery requires a unique streaming M2M device with HEVC CAPTURE;
QUERYCAP names and sysfs labels are not used as capability signals. Visible-size
crop support is separately tested by the matched private V4L2 plugin, retaining
the legacy kernel path when that test fails.

The encoder views single-buffer NV12 as two DMA planes without copying pixels.
For 1080-line input, the UV import includes the allocator's verified tail padding.
The parent-buffer reference keeps the capture lease until encoding finishes.
Compressed output is copied to cached memory before NAL parsing and network delivery.

Build and offline checks: `make test` on Linux with GStreamer development packages.
Plane tests cover physical UV offsets, allocation bounds, metadata and lease retention.
Hardware tests still need the matched VIM4 kernel, firmware and the private patched
V4L2 plugin built by `packaging/kernel-edge/build-gstreamer.py` in the parent workspace.
Install the Wave5 and patched V4L2 plugins in the mainline plugin directory.
Install `libgstvim4rate.so` on the GStreamer search path for both 5.15 and 7.3.
`vim4ptsrate max-rate=N` uses capture PTS to limit delivery. It preserves lower fixed
fractions and publishes a maximum rate for variable input.

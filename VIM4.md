# VIM4 hardware encoder

This page covers encoder integration, DMA ownership and measurement limits.
Current rule: deploy matched platform components before applications; retain upstream authors and licenses.
Read [build](#build-and-install), [Main10](#hevc-main10-remote-hdr), [quality](#hevc-fixed-qp-and-lossless),
[JPEG](#continuous-hardware-jpeg), [rate control](#rate-control), or [shared HDR](#shared-remote-hdr-source).
Property ranges/defaults come from class_init in each linked implementation or gst-inspect-1.0.

## Build and install

Use [amlvenc Makefile](amlvenc/Makefile), [HDR Makefile](hdr/Makefile) and
[installer](install-vim4.sh) for local build/install inputs. The workspace
[package recipe](https://github.com/vim4-kvm/vim4-kvm/blob/main/packaging/ubuntu/package.py)
owns production paths and dependency contracts. Keep private plugins ahead of vendor system plugins;
do not overwrite the latter. Application-only updates cannot provide missing platform prerequisites.
Build the pinned SDK with [build-codec.sh](hdr/build-codec.sh), preserving its notices;
install matched capture, GPU runtime and common launcher before applications.

## HEVC Main10 remote HDR

[Main10 encoder](hdr/gstvim4hevc10.c) preserves input depth and colorimetry.
[Shared source](hdr/gstvim4h265src.c) delegates HDR scaling to capture's Mali/P010 path,
avoiding CPU conversion/upload. SDR scaling instead uses GE2D with visible width distinct from pitch:
VDIN's canvas rounding can otherwise distort ultrawide aspect ratio. Never upscale.

Keep the extended codec ABI in its private linker namespace with matching libraries.
The [SDK patch](hdr/p010-cpu-input.patch) handles little-endian CPU P010 without mutating caller memory
and rounds hardware dimensions while preserving visible cropping. Unaligned widths previously stalled first encode
and teardown; application-side size acceptance alone does not prove support.
CPU input requires contiguous planes; DMA additionally requires a physically contiguous allocation
with the layout validated by the encoder. Arbitrary scatter-gather DMA cannot be imported.
Unsupported DMA layout may use CPU fallback only when its plane contract is satisfied.
VideoMeta supplies padded pitch rather than visible width.

Retain DMA input until the SDK explicitly retires its fd, not merely until an output packet arrives.
On stop, format change or error, destroy the encoder before returning outstanding pool slots.
Timing instrumentation is defined in the encoder; encode-call duration excludes mapping and output copies,
and peak in-flight count includes the submitted input before retirement.

The HDR Makefile check target uses a fake codec with real GStreamer memory to cover retirement,
unmappable DMA, pitch, fallback and cleanup. With the companion capture plugin it also parses the real bin;
a skipped integration case is not a pass. It tests no hardware DMA import, cache coherency or pixel fidelity.
[Main10 hardware diagnostic](tests/hevc-main10.py) checks both ABIs, reopen, crop, pixels and bitstream metadata.
UHD needs the driver's configured encoder memory pool; forcing the old smaller allocation prevents task-buffer creation.
[Stride diagnostic](tests/encoder-stride.py) covers padded eight-bit AVC/HEVC input.

## HEVC fixed QP and lossless

Controls and precedence live in [gst_vim4_hevc10_class_init](hdr/gstvim4hevc10.c).
Fixed QP and lossless bypass bitrate control. Lossless preserves encoder-input YUV,
including ten-bit samples; it cannot undo HDMI conversion, subsampling or scaling.
Neither mode guarantees live throughput.
[Quality diagnostic](tests/hevc-quality.py) checks decoded bytes, profiles, metadata and actual slice QP.
Keep ordinary DMA lifecycle checks as well; pixel fidelity alone cannot prove safe reuse.

## Continuous hardware JPEG

[JPEG encoder](jpeg/vim4/gstvim4jpegenc.c) uses direct driver ioctls.
Import/release each DMA input explicitly: the vendor per-command path leaked attachments and pinned capture buffers.
On timeout stop encoding before returning capture memory; validate output against mapped capacity.
System memory is diagnostic fallback, while live capture/scaling remains DMA.
[Hardware diagnostic](tests/jpeg-hardware.py) exercises sustained output, quality/size changes and reopen.
Scaling tests need the application's compatible allocator. Encoder throughput is separate from network/client limits;
never benchmark concurrently with another KVM viewer.

## Chroma capability

The [vendor encoder](amlvenc/gstamlvenc.c) produces eight-bit 4:2:0.
RGB acceptance does not imply full-chroma output: unsupported profile requests previously negotiated
while SPS still reported ordinary 4:2:0. Check the actual bitstream.
[Chroma diagnostic](tests/encoder-chroma.py) verifies rejected profiles and RGB output with ffprobe.
This bounds the tested API/firmware, not every possible silicon mode.

## Rate control

[gstamlvenc.c](amlvenc/gstamlvenc.c) owns rate budgets, caps reinitialization and property bounds.
Budget is distinct from capture pacing and RTP timestamps. Variable-rate source caps cannot reveal
a changed HDMI rate to the encoder: consumers must supply and update the stable budget.
A mismatched lower budget was measured to over-allocate bits; test-pattern results measure bit allocation,
not live throughput or latency. Prior measurements are in the workspace
[bringup index](https://github.com/vim4-kvm/vim4-kvm/blob/main/docs/bringup/README.md).

[Budget diagnostic](tests/encoder-budget.py) owns options and JSON fields.
Stop KVM viewers, run as the normal encoder user to share the mutex namespace, and restart the application afterward.
For an experimental higher ceiling, use an isolated build and registry rather than replacing the installed plugin.
The test rejects targets above the loaded property's maximum; upgrade plugin and application together.
Use real HDMI motion for throughput; accepting a target neither produces that bitrate on static content
nor establishes a hardware ceiling. Arrival intervals include host scheduling and optional file I/O,
not browser presentation or end-to-end latency.

## Shared remote HDR source

[HDR source](hdr/gstvim4h265src.c) qualifies Wave5 using actual AMLY capture, HDR controls,
P010/Main10 negotiation and VUI rather than kernel name or profile enumeration alone.
The bufferless probe starts no stream and proves no board performance.
Preserve pitch, padded UV storage and buffer lease; reject silent format substitution.
Insert current HDR10 metadata before random access or metadata changes, preserving unrelated SEI;
drop native access units without fresh metadata, while explicit zero metadata remains valid.

Request the raw pad before READY. Native P010 must pass GPU tone mapping before SDR JPEG;
never relabel PQ as SDR or open a second VDIN. Bound raw queues and put an idle screenshot valve before its queue.
Release the entire bin before mode changes, including policy-triggered reopen errors.
Vendor native HDR keeps independent JPEG; vendor SDR shares one source.
Reserve encoder memory before capture pools and release the reservation only when SDK ownership is established.
Capabilities and policy properties can be queried without starting hardware;
eight-bit controls must not be assumed to configure Main10.

Offline negotiation/SEI/cleanup tests do not qualify all applications or GPU stacks.
HLG and mainline Main10 remain outside the qualified vendor path; mainline integration is still a board gate.
Use the workspace evidence index before deployment.

## Quality ceiling (VBR)

Vendor rate control targets bitrate but is not strict CBR. Capping QP holds quality by permitting overshoot;
configure before streaming and respect the I-frame floor.
Strict skip-window experiments dropped frames without meeting the bitrate target, so those fields are not exposed.
Old per-candidate tables are omitted; compare the same moving workload when making a new decision.

## Experimental keyframe controls

Property contracts live in [gstamlvenc.c](amlvenc/gstamlvenc.c); application choices belong to their own pipeline code.
An I-frame QP floor trades quality for size, without a byte-size guarantee.
Row refresh effectiveness, visual quality, new-viewer startup and PLI recovery need separate measurement;
do not infer reference-chain recovery from refreshed rows.
The vendor special-GOP P-frame delta is not an I-frame size control.
Use the budget diagnostic's keyframe indices/sizes and saved Annex B output for independent decode checks,
then verify browser recovery on the actual application.

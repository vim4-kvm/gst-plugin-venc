#!/usr/bin/env python3
"""Exclusive board test: continuous DMA JPEG, quality changes and pipeline teardown.

Stop the selected KVM first. Select the test plugin with GST_PLUGIN_PATH; for
GE2D scaling also use One-KVM's compatible allocator via LD_LIBRARY_PATH.
"""
import argparse
import json
import subprocess
import time
from pathlib import Path
import gi

gi.require_version('Gst', '1.0')
gi.require_version('GstApp', '1.0')
from gi.repository import Gst, GstApp


def measure(quality, height, frames, directory):
    scale = (f'amlvconv ! video/x-raw,format=NV12,width={height * 16 // 9},height={height} ! '
             if height != 2160 else '')
    pipeline = Gst.parse_launch(
        f'vfmcapsrc ! {scale}queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! '
        f'vim4jpegenc quality={quality} ! '
        'appsink name=frames sync=false max-buffers=1 drop=true')
    sink = pipeline.get_by_name('frames')
    rows = []
    try:
        pipeline.set_state(Gst.State.PLAYING)
        for index in range(frames):
            sample = sink.try_pull_sample(3 * Gst.SECOND)
            if sample is None:
                message = pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR)
                raise RuntimeError(str(message.parse_error()) if message else 'No JPEG frame')
            buffer = sample.get_buffer()
            jpeg = buffer.extract_dup(0, buffer.get_size())
            assert jpeg[:2] == b'\xff\xd8' and jpeg[-2:] == b'\xff\xd9', 'Incomplete JPEG'
            caps = sample.get_caps().get_structure(0)
            assert caps.get_value('height') == height
            rows.append((time.monotonic(), len(jpeg)))
            if index in (0, frames - 1):
                (directory / f'{height}-q{quality}-{index}.jpg').write_bytes(jpeg)
            sample = buffer = None
    finally:
        pipeline.set_state(Gst.State.NULL)
    rows = rows[10:]
    elapsed = rows[-1][0] - rows[0][0]
    result = dict(height=height, quality=quality, frames=frames,
                  fps=(len(rows)-1)/elapsed,
                  mbps=sum(size for _, size in rows[1:])*8/elapsed/1e6,
                  max_gap_ms=max(b[0]-a[0] for a, b in zip(rows, rows[1:]))*1000)
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exclusive', action='store_true', required=True)
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--output', type=Path, default=Path('/tmp/vim4-jpeg-check'))
    args = parser.parse_args()
    if args.frames < 30:
        parser.error('at least 30 frames required')
    args.output.mkdir(exist_ok=True, parents=True)
    Gst.init(None)
    for height, quality in [(2160, 50), (2160, 80), (2160, 100), (1080, 80), (720, 80), (2160, 80)]:
        measure(quality, height, args.frames, args.output)
    for image in args.output.glob('*.jpg'):
        subprocess.run(['gst-launch-1.0', '-q', 'filesrc', f'location={image}', '!',
                        'jpegdec', '!', 'fakesink'], check=True, timeout=5)
    print('PASS: saved JPEG frames decoded; repeated pipeline teardown/reopen completed.', flush=True)

#!/usr/bin/python3
"""Measure bit allocation with NanoKVM stopped; software patterns are not real-time."""
import argparse
import json
import time

import gi

gi.require_version('Gst', '1.0')
from gi.repository import Gst


def measure(rate, pattern):
    Gst.init(None)
    source = ('vfmcapsrc' if pattern == 'hdmi' else
              f'videotestsrc num-buffers=360 pattern={pattern} ! '
              'video/x-raw,format=NV12,width=3840,height=2160,framerate=60/1')
    pipeline = Gst.parse_launch(
        source + f' ! amlvenc bitrate=12000 gop=120 framerate={rate} ! '
        'video/x-h264 ! h264parse ! appsink name=out sync=false')
    sink = pipeline.get_by_name('out')
    rows = []
    pipeline.set_state(Gst.State.PLAYING)
    try:
        for _ in range(360):
            sample = sink.emit('try-pull-sample', 10 * Gst.SECOND)
            if sample is None:
                message = pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR)
                raise RuntimeError(str(message.parse_error()) if message else 'No sample')
            buffer = sample.get_buffer()
            rows.append(dict(size=buffer.get_size(), pts=buffer.pts,
                             key=not buffer.has_flags(Gst.BufferFlags.DELTA_UNIT),
                             time=time.monotonic_ns()))
    finally:
        pipeline.set_state(Gst.State.NULL)
    duration = (rows[-1]['pts'] - rows[0]['pts']) / 1e9
    return dict(fps_setting=rate, pattern=pattern, frames=len(rows),
                mbps=sum(row['size'] for row in rows[1:]) * 8 / duration / 1e6,
                keys=[row['size'] for row in rows if row['key']],
                elapsed=(rows[-1]['time'] - rows[0]['time']) / 1e9)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rate', type=int, choices=(0, 30, 60))
    parser.add_argument('--pattern', choices=('smpte', 'ball', 'hdmi'), default='smpte')
    args = parser.parse_args()
    print(json.dumps(measure(args.rate, args.pattern)))

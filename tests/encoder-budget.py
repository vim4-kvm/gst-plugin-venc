#!/usr/bin/python3
"""Measure bit allocation with NanoKVM stopped; software patterns are not real-time."""
import argparse
import json
import time
from contextlib import nullcontext

import gi

gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0')
from gi.repository import Gst, GstVideo


def measure(rate, pattern, frames=360, gop=120, i_qp_min=-1, intra_refresh_rows=0,
            output=None, request_idr=False, qp_max=-1, bitrate=12000, codec='h264'):
    Gst.init(None)
    encoder = Gst.ElementFactory.make('amlvenc')
    if encoder is None:
        raise RuntimeError('amlvenc plugin is unavailable')
    maximum = encoder.find_property('bitrate').maximum
    if bitrate > maximum:
        raise ValueError(f'Loaded plugin supports at most {maximum} kbps; use an experimental build')
    source = ('vfmcapsrc' if pattern == 'hdmi' else
              f'videotestsrc num-buffers={frames} pattern={pattern} ! '
              'video/x-raw,format=NV12,width=3840,height=2160,framerate=60/1')
    pipeline = Gst.parse_launch(
        source + f' ! amlvenc bitrate={bitrate} gop={gop} framerate={rate} '
        f'i-qp-min={i_qp_min} intra-refresh-rows={intra_refresh_rows} qp-max={qp_max} ! '
        f'video/x-{codec} ! {codec}parse config-interval=-1 ! '
        f'video/x-{codec},stream-format=byte-stream,alignment=au ! appsink name=out sync=false max-buffers=2')
    sink = pipeline.get_by_name('out')
    rows = []
    pipeline.set_state(Gst.State.PLAYING)
    try:
        with open(output, 'wb') if output else nullcontext() as encoded:
            for index in range(frames):
                if request_idr and index == frames // 2:
                    event = GstVideo.video_event_new_upstream_force_key_unit(Gst.CLOCK_TIME_NONE, True, 0)
                    if not sink.send_event(event):
                        raise RuntimeError('Encoder rejected IDR request')
                sample = sink.emit('try-pull-sample', 10 * Gst.SECOND)
                if sample is None:
                    message = pipeline.get_bus().pop_filtered(Gst.MessageType.ERROR)
                    raise RuntimeError(str(message.parse_error()) if message else 'No sample')
                buffer = sample.get_buffer()
                caps = sample.get_caps().to_string()
                rows.append(dict(size=buffer.get_size(), pts=buffer.pts,
                                 key=not buffer.has_flags(Gst.BufferFlags.DELTA_UNIT),
                                 time=time.monotonic_ns()))
                if encoded:
                    encoded.write(buffer.extract_dup(0, buffer.get_size()))
    finally:
        pipeline.set_state(Gst.State.NULL)
    duration = (rows[-1]['pts'] - rows[0]['pts']) / 1e9
    intervals = sorted((b['time'] - a['time']) / 1e6 for a, b in zip(rows[30:], rows[31:]))
    # Peak bitrate over one-second windows of media time, excluding the first IDR.
    window, peak = [], 0
    for row in rows[1:]:
        window.append(row)
        while window[-1]['pts'] - window[0]['pts'] >= Gst.SECOND:
            window.pop(0)
        peak = max(peak, sum(r['size'] for r in window) * 8 / 1e6)
    return dict(codec=codec, caps=caps, fps_setting=rate, bitrate_kbps=bitrate, pattern=pattern, frames=len(rows), gop=gop,
                measured_fps=(len(rows) - 1) * 1e9 / (rows[-1]['time'] - rows[0]['time']),
                i_qp_min=i_qp_min, intra_refresh_rows=intra_refresh_rows, qp_max=qp_max,
                peak_1s_mbps=peak,
                interval_p99_ms=intervals[int((len(intervals)-1)*.99)],
                interval_max_ms=max(intervals),
                mbps=sum(row['size'] for row in rows[1:]) * 8 / duration / 1e6,
                keys=[row['size'] for row in rows if row['key']],
                key_indices=[i for i, row in enumerate(rows) if row['key']],
                elapsed=(rows[-1]['time'] - rows[0]['time']) / 1e9)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rate', type=int, choices=(0, 30, 60))
    parser.add_argument('--pattern', choices=('smpte', 'ball', 'hdmi'), default='smpte')
    parser.add_argument('--frames', type=int, default=360)
    parser.add_argument('--gop', type=int, default=120)
    parser.add_argument('--i-qp-min', type=int, default=-1)
    parser.add_argument('--intra-refresh-rows', type=int, default=0)
    parser.add_argument('--output', help='Save Annex B video for independent decoder validation')
    parser.add_argument('--codec', choices=('h264', 'h265'), default='h264')
    parser.add_argument('--request-idr', action='store_true')
    parser.add_argument('--qp-max', type=int, default=-1)
    parser.add_argument('--bitrate', type=int, default=12000)
    args = parser.parse_args()
    if not (32 <= args.frames <= 36000 and 1 <= args.gop <= 1000 and
            -1 <= args.i_qp_min <= 51 and 0 <= args.intra_refresh_rows <= 135 and
            -1 <= args.qp_max <= 51 and 1000 <= args.bitrate <= 100000):
        parser.error('invalid experiment bounds')
    print(json.dumps(measure(args.rate, args.pattern, args.frames, args.gop,
                            args.i_qp_min, args.intra_refresh_rows, args.output, args.request_idr,
                            args.qp_max, args.bitrate, args.codec)))

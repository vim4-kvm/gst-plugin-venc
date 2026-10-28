#!/usr/bin/env python3
"""Exclusive hardware test: decoded lossless samples and fixed-QP quality changes."""
import argparse
import array
import json
from pathlib import Path
import random
import re
import subprocess
import tempfile

import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exclusive', required=True, action='store_true')
    parser.parse_args()
    Gst.init(None)
    width, height, frames = 640, 384, 5
    with tempfile.TemporaryDirectory(prefix='hevc-quality-') as directory:
        for fmt, pix in [('NV12', 'nv12'), ('NV21', 'nv21'), ('P010_10LE', 'p010le')]:
            rng = random.Random(152)
            count = width * height * 3 // 2
            samples = [rng.randrange(64, 940) if fmt == 'P010_10LE' else rng.randrange(16, 236) for _ in range(count)]
            raw = array.array('H', (v << 6 for v in samples)).tobytes() if fmt == 'P010_10LE' else bytes(samples)
            sizes = []
            for mode in ['lossless=true', 'fixed-qp=18', 'fixed-qp=40']:
                path = Path(directory) / 'stream.h265'
                color = 'bt2100-pq' if fmt == 'P010_10LE' else 'bt709'
                pipeline = Gst.parse_launch('appsrc name=input format=time ! '
                    f'video/x-raw,format={fmt},width={width},height={height},framerate=60/1,colorimetry={color} ! '
                    f'vim4hevc10enc {mode} ! h265parse ! filesink location={path}')
                try:
                    pipeline.set_state(Gst.State.PLAYING)
                    source = pipeline.get_by_name('input')
                    for i in range(frames):
                        buffer = Gst.Buffer.new_wrapped(raw)
                        buffer.pts = i * Gst.SECOND // 60
                        buffer.duration = Gst.SECOND // 60
                        assert source.emit('push-buffer', buffer) == Gst.FlowReturn.OK
                    source.emit('end-of-stream')
                    message = pipeline.get_bus().timed_pop_filtered(30 * Gst.SECOND, Gst.MessageType.ERROR | Gst.MessageType.EOS)
                    assert message is not None, 'timeout'
                    assert message.type == Gst.MessageType.EOS, message.parse_error()
                finally:
                    pipeline.set_state(Gst.State.NULL)
                decoded = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(path), '-pix_fmt', pix, '-f', 'rawvideo', '-'])
                stream = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(path)]))['streams'][0]
                assert stream['profile'] == ('Main 10' if fmt == 'P010_10LE' else 'Main')
                assert stream['color_transfer'] == ('smpte2084' if fmt == 'P010_10LE' else 'bt709')
                assert len(decoded) == len(raw) * frames, (fmt, mode, len(decoded))
                error = sum(a != b for a, b in zip(raw * frames, decoded))
                if mode == 'lossless=true':
                    assert error == 0, (fmt, 'lossless pixel mismatch', error)
                else:
                    trace = subprocess.run(['ffmpeg', '-v', 'trace', '-i', str(path),
                        '-c:v', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
                        check=True, capture_output=True, text=True).stderr
                    initial = [int(v) + 26 for v in re.findall(r'\binit_qp_minus26\s+[^\n]*= (-?\d+)', trace)]
                    delta = [int(v) for v in re.findall(r'\bslice_qp_delta\s+[^\n]*= (-?\d+)', trace)]
                    assert initial and len(set(initial)) == 1 and len(delta) == frames, (fmt, initial, delta)
                    assert all(initial[0] + v == int(mode.split('=')[1]) for v in delta), (fmt, initial, delta)
                sizes.append(path.stat().st_size)
                print(json.dumps({'format': fmt, 'mode': mode, 'bytes': sizes[-1], 'different_bytes': error}), flush=True)
            assert sizes[1] > sizes[2], (fmt, sizes)


if __name__ == '__main__':
    main()

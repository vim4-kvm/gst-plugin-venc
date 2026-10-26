#!/usr/bin/env python3
"""Exclusive hardware regression: Main10 samples, VUI, ABI coexistence and reopen.

Run as the encoder user with all KVM applications stopped. Requires PyGObject,
ffmpeg/ffprobe and GST_PLUGIN_PATH selecting the candidate plugins.
"""
import argparse
import array
import json
import math
from pathlib import Path
import subprocess
import tempfile

import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst


def run_pipeline(pipeline, raw=None, frames=30):
    try:
        assert pipeline.set_state(Gst.State.PLAYING) != Gst.StateChangeReturn.FAILURE
        buffers = []
        if raw is not None:
            source = pipeline.get_by_name('input')
            for index in range(frames):
                buffer = Gst.Buffer.new_wrapped(raw)
                buffer.pts = index * Gst.SECOND // 60
                buffer.duration = Gst.SECOND // 60
                buffers.append(buffer)
                assert source.emit('push-buffer', buffer) == Gst.FlowReturn.OK
            source.emit('end-of-stream')
        message = pipeline.get_bus().timed_pop_filtered(
            30 * Gst.SECOND, Gst.MessageType.ERROR | Gst.MessageType.EOS)
        assert message is not None, 'encoder timeout'
        assert message.type == Gst.MessageType.EOS, message.parse_error()
        # Some vendor CPU-copy paths swap the caller's read-only input in place.
        assert all(buffer.extract_dup(0, len(raw)) == raw for buffer in buffers), 'input modified'
    finally:
        pipeline.set_state(Gst.State.NULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exclusive', action='store_true', required=True)
    args = parser.parse_args()
    assert args.exclusive
    Gst.init(None)
    with tempfile.TemporaryDirectory(prefix='vim4-main10-') as directory:
        directory = Path(directory)
        for index, width in enumerate([640, 656, 640]):
            # Exercise the system eight-bit ABI before every private Main10 init.
            run_pipeline(Gst.parse_launch('videotestsrc num-buffers=5 ! '
                'video/x-raw,format=NV12,width=640,height=384,framerate=60/1 ! '
                'amlvenc bitrate=8000 framerate=60 ! video/x-h265 ! fakesink'))
            height = 384
            y = [64 + x * 876 // (width - 1) for _ in range(height) for x in range(width)]
            uv = [value for row in range(height // 2) for x in range(width // 2)
                  for value in (256 + x * 512 // (width // 2 - 1), 640 - row)]
            samples = y + uv
            raw = array.array('H', (value << 6 for value in samples)).tobytes()
            output = directory / f'{index}.h265'
            run_pipeline(Gst.parse_launch('appsrc name=input format=time ! '
                f'video/x-raw,format=P010_10LE,width={width},height={height},'
                'framerate=60/1,colorimetry=bt2100-pq ! '
                'vim4hevc10enc bitrate=8000 framerate=60 ! h265parse ! '
                f'filesink location={output}'), raw)
            stream = json.loads(subprocess.check_output(['ffprobe', '-v', 'error',
                '-count_frames', '-show_streams', '-of', 'json', str(output)]))['streams'][0]
            assert (stream['profile'], stream['pix_fmt'], stream['color_space'],
                    stream['color_transfer'], stream['color_primaries']) == (
                    'Main 10', 'yuv420p10le', 'bt2020nc', 'smpte2084', 'bt2020'), stream
            assert int(stream['nb_read_frames']) == 30, stream
            decoded = array.array('H', subprocess.check_output(['ffmpeg', '-v', 'error',
                '-i', str(output), '-frames:v', '1', '-pix_fmt', 'p010le', '-f', 'rawvideo', '-']))
            actual = [value >> 6 for value in decoded]
            assert len(samples) == len(actual)
            psnr = []
            for start, stop in [(0, width * height), (width * height, len(samples))]:
                mse = sum((a-b)**2 for a,b in zip(samples[start:stop], actual[start:stop])) / (stop-start)
                psnr.append(10 * math.log10(1023**2 / max(mse, 1e-12)))
            assert min(psnr) > 42, (width, psnr)
            print(json.dumps({'width': width, 'frames': 30, 'y_uv_psnr_db': psnr}), flush=True)


if __name__ == '__main__':
    main()

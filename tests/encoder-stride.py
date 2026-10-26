#!/usr/bin/env python3
"""Exclusive hardware regression: padded NV21 rows must survive AVC/HEVC encoding."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exclusive', action='store_true', required=True)
    parser.parse_args()
    import gi
    gi.require_version('Gst', '1.0')
    gi.require_version('GstVideo', '1.0')
    from gi.repository import Gst, GstVideo
    Gst.init(None)
    width, height, pitch = 656, 384, 704
    expected = bytes(32 + (x // 32 + y // 24) % 12 * 16
                     for y in range(height) for x in range(width))
    raw = bytearray([235]) * (pitch * height) + bytearray([128]) * (pitch * height // 2)
    for y in range(height):
        raw[y*pitch:y*pitch+width] = expected[y*width:(y+1)*width]
    with tempfile.TemporaryDirectory(prefix='encoder-stride-') as directory:
        for codec in ('h264', 'h265'):
            output = Path(directory) / f'padded.{codec}'
            pipeline = Gst.parse_launch('appsrc name=input format=time ! '
                f'video/x-raw,format=NV21,width={width},height={height},framerate=60/1,colorimetry=bt709 ! '
                f'amlvenc bitrate=8000 framerate=60 ! video/x-{codec} ! filesink location={output}')
            try:
                assert pipeline.set_state(Gst.State.PLAYING) != Gst.StateChangeReturn.FAILURE
                source = pipeline.get_by_name('input')
                for index in range(10):
                    buffer = Gst.Buffer.new_wrapped(bytes(raw))
                    GstVideo.buffer_add_video_meta_full(buffer, GstVideo.VideoFrameFlags.NONE,
                        GstVideo.VideoFormat.NV21, width, height, 2,
                        [0, pitch*height, 0, 0], [pitch, pitch, 0, 0])
                    buffer.pts = index * Gst.SECOND // 60
                    buffer.duration = Gst.SECOND // 60
                    assert source.emit('push-buffer', buffer) == Gst.FlowReturn.OK
                source.emit('end-of-stream')
                message = pipeline.get_bus().timed_pop_filtered(20*Gst.SECOND,
                    Gst.MessageType.ERROR | Gst.MessageType.EOS)
                assert message is not None, 'encoder timeout'
                assert message.type == Gst.MessageType.EOS, message.parse_error()
            finally:
                pipeline.set_state(Gst.State.NULL)
            decoded = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(output),
                '-frames:v', '1', '-pix_fmt', 'yuv420p', '-f', 'rawvideo', '-'])
            assert len(decoded) == width*height*3//2
            mae = sum(abs(a-b) for a,b in zip(expected, decoded[:width*height])) / len(expected)
            assert mae < 4, (codec, mae)
            print(f'{codec}: padded {width}x{height} pitch={pitch}, decoded Y MAE={mae:.3f}', flush=True)


if __name__ == '__main__':
    main()

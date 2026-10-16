#!/usr/bin/env python3
"""Exclusive board regression: requested profiles must agree with real chroma.

Run as the normal encoder user with all KVM applications stopped. Select the
candidate using GST_PLUGIN_PATH and the compatible allocator via LD_LIBRARY_PATH.
"""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exclusive', action='store_true')
    args = parser.parse_args()
    if not args.exclusive:
        parser.error('requires --exclusive after stopping other encoder users')

    import gi
    gi.require_version('Gst', '1.0')
    from gi.repository import Gst
    Gst.init(None)
    factory = Gst.ElementFactory.find('amlvenc')
    if factory is None:
        raise RuntimeError('amlvenc is unavailable')
    source_caps = next(p.get_caps() for p in factory.get_static_pad_templates()
                       if p.direction == Gst.PadDirection.SRC)
    for media, profile in [('h264', 'high-4:4:4'), ('h264', 'high-4:2:2'),
                           ('h264', 'high-10'), ('h265', 'main-444'), ('h265', 'main-10')]:
        requested = Gst.Caps.from_string(f'video/x-{media},profile={profile}')
        if source_caps.can_intersect(requested):
            raise RuntimeError(f'unsupported profile advertised: {requested.to_string()}')

    results = {}
    with tempfile.TemporaryDirectory(prefix='encoder-chroma-') as directory:
        for codec in ('h264', 'h265'):
            output = Path(directory) / f'rgb.{codec}'
            subprocess.run([
                'gst-launch-1.0', '-q', 'videotestsrc', 'num-buffers=30', 'pattern=ball',
                '!', 'video/x-raw,format=RGB,width=1920,height=1080,framerate=60/1',
                '!', 'amlvenc', 'bitrate=20000', 'framerate=60',
                '!', f'video/x-{codec}', '!', 'filesink', f'location={output}',
            ], check=True, timeout=30)
            result = json.loads(subprocess.check_output([
                'ffprobe', '-v', 'error', '-show_entries',
                'stream=codec_name,profile,pix_fmt,width,height', '-of', 'json', str(output),
            ], timeout=10))['streams'][0]
            if result['pix_fmt'] != 'yuv420p':
                raise RuntimeError(f'unexpected output format: {result}')
            results[codec] = result
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()

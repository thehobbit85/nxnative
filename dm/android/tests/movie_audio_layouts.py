#!/usr/bin/env python3
"""Generate synthetic AAC-LC channel probes, or compare their guest output.

Run 'generate DIR', then run t_android_movie_aac OUT.pcm tones-N.aac on the
Mac and Android hosts. Save their guest captures in DIR as mac-N.pcm and
android-N.pcm respectively, then run 'compare DIR'. Requires FFmpeg to generate.
"""
from array import array
import argparse
import math
from pathlib import Path
import subprocess


# ADTS configurations 1..7, with the FFmpeg speaker indices in the order
# Apple exposes to movie::Buffer. Each speaker carries a distinct sine tone.
CASES = {
    1: ("mono", [443], [0]),
    2: ("stereo", [443, 709], [0, 1]),
    3: ("3.0", [443, 709, 997], [2, 0, 1]),
    4: ("4.0", [443, 709, 997, 1207], [2, 0, 1, 3]),
    5: ("5.0", [443, 709, 997, 1301, 1607], [2, 0, 1, 3, 4]),
    6: ("5.1", [443, 709, 997, 65, 1301, 1607], [2, 0, 1, 4, 5, 3]),
    8: ("7.1", [443, 709, 997, 65, 1301, 1607, 1867, 2099], [2, 0, 1, 6, 7, 4, 5, 3]),
}


def generate(root):
    root.mkdir(parents=True, exist_ok=True)
    ffmpeg = ["ffmpeg", "-nostdin", "-loglevel", "error", "-y"]
    for ch, (layout, tones, _) in CASES.items():
        rate = 44100 if ch == 1 else 48000
        pcm = array("h", (round(10000 * math.sin(i*f*2*math.pi/rate)) for i in range(32768) for f in tones))
        stream = root / f"tones-{ch}.aac"
        subprocess.run([*ffmpeg, "-f", "s16le", "-ar", str(rate), "-ac", str(ch), "-channel_layout", layout,
                        "-i", "pipe:0", "-c:a", "aac", "-b:a", str(ch*48000), "-f", "adts", stream],
                       input=pcm.tobytes(), check=True)
        subprocess.run([*ffmpeg, "-i", stream, "-f", "s16le", root / f"ref-{ch}.pcm"], check=True)


def snr(signal, actual):
    error = sum((x-y)**2 for x, y in zip(signal, actual))
    return 10*math.log10(sum(x*x for x in signal)/error) if error else math.inf


def compare(root):
    for ch, (_, _, order) in CASES.items():
        ref, mac, android = (array("h", (root / f"{host}-{ch}.pcm").read_bytes()) for host in ("ref", "mac", "android"))
        assert len(ref) == len(mac) == len(android) == 33*1024*ch
        scores = []
        for i, speaker in enumerate(order):
            values, apple, mobile = ref[speaker::ch], mac[i::ch], android[i::ch]
            scores.extend((snr(values, mobile), snr(values, apple), snr(apple, mobile)))
        print(f"{ch} channels: worst per-channel SNR {min(scores):.1f} dB")
        assert min(scores) >= 35, "guest speaker order or PCM differs from reference"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["generate", "compare"])
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    (generate if args.action == "generate" else compare)(args.directory)

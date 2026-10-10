#!/usr/bin/env python3
"""Converts the radio spots in radio-stories/ into files suited to the
DY-SV8F module - one spot per installation device, each device's module
holding its spot as its only file, 00000.mp3 - using the same settings as
the companion tutrakan-telestories project:

- Two-pass loudness-normalized to a common -16 LUFS integrated / -1.5 dBTP
  level, so every spot plays back equally loud. The sources are mastered
  hot (-11..-13 LUFS mono) with true peaks above 0 dBFS, so this also removes
  their clipping.
- Mono, 22050 Hz, 64 kbps CBR MP3, to ease load on the module.
- Metadata stripped.

Outputs go to audio_output/device-<n>/00000.mp3, <n> taken from the
source filename (Spot <n>). Re-run any time a source file is replaced.
"""
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "radio-stories"
OUT = ROOT / "audio_output"

TARGET_LUFS = -16
TARGET_TP = -1.5
TARGET_LRA = 11

SAMPLE_RATE = 22050
BITRATE = "64k"

MONO_DOWNMIX = "pan=mono|c0=0.5*c0+0.5*c1"


def run(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)}\n{result.stderr}")
    return result.stderr


def measure_loudness(input_path, downmix=True):
    prefix = f"{MONO_DOWNMIX}," if downmix else ""
    filter_chain = f"{prefix}loudnorm=I={TARGET_LUFS}:TP={TARGET_TP}:LRA={TARGET_LRA}:print_format=json"
    stderr = run(["ffmpeg", "-y", "-i", str(input_path), "-af", filter_chain, "-f", "null", "-"])
    start, end = stderr.rfind("{"), stderr.rfind("}")
    return json.loads(stderr[start:end + 1])


def normalize(input_path, out_path):
    m = measure_loudness(input_path)
    filter_chain = (
        f"{MONO_DOWNMIX},"
        f"loudnorm=I={TARGET_LUFS}:TP={TARGET_TP}:LRA={TARGET_LRA}:"
        f"measured_I={m['input_i']}:measured_TP={m['input_tp']}:"
        f"measured_LRA={m['input_lra']}:measured_thresh={m['input_thresh']}:"
        f"offset={m['target_offset']}:linear=true,"
        f"aresample={SAMPLE_RATE}"
    )
    run([
        "ffmpeg", "-y", "-i", str(input_path), "-af", filter_chain,
        "-ar", str(SAMPLE_RATE), "-ac", "1", "-b:a", BITRATE, "-codec:a", "libmp3lame",
        "-map_metadata", "-1", "-vn",
        str(out_path),
    ])
    after = measure_loudness(out_path, downmix=False)  # already mono - don't downmix again
    print(f"  {input_path.name}: {float(m['input_i']):.1f} LUFS / {float(m['input_tp']):.1f} dBTP -> "
          f"{float(after['input_i']):.1f} LUFS / {float(after['input_tp']):.1f} dBTP -> "
          f"{out_path.relative_to(OUT)}")


def main():
    OUT.mkdir(exist_ok=True)
    sources = sorted(SRC.glob("*.mp3"))
    if not sources:
        raise SystemExit(f"no .mp3 files in {SRC}")

    print("Normalizing radio spots to a common loudness level...")
    for src in sources:
        match = re.search(r"Spot (\d+)", src.name)
        if not match:
            raise SystemExit(f"can't find a spot number in {src.name}")
        device_dir = OUT / f"device-{match.group(1)}"
        device_dir.mkdir(exist_ok=True)
        normalize(src, device_dir / "00000.mp3")

    print(f"\nDone. Each device's module contents are in {OUT}/device-<n>/")


if __name__ == "__main__":
    main()

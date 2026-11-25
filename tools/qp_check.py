#!/usr/bin/env python3
"""How hard was a recording compressed? Reads the quantizer (QP) out of an H.264 or HEVC file.

    python qp_check.py "path/to/02_toys.mov" [--points 40] [--window 2]

QP is how hard a block was compressed: under about 23 is clean, from about 30 the picture is
visibly soft, from 35 it breaks into blocks. The script samples --points stretches of --window
seconds spread over the file and reads each frame's slice QP from the bitstream with ffmpeg's
trace_headers filter (ffmpeg and ffprobe on PATH; nothing is decoded). Slice QP is exact for a
constant-quality recording; under CBR the encoder may still nudge single blocks around it.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys

FIELD = re.compile(r"\]\s+\d+\s+(\S+)\s+[01]+ = (-?\d+)\s*$")


def duration_of(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
        capture_output=True, text=True, check=True).stdout.strip()
    return float(out)


def codec_of(path):
    return subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name",
         "-of", "csv=p=0", path], capture_output=True, text=True, check=True).stdout.strip()


def frame_qps(path, start, length, codec):
    """Mean slice QP of every frame in [start, start + length)."""
    log = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostdin", "-ss", f"{start:.3f}", "-t", f"{length:.3f}", "-i", path,
         "-map", "0:v:0", "-c:v", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"],
        capture_output=True, text=True, errors="replace").stderr
    hevc = codec == "hevc"
    init_key = "init_qp_minus26" if hevc else "pic_init_qp_minus26"
    pps_key = "pps_pic_parameter_set_id" if hevc else "pic_parameter_set_id"
    slice_pps_key = "slice_pic_parameter_set_id" if hevc else "pic_parameter_set_id"
    init = {}
    frames, current = [], []
    section, pps_id = None, 0
    for line in log.splitlines():
        if "Picture Parameter Set" in line:
            section, pps_id = "pps", 0
            continue
        if "Slice Header" in line or "Slice Segment Header" in line:
            section, pps_id = "slice", 0
            continue
        m = FIELD.search(line)
        if not m:
            continue
        name, value = m.group(1), int(m.group(2))
        if section == "pps":
            if name == pps_key:
                pps_id = value
            elif name == init_key:
                init[pps_id] = value
        elif section == "slice":
            if name in ("first_mb_in_slice", "first_slice_segment_in_pic_flag"):
                new_frame = value == 0 if name == "first_mb_in_slice" else value == 1
                if new_frame and current:
                    frames.append(statistics.fmean(current))
                    current = []
            elif name == slice_pps_key:
                pps_id = value
            elif name == "slice_qp_delta":
                current.append(26 + init.get(pps_id, 0) + value)
    if current:
        frames.append(statistics.fmean(current))
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file")
    parser.add_argument("--points", type=int, default=40)
    parser.add_argument("--window", type=float, default=2.0)
    args = parser.parse_args()

    codec = codec_of(args.file)
    if codec not in ("h264", "hevc"):
        sys.exit(f"{args.file}: {codec} is not H.264 or HEVC; this script only reads those.")
    total = duration_of(args.file)
    size = os.path.getsize(args.file)
    points = max(1, min(args.points, int(total // args.window) or 1))
    windows, everything = [], []
    for i in range(points):
        start = max(0.0, total * (i + 0.5) / points - args.window / 2)
        qps = frame_qps(args.file, start, args.window, codec)
        if qps:
            windows.append((start, statistics.fmean(qps)))
            everything.extend(qps)
    if not everything:
        sys.exit(f"{args.file}: no slice QP found (is ffmpeg's trace_headers filter available?)")

    worst_at, worst = max(windows, key=lambda w: w[1])
    print(f"{os.path.basename(args.file)}  {codec}  {total / 60:.1f} min  "
          f"{size / 1e9:.2f} GB  {size * 8 / total / 1e6:.1f} Mbps average")
    print(f"  median QP        {statistics.median(everything):.1f}")
    print(f"  worst stretch    {worst:.1f}  (at {int(worst_at // 60)}:{int(worst_at % 60):02d})")
    print(f"  time at QP 30+   {100 * sum(q >= 30 for q in everything) / len(everything):.0f}%")
    print(f"  time at QP 35+   {100 * sum(q >= 35 for q in everything) / len(everything):.0f}%")
    print(f"  ({len(everything)} frames in {len(windows)} stretches of {args.window:g} s)")


if __name__ == "__main__":
    main()

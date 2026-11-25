# /// script
# requires-python = ">=3.10"
# dependencies = ["av>=14", "numpy"]
# ///
"""How hard was a recording compressed? Reads the quantizer (QP) out of an H.264 or HEVC file.

    uv run qp_check.py "path/to/02_toys.mov" [--points 40] [--window 2]
    (or: pip install av numpy, then python qp_check.py ...)

QP is how hard a block was compressed: under about 23 is clean, from about 30 the picture is
visibly soft, from 35 it breaks into blocks. The script decodes --points stretches of --window
seconds spread over the file and averages the QP of every block in each frame. Every block
counts, because NVENC writes one fixed QP in each frame's header and sets the real one per
block.
"""

import argparse
import os
import statistics
import sys

import av


def frame_qps(container, stream, start, length):
    """Mean block QP of every frame in [start, start + length)."""
    tb = stream.time_base
    container.seek(int(start / tb), stream=stream, backward=True)
    out = []
    for frame in container.decode(stream):
        if frame.pts is None:
            continue
        t = float(frame.pts * tb)
        if t < start:
            continue
        if t >= start + length:
            break
        params = frame.side_data.get(av.sidedata.sidedata.Type.VIDEO_ENC_PARAMS)
        if params is not None and params.nb_blocks:
            out.append(float(params.qp_map().mean()))
        elif params is not None:
            out.append(float(params.qp))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file")
    parser.add_argument("--points", type=int, default=40)
    parser.add_argument("--window", type=float, default=2.0)
    args = parser.parse_args()

    container = av.open(args.file)
    stream = container.streams.video[0]
    codec = stream.codec_context.name
    if codec not in ("h264", "hevc"):
        sys.exit(f"{args.file}: {codec} is not H.264 or HEVC; this script only reads those.")
    stream.codec_context.options = {"export_side_data": "venc_params"}
    stream.thread_type = "AUTO"
    if stream.duration is not None:
        total = float(stream.duration * stream.time_base)
    else:
        total = container.duration / av.time_base
    size = os.path.getsize(args.file)
    points = max(1, min(args.points, int(total // args.window) or 1))
    windows, everything = [], []
    for i in range(points):
        start = max(0.0, total * (i + 0.5) / points - args.window / 2)
        qps = frame_qps(container, stream, start, args.window)
        if qps:
            windows.append((start, statistics.fmean(qps)))
            everything.extend(qps)
    if not everything:
        sys.exit(f"{args.file}: the decoder gave no QP (this FFmpeg build may not export it)")

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

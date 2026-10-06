#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""裁屏、旋转、灰度互相关对时，再输出 H.264 并排录像（仅标准库与 ffmpeg）。"""
import argparse
import math
from pathlib import Path
import subprocess


def rotation(degrees):
    return {0: [], 90: ['transpose=1'], 180: ['hflip', 'vflip'],
            270: ['transpose=2']}[degrees]


def raw_input(path, fps):
    return ['-f', 'rawvideo', '-pixel_format', 'monob', '-video_size', '800x600',
            '-framerate', str(fps), '-i', str(path)]


def samples(input_args, filters, rate):
    cmd = ['ffmpeg', '-v', 'error', *input_args, '-an', '-vf',
           ','.join(filters + [f'fps={rate}', 'scale=24:18:flags=area', 'format=gray']),
           '-pix_fmt', 'gray', '-f', 'rawvideo', '-']
    data = subprocess.check_output(cmd)
    frames = []
    for start in range(0, len(data) - 431, 432):
        frame = data[start:start + 432]
        mean = sum(frame) / 432
        values = [v - mean for v in frame]
        norm = math.sqrt(sum(v * v for v in values))
        frames.append([v / norm for v in values] if norm else None)
    return frames


def correlate(camera, reference, limit):
    # camera[i + lag] corresponds to reference[i]. Normalize each frame to
    # reduce the effect of exposure and the panel's lower contrast.
    minimum = max(2, min(len(camera), len(reference)) // 2)
    best = None
    for lag in range(-limit, limit + 1):
        start, end = max(0, -lag), min(len(reference), len(camera) - lag)
        if end - start < minimum:
            continue
        total, count = 0.0, 0
        for i in range(start, end):
            a, b = camera[i + lag], reference[i]
            if a is None or b is None:
                continue
            total += sum(x * y for x, y in zip(a, b))
            count += 1
        if count < 2:
            continue
        candidate = (total / count, count, -abs(lag), lag)
        if best is None or candidate[:3] > best[:3]:
            best = candidate
    if best is None:
        raise ValueError('录像与原片没有足够的非恒定帧用于互相关')
    return best[3], best[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--video', type=Path, required=True, help='相机录像')
    parser.add_argument('--raw', type=Path, required=True, help='800x600 monob 原片')
    parser.add_argument('--crop', required=True, help='屏幕裁剪框 X:Y:WIDTH:HEIGHT')
    parser.add_argument('--rotation', type=int, choices=(0, 90, 180, 270), default=0,
                        help='录像顺时针旋转角度')
    parser.add_argument('--transpose', choices=('none', '0', '1', '2', '3'), default='none',
                        help='原片 ffmpeg transpose 编号；2 为逆时针 90 度')
    parser.add_argument('--output', type=Path, required=True, help='并排 H.264 mp4')
    parser.add_argument('--fps', type=float, default=30, help='原片帧率')
    parser.add_argument('--sample-fps', type=float, default=5, help='互相关采样率，决定偏移精度')
    parser.add_argument('--max-offset', type=float, default=15, help='正负偏移搜索范围（秒）')
    args = parser.parse_args()
    try:
        x, y, w, h = map(int, args.crop.split(':'))
    except ValueError:
        parser.error('--crop 格式为 X:Y:WIDTH:HEIGHT')
    if min(x, y) < 0 or min(w, h, args.fps, args.sample_fps) <= 0 or args.max_offset < 0:
        parser.error('裁剪尺寸与帧率必须为正，坐标与搜索范围不能为负')
    if args.raw.stat().st_size % 60000:
        parser.error('raw 文件长度不是 60000 字节的整数倍')
    camera_filters = [f'crop={w}:{h}:{x}:{y}', *rotation(args.rotation)]
    reference_filters = [] if args.transpose == 'none' else [f'transpose={args.transpose}']
    camera = samples(['-i', str(args.video)], camera_filters, args.sample_fps)
    reference = samples(raw_input(args.raw, args.fps), reference_filters, args.sample_fps)
    lag, score = correlate(camera, reference, round(args.max_offset * args.sample_fps))
    offset = lag / args.sample_fps
    print(f'offset_seconds={offset:.6f} correlation={score:.6f} '
          f'(camera[t + offset] matches raw[t], resolution={1 / args.sample_fps:.6f}s)', flush=True)
    width, height = (800, 600) if args.transpose == 'none' else (600, 800)
    left = camera_filters + [f'trim=start={max(0, offset)}', 'setpts=PTS-STARTPTS',
                             f'scale={width}:{height}', 'setsar=1', f'fps={args.fps}']
    right = reference_filters + [f'trim=start={max(0, -offset)}', 'setpts=PTS-STARTPTS',
                                'setsar=1']
    graph = ('[0:v]' + ','.join(left) + '[cam];[1:v]' + ','.join(right) +
             '[ref];[cam][ref]hstack=inputs=2:shortest=1[out]')
    subprocess.run(['ffmpeg', '-y', '-v', 'error', '-i', str(args.video),
                    *raw_input(args.raw, args.fps), '-filter_complex', graph,
                    '-map', '[out]', '-an', '-c:v', 'libx264', '-pix_fmt', 'yuv420p',
                    '-movflags', '+faststart', str(args.output)], check=True)


if __name__ == '__main__':
    main()

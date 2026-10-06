#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
if (( $# != 4 )); then
    echo 'Usage: record.sh CAMERA_DEVICE WIDTHxHEIGHT SECONDS OUTPUT.mp4' >&2
    exit 2
fi
ffmpeg -y -f v4l2 -video_size "$2" -i "$1" -t "$3" -an -c:v libx264 -pix_fmt yuv420p "$4"

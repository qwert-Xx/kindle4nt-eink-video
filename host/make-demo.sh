#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Usage: make-demo.sh OUTPUT [all|anim|life|mandelbrot]
set -euo pipefail
output=${1:?Usage: make-demo.sh OUTPUT [all|anim|life|mandelbrot]}
part=${2:-all}
anim="color=white:s=800x600:r=30:d=30,format=gray,geq=lum='if(lt(T,10), if(lt(hypot(X-400-280*sin(T*1.3),Y-300-180*cos(T*0.9)),110)+between(X,mod(T*200,800),mod(T*200,800)+60),0,255), if(lt(T,20), if(gt((X-400)*cos(T*0.8)+(Y-300)*sin(T*0.8), 120*sin(T*1.7)) - lt(hypot(X-400,Y-300),90+60*sin(T*3)) ,0,255), if(lt(hypot(X-400-300*sin(T*2.1),Y-300-200*sin(T*1.7)),60)+lt(hypot(X-400-300*cos(T*1.9),Y-300-200*sin(T*2.5)),45)+lt(hypot(X-400-250*sin(T*3.1),Y-300-150*cos(T*2.3)),35),0,255)))'"
life='life=s=200x150:mold=0:r=30:ratio=0.12:life_color=white:death_color=black,scale=800:600:flags=neighbor,format=gray,negate'
mandelbrot="mandelbrot=s=800x600:rate=30:end_scale=0.002,format=gray,lut=y='if(gt(val\,60)\,0\,255)'"
render() {
    ffmpeg -v error -f lavfi -i "$1" -frames:v "$2" -pix_fmt monob -f rawvideo -
}
case "$part" in
    all) { render "$anim" 900; render "$life" 300; render "$mandelbrot" 300; } > "$output" ;;
    anim) render "$anim" 900 > "$output" ;;
    life) render "$life" 300 > "$output" ;;
    mandelbrot) render "$mandelbrot" 300 > "$output" ;;
    *) echo 'Unknown segment; use all, anim, life or mandelbrot' >&2; exit 2 ;;
esac
python3 - "$output" <<'PY'
import os, sys
size = os.path.getsize(sys.argv[1])
print(f'{sys.argv[1]}: {size // 60000} frames, {size} bytes (60000 bytes/frame)')
PY

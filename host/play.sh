#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
usage() {
    echo 'Usage: play.sh --target USER@HOST --player LOCAL_BINARY --remote-player PATH --raw FILE [--port 5000] [--stream-host HOST] [--ssh-option ARG]... [-- PLAYER_ARGS...]'
}
ssh_args=(); player_args=(); port=5000; stream_host=''
target=''; player=''; remote=''; raw=''
while (( $# )); do
    case "$1" in
        --target) target=$2; shift 2 ;;
        --player) player=$2; shift 2 ;;
        --remote-player) remote=$2; shift 2 ;;
        --raw) raw=$2; shift 2 ;;
        --port) port=$2; shift 2 ;;
        --stream-host) stream_host=$2; shift 2 ;;
        --ssh-option) ssh_args+=("$2"); shift 2 ;;
        --) shift; player_args=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
if [[ -z $target || -z $player || -z $remote || -z $raw ]]; then usage >&2; exit 2; fi
if [[ -z $stream_host ]]; then
    stream_host=${target##*@}
    stream_host=${stream_host#[}; stream_host=${stream_host%]}
fi
# Quote each argument for the remote POSIX shell, including paths with spaces.
quote() { local s=${1//\'/\'\\\'\'}; printf "'%s'" "$s"; }
rprog=$(quote "$remote"); rlog=$(quote "$remote.log"); rpid=$(quote "$remote.pid")
rdir=$(quote "$(dirname "$remote")")
ssh "${ssh_args[@]}" "$target" "mkdir -p $rdir && cat > $rprog && chmod +x $rprog && : > $rlog" < "$player"
command="$rprog $(quote "$port")"
for arg in "${player_args[@]}"; do command+=" $(quote "$arg")"; done
ssh "${ssh_args[@]}" "$target" "echo \$\$ > $rpid; exec $command > $rlog 2>&1" &
runner=$!
cleanup() {
    status=$?
    trap - EXIT
    if kill -0 "$runner" 2>/dev/null; then
        ssh "${ssh_args[@]}" "$target" "kill -TERM \$(cat $rpid)" || true
    fi
    wait "$runner" || { if (( status == 0 )); then status=1; fi; }
    ssh "${ssh_args[@]}" "$target" "cat $rlog; rm -f $rpid" || true
    exit "$status"
}
trap cleanup EXIT
ready=0
for (( i=0; i<100; i++ )); do
    if ssh "${ssh_args[@]}" "$target" "grep -q listening $rlog"; then ready=1; break; fi
    if ! kill -0 "$runner" 2>/dev/null; then break; fi
    sleep 0.5
done
if (( ! ready )); then echo 'Player did not start listening' >&2; exit 1; fi
ffmpeg -v error -re -f rawvideo -pixel_format monob -video_size 800x600 -framerate 30 -i "$raw" -c:v copy -f rawvideo - |
    nc -N "$stream_host" "$port"
wait "$runner"

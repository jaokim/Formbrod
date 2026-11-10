#!/bin/sh
# Makes test inputs with the ffmpeg CLI, runs the libav backend test on
# them, and cross-checks the output durations with ffprobe.
#   tests/test_libav.sh <test binary> <work dir>
set -e
bin=$1
dir=$2
mkdir -p "$dir"

gen() {
    ffmpeg -v error -y "$@"
}
gen -f lavfi -i testsrc=size=320x240:rate=25:duration=5 \
    -f lavfi -i sine=frequency=440:sample_rate=48000:duration=5 \
    -ac 2 -c:v mpeg4 -c:a aac -shortest "$dir/a.mp4"
gen -f lavfi -i testsrc2=size=640x360:rate=30:duration=4 \
    -f lavfi -i sine=frequency=880:sample_rate=44100:duration=4 \
    -c:v mpeg4 -c:a mp2 -shortest "$dir/b.mkv"
gen -f lavfi -i testsrc=size=320x240:rate=25:duration=3 \
    -c:v mpeg4 "$dir/c.mp4"

"$bin" "$dir"

check() {
    d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$dir/$1")
    echo "ffprobe: $1 is $d s (expected $2)"
    awk -v d="$d" -v e="$2" 'BEGIN { exit !(d > e - 0.15 && d < e + 0.15) }' \
        || { echo "FAIL: $1 duration"; exit 1; }
}
check out_join.mp4 4.0
check out_reversed.mp4 2.5
check out_video_only.mkv 3.0
check out_h264.mkv 9.0
check out_mixed.mp4 5.5
# decode every frame to make sure the files are clean
for f in out_join.mp4 out_reversed.mp4 out_video_only.mkv out_h264.mkv \
         out_mixed.mp4; do
    ffmpeg -v error -xerror -i "$dir/$f" -f null - || { echo "FAIL: $f decode"; exit 1; }
done
echo "ffprobe checks passed"

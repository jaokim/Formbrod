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
# Music on its own: sound from another file than the pictures.
gen -f lavfi -i sine=frequency=330:sample_rate=44100:duration=6 \
    -ac 2 -c:a pcm_s16le "$dir/d.wav"
# A still picture, 8-bit with a palette, an odd size.
gen -f lavfi -i testsrc=size=621x610 -frames:v 1 -pix_fmt pal8 "$dir/e.bmp"
# Longer: pictures for 30 s, and music as mp3 (like a song on its own track).
gen -f lavfi -i testsrc=size=320x240:rate=25:duration=30 \
    -c:v mpeg4 "$dir/f.mp4"
gen -f lavfi -i sine=frequency=440:sample_rate=44100:duration=32 \
    -ac 2 -c:a mp2 "$dir/g.mp2"

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
check out_xfade.mp4 3.0
# decode every frame to make sure the files are clean
for f in out_join.mp4 out_reversed.mp4 out_video_only.mkv out_h264.mkv \
         out_mixed.mp4 out_xfade.mp4; do
    ffmpeg -v error -xerror -i "$dir/$f" -f null - || { echo "FAIL: $f decode"; exit 1; }
done
echo "ffprobe checks passed"

# The pictures themselves, not just the lengths: an ffmpeg build that
# writes garbage still makes files of the right length.
psnr() {
    ffmpeg -i "$dir/$1" -i "$dir/$2" -lavfi "$3" -f null - 2>&1 |
        sed -n 's/.*PSNR.*average:\([0-9.]*\).*/\1/p' | tail -1
}
need() {
    echo "picture: $1 is $2 dB like its source (want $3 or more)"
    awk -v p="$2" -v w="$3" 'BEGIN { exit !(p + 0 >= w) }' ||
        { echo "FAIL: $1 picture"; exit 1; }
}
# The proxy copy of a.mp4: 160 wide, scaled the same way, full range.
proxy=$(ls "$dir"/*.proxy1-160.mov | head -1)
p=$(psnr a.mp4 "$(basename "$proxy")" \
    "[0:v]scale=160:120:flags=fast_bilinear,format=yuv420p[a];[1:v]format=yuv420p[b];[a][b]psnr")
need "the proxy copy" "$p" 35
# The window from 3.5 s: its first 1.5 s are a.mp4 from 3.5 s.
p=$(psnr a.mp4 out_window.mp4 \
    "[0:v]trim=start=3.5:duration=1.4,setpts=PTS-STARTPTS,format=yuv420p[a];[1:v]trim=duration=1.4,setpts=PTS-STARTPTS,format=yuv420p[b];[a][b]psnr")
need "the window" "$p" 25
# A rendered piece (10 - 20 s of f.mp4) against f.mp4 itself.
p=$(psnr f.mp4 piece1.render1.mov \
    "[0:v]trim=start=10:duration=9.9,setpts=PTS-STARTPTS,scale=320:240:flags=fast_bilinear,format=yuv420p[a];[1:v]trim=duration=9.9,setpts=PTS-STARTPTS,format=yuv420p[b];[a][b]psnr")
need "the rendered piece" "$p" 30
echo "picture checks passed"

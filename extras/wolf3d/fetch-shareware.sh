#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Fetch the Wolfenstein 3D shareware, version 1.4 (episode 1), and put its
# data files in DIR (default: data), named the way xwolf looks for them.
# Run it on a computer with curl, unzip and 7-Zip (7z or 7zz), not on the
# board: the data is inside an ARJ archive, which only 7-Zip unpacks.
#
#     ./fetch-shareware.sh [DIR]
#
# The shareware is id Software's, free to copy as it comes; it is not part
# of this repository.
set -e

dir=${1:-data}
url=https://www.gamers.org/pub/games/wolf3d/official/1wolf14.zip
md5=e6d75d885e408003d2f4751d8dfd1cb0

if command -v 7zz > /dev/null; then
    sevenzip=7zz
elif command -v 7z > /dev/null; then
    sevenzip=7z
else
    echo "need 7-Zip (7z or 7zz) to unpack the data" >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL -o "$work/1wolf14.zip" "$url"
sum=$( (md5sum "$work/1wolf14.zip" 2>/dev/null || md5 -r "$work/1wolf14.zip") | cut -d' ' -f1)
if [ "$sum" != "$md5" ]; then
    echo "1wolf14.zip: MD5 $sum, expected $md5" >&2
    exit 1
fi

# The zip holds a self-extracting ARJ archive (a DOS program with the
# archive after it) in three parts; 7-Zip finds the archive when not told
# what kind of file it is, so the joined file has no .arj name.
(cd "$work" && unzip -q 1wolf14.zip && cat WOLF3D1.1 WOLF3D1.2 WOLF3D1.3 > wolf3d1.bin)
mkdir -p "$work/x"
"$sevenzip" x -y -o"$work/x" "$work/wolf3d1.bin" > /dev/null

mkdir -p "$dir"
for f in "$work"/x/*.WL1; do
    cp "$f" "$dir/$(basename "$f" | tr 'A-Z' 'a-z')"
done
cp "$work/x/READ1ST.TXT" "$dir/" 2>/dev/null || true
ls "$dir"

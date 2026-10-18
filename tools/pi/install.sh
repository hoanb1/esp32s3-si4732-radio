#!/bin/bash
# Install ft8station on the Pi (run as root from a checkout copied to the Pi):
#   sudo tools/pi/install.sh /path/to/ft8_lib /home/pi/Ft8Pi
# Builds the ft8_lib encoder and a patched copy of Ft8Pi (raw tones, DT=0 start) into
# /usr/local/lib/ft8station; the original Ft8Pi install is left untouched.
set -e
LIB=${1:?path to ft8_lib (the folder with ft8/ and common/)}
FT8PI=${2:?path to the Ft8Pi source}
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=/usr/local/lib/ft8station
BUILD=$(mktemp -d)
mkdir -p "$DEST"
gcc -O2 -I"$LIB" -o "$DEST/enc" "$HERE/enc.c" "$LIB"/ft8/message.c "$LIB"/ft8/encode.c \
    "$LIB"/ft8/text.c "$LIB"/ft8/constants.c "$LIB"/ft8/crc.c
cp -r "$FT8PI"/. "$BUILD"
(cd "$BUILD" && rm -f ft8 *.o && patch -p1 < "$HERE/ft8pi-tones.patch" && make ft8)
install -m 0755 "$BUILD/ft8" "$DEST/ft8"
install -m 0755 "$HERE/ft8station.py" /usr/local/bin/ft8station
rm -rf "$BUILD"
echo "Installed. Next: ft8station config --call <CALL> --grid <GRID>"

#!/bin/sh
# Build xextool against the xenia-edge tree checked out next to this dump.
set -e
XENIA="${1:-../../xenia-edge}"
gcc -O2 \
  -I "$XENIA/third_party/crypto" -I "$XENIA/third_party/mspack" -I . \
  xextool.c \
  "$XENIA/third_party/crypto/rijndael-alg-fst.c" \
  "$XENIA/third_party/mspack/lzxd.c" \
  stub.c \
  -o xextool
echo "built ./xextool"

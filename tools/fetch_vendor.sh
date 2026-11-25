#!/bin/sh
# fpj2me — tools/fetch_vendor.sh : (re)fetch the vendored single-file libs.
# vendor/ already ships with the tree; run this only to refresh versions.
set -e
cd "$(dirname "$0")/../vendor"

# miniz 3.0.2 amalgamation (MIT) — we build tinfl-only
curl -sfL -o miniz.c   https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz.c
curl -sfL -o miniz.h   https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz.h
curl -sfL -o miniz.LICENSE https://raw.githubusercontent.com/richgel999/miniz/3.0.2/LICENSE

# stb_image 2.34 (MIT / public domain) — PNG decoder only
curl -sfL -o stb_image.h https://raw.githubusercontent.com/nothings/stb/2.34/stb_image.h

echo "vendor refreshed. (font8x8.h is generated locally — see git history)"

#!/bin/sh
# fpj2me host check: compile the WHOLE runtime natively (VM + bridge +
# GC + natives) against stubbed fp_* primitives and run the smoke tests.
set -e
cd "$(dirname "$0")/.."

mkdir -p /tmp/fpj2me/rms/tests
python3 tools/make_test_jar.py
python3 tools/make_deep_jar.py

# the VM boots "game.jar" from FPJ_SD_ROOT (relative paths like on
# target; the MIDlet-1 attributes come from the jar's own manifest)
cp tests/test.jar /tmp/fpj2me/game.jar

# the sibling game.jad (MIDP 2.0): overrides manifest keys for the
# getAppProperty chain tests. Mixes CRLF/LF endings and folds one
# value across an RFC-822 continuation line (single leading space).
printf 'MIDlet-1: Tst, , Tst\r\nMIDlet-Name: JAD-Name\r\n' >  /tmp/fpj2me/game.jad
printf 'X-Long-Value: start\r\n continued-end\nX-Empty:\n'   >> /tmp/fpj2me/game.jad

gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
    -funsigned-char \
    -DFP_LCD_W=128 -DFP_LCD_H=160 -DFPJ_SD_ROOT='"/tmp/fpj2me/"' \
    -Isrc -Ivendor \
    -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS \
    src/jar.c src/heap.c src/interp.c src/natives.c \
    src/midp_image.c src/midp_graphics.c \
    src/midp_input.c src/midp_rms.c src/midp_media.c src/fp_font.c \
    vendor/miniz.c vendor/stb_impl.c \
    tests/hostcheck.c -o tests/hostcheck

cd tests && ./hostcheck

# ---- boot stack checks ----------------------------------------------
# The phone's app stack is 36 KB (sc6531e_fdl.ld EXTRA_SIZE 0x9000).
# Boot the smoke jar AND the 24-deep super-chain jar inside a thread
# with exactly that much stack: the iterative class_load + static
# tinfl state must keep the stack flat (the old recursive version
# died at ~4 levels — see tools/make_deep_jar.py).
cd ..
gcc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
    -funsigned-char -pthread \
    -DFP_LCD_W=128 -DFP_LCD_H=160 -DFPJ_SD_ROOT='""' \
    -Isrc -Ivendor \
    -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS \
    src/jar.c src/heap.c src/interp.c src/natives.c \
    src/midp_image.c src/midp_graphics.c \
    src/midp_input.c src/midp_rms.c src/midp_media.c src/fp_font.c \
    vendor/miniz.c vendor/stb_impl.c \
    tests/boot_jar.c -o tests/boot_jar

./tests/boot_jar tests/test.jar 36 >/dev/null 2>&1 \
    && echo "PASS  boot smoke jar in 36KB-stack thread" \
    || { echo "FAIL  boot smoke jar in 36KB-stack thread"; exit 1; }
./tests/boot_jar tests/deep.jar 36 >/dev/null 2>&1 \
    && echo "PASS  boot 24-deep chain in 36KB-stack thread" \
    || { echo "FAIL  boot 24-deep chain in 36KB-stack thread"; exit 1; }

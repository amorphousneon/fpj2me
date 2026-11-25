# fpj2me - an fpdoom app directory.
#
# INTEGRATION (one-time):
#   1. clone fpdoom:            git clone https://github.com/ilyakurdyukov/fpdoom
#   2. copy this fpj2me dir in: cp -r fpj2me /path/to/fpdoom/
#   3. build the target app:
#        cd /path/to/fpdoom/fpj2me
#        make LIBC_SDIO=3 CHIP=1 TOOLCHAIN=arm-buildroot-linux-gnueabi
#                                       # CHIP=1 -> SC6531E (itel it2163)
#      -> fpj2me.bin
#
#      TOOLCHAIN= is the arm-* prefix of your cross compiler (needs gcc,
#      objcopy, nm; -nostdlib so any armv5 gcc works). Put its bin/ dir on
#      PATH or set TOOLCHAIN=/full/path/prefix.
#
#      LIBC_SDIO=3 is MANDATORY for sdboot apps (SD-card FAT32 file IO).
#      With LIBC_SDIO=0 the app expects libc_server over USB (debugging).
#
#   4. copy to the SD card next to your other games and add a menu line
#      to fpbin/config.txt (see README.txt "SD card layout").
#
# Host-side checks (no hardware, no ARM toolchain):  make check

NAME = fpj2me

APP_SRCS = src/target_main src/fp_platform src/fp_menu src/fp_font \
           src/jar src/heap src/interp \
           src/natives src/midp_image src/midp_graphics src/midp_input \
           src/midp_rms src/midp_media \
           vendor/miniz vendor/stb_impl
APP_OBJS1 = $(APP_SRCS:%=$(OBJDIR)/app/%.o)
APP_OBJS2 =

include ../buildinc.make

APP_CFLAGS += -std=c99 -I$(SYSDIR) -Isrc -Ivendor
APP_CFLAGS += -DFP_LCD_W=128 -DFP_LCD_H=160

# Some cross toolchains (Buildroot!) default to -fstack-protector-strong,
# which emits calls to __stack_chk_guard/__stack_chk_fail — not available
# with -nostdlib. Disable it for every TU (app AND sys) so the link works
# on ANY toolchain, with or without SSP defaults.
EXTRA_CFLAGS += -fno-stack-protector

# CLDC 1.1 float/double opcodes need the ARM soft-float helpers from
# libgcc (__aeabi_dadd, __aeabi_fmul, ...). fpdoom links -nostdlib and
# the stock rule puts nothing after the objects, where an archive member
# would never be pulled in. Override BOTH part link rules so libgcc comes
# AFTER the objects (wrapped in a group; harmless with any toolchain).
$(OBJDIR)/$(NAME)_part1.elf: $(OBJS1)
	$(CC) $(LFLAGS) $(OBJS1) -Wl,--start-group -lgcc -Wl,--end-group -o $@

$(OBJDIR)/$(NAME)_part2.elf: $(OBJS2)
	$(CC) $(LFLAGS) $(OBJS2) -Wl,--start-group -lgcc -Wl,--end-group -o $@

# tinfl-only miniz: no stdio, no time, no zip archive APIs (jar.c walks
# the central directory itself)
$(OBJDIR)/app/vendor/miniz.o: APP_CFLAGS += -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS

.PHONY: check
check:
	./tests/run.sh

#!/bin/sh
# JagSkyroads - the C blob.  Runs inside the marsdev-32x Docker image
# (m68k-elf-gcc 15.2.0, the chain JagLBA and Keen Dreams are built with):
#
#   docker run --rm -v "%CD%:/work" -w /work/csrc marsdev-32x:latest "sh build_engine.sh"
#
# The command goes as ONE string: the image's entrypoint ignores anything
# else and exits 0 printing nothing.
#
# Output: build/engine.bin (packed at ROM_CBLOB, copied to DBLOB at boot),
# plus .elf, .sym, .lst.
set -e

BIN=/marsdev/mars/m68k-elf/bin
LIBGCC=$("$BIN/m68k-elf-gcc" -m68000 -print-libgcc-file-name)
OUT=../build

# No -mshort: int stays 32 bits, as on the PC the oracle is compiled on - the
# port's arithmetic assumes it.
CFLAGS="${EXTRA_CFLAGS} -m68000 -O2 -std=c11 -fomit-frame-pointer -ffreestanding -fno-builtin \
        -fno-tree-loop-distribute-patterns --param=min-pagesize=0 -Wall -Wextra -I."

SRC="port/play.c road.c libc.c game_main.c"
OBJS="crt0.o"

"$BIN/m68k-elf-as" -m68000 crt0.s -o crt0.o
for f in $SRC; do
    o=$(basename "$f" .c).o
    "$BIN/m68k-elf-gcc" $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

mkdir -p "$OUT"
"$BIN/m68k-elf-ld" --no-warn-rwx-segments -T ../include/jag_c.ld $OBJS "$LIBGCC" -o "$OUT/engine.elf"
"$BIN/m68k-elf-objcopy" -O binary -j .text -j .data "$OUT/engine.elf" "$OUT/engine.bin"
"$BIN/m68k-elf-nm" -n "$OUT/engine.elf" > "$OUT/engine.sym"
"$BIN/m68k-elf-objdump" -d "$OUT/engine.elf" > "$OUT/engine.lst"
rm -f *.o

# The entry table MUST be the blob's first bytes: RMAC jumps to fixed offsets.
FIRST=$("$BIN/m68k-elf-nm" -n "$OUT/engine.elf" | grep -m1 ' [Tt] ' | awk '{print $3}')
if [ "$FIRST" != "entry_table" ]; then
    echo "ERROR: entry_table is not the blob's first symbol ($FIRST)"
    exit 1
fi
"$BIN/m68k-elf-size" "$OUT/engine.elf"

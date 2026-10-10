#!/bin/sh
#
# Populates the card directory the "--sdhost" backend serves, for the fattest
# ROM. Run from the repository root:
#
#   sh test/mkcard.sh
#   ./cuzebox.exe --sdhost test/roms/fattest.hex
#
# The content is generated rather than committed: hostrd.dat alone is 100000
# bytes, and all of it is mechanical.

set -e

DIR=test/roms/uzedisk

# Start from an empty card. What a previous run left behind (renamed files, the
# trash) makes the suite's renames fail with FR_EXIST.
rm -rf "$DIR"
mkdir -p "$DIR"

# Something ordinary to read, and a zero length file, which gets no cluster at
# all on a real FAT volume.
printf 'hello from the host card\n' > "$DIR/hello.txt"
: > "$DIR/empty.dat"

# A file the ROM deletes. Everything else the suite deletes it created itself,
# which cannot show a deferral at reset or exit: a destructive operation is only
# deferrable if it applies to something that was on the card to begin with.
printf 'delete me\n' > "$DIR/doomed.txt"

# Two host names converging on one 8.3 name, so the "~1" dedup of section 7.4
# has something to do. Without it the FAT namespace stops being bijective with
# the host directory and a delete or rename becomes ambiguous.
printf 'report body\n' > "$DIR/MyLongReport.txt"
printf 'record body\n' > "$DIR/MyLongRecord.txt"

# What the ROM's HOST READTHROUGH step checks. Four clusters at 32 KBytes, with
# a position dependent pattern, so a wrong cluster ordinal shows up as a first
# bad offset landing exactly on a 32768 byte boundary rather than as plausible
# looking rubbish. The ROM recomputes this same expression.
perl -e 'for my $i (0..99999){ print chr((($i * 7 + ($i >> 8)) & 0xFF)); }' \
     > "$DIR/hostrd.dat"

# Byte n of a pattern file is the ROM's fillbuf() pattern at base + n, so what
# the ROM does to it can be checked on the host.
pat(){ perl -e 'my ($b, $l) = @ARGV; for my $i (0..$l-1){ my $v = $b + $i; print chr(($v ^ ($v >> 8)) & 0xFF); }' "$1" "$2"; }

# Files the card comes with that the ROM changes: appended to, rewritten in
# place without changing size, and truncated. A file already on the card is
# sized by a different rule from one the ROM creates.
printf 'appended to by the ROM:\n' > "$DIR/append.txt"
pat $((0x9999)) 1000 > "$DIR/save.dat"
pat $((0xC3C3)) 3000 > "$DIR/shrink.txt"

# "sh test/mkcard.sh tail": the files the ROM's tail works on after DONE, for
# resetting or closing the emulator while the card is busy.
if [ "$1" = "tail" ]; then
  pat $((0x7777)) 3000 > "$DIR/taila.dat"   # deleted, its cluster reused
  pat $((0x8888)) 5000 > "$DIR/tailt.dat"   # truncated to 100
  pat $((0x6666)) 1000 > "$DIR/tailw.dat"   # appended to until stopped
fi

echo "Card directory $DIR:"
ls -l "$DIR"

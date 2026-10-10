#!/usr/bin/env bash
#
# Builds the probe fixture if needed, launches cuzebox.exe with the GDB stub
# on a scratch port, runs test_stub.py against it, and tears the emulator
# down again. Run from anywhere; paths are resolved relative to this script.
#
# Copyright (C) 2026
#   Alec Bourque (Uze)
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PORT="${1:-12340}"
ROM="$REPO_ROOT/test/roms/Basic.hex"
LOG="$REPO_ROOT/test/gdb/last_run.log"

cd "$REPO_ROOT" || exit 1

if [ ! -f "$REPO_ROOT/cuzebox.exe" ]; then
    echo "cuzebox.exe not found in $REPO_ROOT -- build it first (make)" >&2
    exit 1
fi

if [ ! -f "$SCRIPT_DIR/fixtures/probe.bin" ]; then
    echo "Assembling test/gdb/fixtures/probe.bin ..."
    # Linked (not just assembled) so intra-file rjmp/rcall relocations are
    # actually resolved before the raw .text bytes are extracted -- avr-as
    # defers branch relaxation to the linker even for local same-section
    # symbols, so a bare .o's rjmp opcodes are still zero-filled placeholders.
    ( cd "$SCRIPT_DIR/fixtures" && \
      avr-gcc -mmcu=atmega644 -nostdlib -c probe.S -o probe.o && \
      avr-gcc -mmcu=atmega644 -nostdlib -Wl,--section-start=.text=0 \
          -o probe.elf probe.o && \
      avr-objcopy -O binary --only-section=.text probe.elf probe.bin ) || exit 1
fi

rm -f "$LOG"
"$REPO_ROOT/cuzebox.exe" "$ROM" "--gdb=$PORT" > "$LOG" 2>&1 &
EMU_PID=$!

# Wait for the stub's listener to come up rather than sleeping a fixed
# amount: startup time depends on machine load.
UP=0
for _ in $(seq 1 50); do
    if command -v python3 >/dev/null 2>&1; then
        if python3 -c "
import socket, sys
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect(('127.0.0.1', $PORT))
    s.close()
    sys.exit(0)
except OSError:
    sys.exit(1)
" 2>/dev/null; then
            UP=1
            break
        fi
    fi
    sleep 0.2
done

if [ "$UP" != "1" ]; then
    echo "cuzebox.exe never opened port $PORT -- see $LOG" >&2
    kill "$EMU_PID" 2>/dev/null
    exit 1
fi

python3 "$SCRIPT_DIR/test_stub.py" 127.0.0.1 "$PORT"
RESULT=$?

kill "$EMU_PID" 2>/dev/null
wait "$EMU_PID" 2>/dev/null

if [ "$RESULT" != "0" ]; then
    echo "--- emulator log ($LOG) ---"
    tail -n 40 "$LOG"
fi

exit $RESULT

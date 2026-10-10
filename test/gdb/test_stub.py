#!/usr/bin/env python3
#
# Scripted end-to-end tests for the GDB Remote Serial Protocol stub, driven
# at the raw packet level (no avr-gdb dependency) so a failure here points
# at the stub, never at a GDB client version quirk. Companion to the manual
# avr-gdb / VSCode verification described in doc/notes-gdbstub.md.
#
# Run via run_tests.sh, which launches cuzebox.exe with --gdb=1234 first.
# Can also be pointed at an already-running instance:
#   python3 test_stub.py [host] [port]
#
# Copyright (C) 2026
#   Alec Bourque (Uze)
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
import binascii
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsp_client import RSPClient, RSPError  # noqa: E402

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1234

# Scratch flash area above the end of test/roms/Basic.hex (~0xE9FC), used to
# inject a small synthetic instruction sequence so breakpoint/watchpoint
# tests do not depend on disassembling whatever ROM happens to be loaded.
PROBE_ADDR = 0xFC00
PROBE_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "fixtures", "probe.bin")

# Layout of fixtures/probe.S (byte offsets from PROBE_ADDR):
#   +0   ldi  r16, 0x42
#   +2   sts  0x0200, r16   (2-word instruction; data write watchpoint)
#   +6   lds  r17, 0x0200   (2-word instruction; data read watchpoint)
#   +10  nop                (software breakpoint target)
#   +12  rjmp probe         (loop back to the top: the emulator keeps running
#                             between RSP packets since the harness does not
#                             use --gdb-wait, so a breakpoint/watchpoint
#                             installed after $pc was redirected here must
#                             still get a chance to fire on the next pass)
PROBE_STS = PROBE_ADDR + 2
PROBE_NOP = PROBE_ADDR + 10

WATCH_ADDR = 0x800200  # GDB data-space address of the SRAM byte at 0x0200

_fail_count = 0
_pass_count = 0


def check(cond, msg):
    global _fail_count, _pass_count
    if cond:
        _pass_count += 1
    else:
        _fail_count += 1
        print("FAIL: %s" % msg)


def connect():
    """Opens a fresh connection, verified live with a throwaway exchange.

    The stub services its socket once per emulated video frame (~16ms)
    while the CPU is not halted (see cu_gdb_hold() in cu_gdb.c), so a
    reconnect that lands within a couple of milliseconds of the previous
    test's disconnect can arrive before the server has noticed the old
    socket close; gdb_accept() then correctly refuses it as a second
    client. That is a harness pacing artifact from running many scripted
    connect/disconnect cycles back to back, not something a real GDB
    session (one long-lived connection) would ever hit, so it is worked
    around here with a short retry rather than in the stub.

    The connect itself has to be inside the retry, not just the exchange:
    the listener has a backlog of 1 (listen(gdb_listen, 1) in cu_gdb.c) and
    is only serviced once per frame while the CPU runs, so a reconnect
    arriving before the previous close has been noticed is refused outright
    by the OS rather than accepted and then rejected. That surfaces as
    ConnectionRefusedError out of socket.create_connection."""
    last_err = None
    for _ in range(20):
        rsp = None
        try:
            rsp = RSPClient(HOST, PORT, timeout=5.0)
            r = rsp.xchg("qSupported", timeout=1.0)
            if r is not None:
                return rsp
            last_err = "no reply to qSupported"
        except OSError as e:
            last_err = e
        if rsp is not None:
            rsp.close()
        time.sleep(0.1)
    raise RSPError("could not establish a live connection: %s" % (last_err,))


def hexbytes(data):
    return binascii.hexlify(data).decode("ascii")


def parse_stop(pkt):
    """Parses a Txx... stop reply into (signal, {reg: hexvalue}, {key: val})."""
    assert pkt is not None and pkt[0] == "T", "not a stop reply: %r" % pkt
    sig = pkt[1:3]
    rest = pkt[3:]
    regs = {}
    extra = {}
    for field in rest.split(";"):
        if not field or ":" not in field:
            continue
        k, v = field.split(":", 1)
        if all(c in "0123456789abcdefABCDEF" for c in k):
            regs[k] = v
        else:
            extra[k] = v
    return sig, regs, extra


def le_pc_bytes(pkt_pc_hex):
    """22:xxyyzz00 -> integer byte address (little endian, 3 significant
    bytes, per gdb_regs_get)."""
    b = bytes.fromhex(pkt_pc_hex)
    return b[0] | (b[1] << 8) | (b[2] << 16)


def qrcmd(rsp, cmdtext, timeout=5.0):
    """Sends a monitor command, collecting all O-packet console output up to
    the terminating OK/E reply. Returns (console_text, final_reply)."""
    rsp.send("qRcmd," + hexbytes(cmdtext.encode("ascii")))
    out = ""
    while True:
        pkt = rsp.read_packet(timeout)
        if pkt is None:
            return out, None
        if pkt.startswith("O") and pkt != "OK":
            out += bytes.fromhex(pkt[1:]).decode("ascii", "replace")
            continue
        return out, pkt


def upload_probe(rsp):
    with open(PROBE_BIN, "rb") as f:
        data = f.read()
    r = rsp.xchg("M%x,%x:%s" % (PROBE_ADDR, len(data), hexbytes(data)))
    check(r == "OK", "probe upload should return OK, got %r" % r)


def set_pc(rsp, byte_addr):
    lo = byte_addr & 0xFF
    mid = (byte_addr >> 8) & 0xFF
    hi = (byte_addr >> 16) & 0xFF
    r = rsp.xchg("P22=%02x%02x%02x00" % (lo, mid, hi))
    check(r == "OK", "P22 (set PC) should return OK, got %r" % r)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_qsupported():
    rsp = connect()
    try:
        r = rsp.xchg("qSupported:multiprocess+;swbreak+;hwbreak+")
        check(r is not None, "qSupported got a reply")
        check("qXfer:memory-map:read+" in r, "qSupported advertises memory-map")
        check("swbreak+" in r, "qSupported advertises swbreak")
        check("hwbreak+" in r, "qSupported advertises hwbreak")
        # --gdb-reverse was not passed by run_tests.sh's default launch, so
        # reverse-exec must NOT be advertised: avr-gdb 7.8 crashes its own
        # execution control the moment it is. See doc/notes-gdbstub.md.
        check("ReverseStep" not in r,
              "qSupported must not advertise reverse-exec without --gdb-reverse")
    finally:
        rsp.close()


def test_registers_roundtrip():
    rsp = connect()
    try:
        g = rsp.xchg("g")
        check(g is not None and len(g) == 39 * 2,
              "g reply is 39 bytes of register data, got %r" % (g,))

        r = rsp.xchg("P10=ab")  # r16
        check(r == "OK", "P10=ab (write r16) OK")
        r = rsp.xchg("p10")
        check(r == "ab", "p10 (read r16) reflects the write, got %r" % r)
    finally:
        rsp.close()


def test_memory_flash_read():
    rsp = connect()
    try:
        # First 4 bytes of test/roms/Basic.hex: 0C 94 41 08
        r = rsp.xchg("m0,4")
        check(r == "0c944108", "flash read at 0 matches Basic.hex header, got %r" % r)
    finally:
        rsp.close()


def test_memory_data_rw():
    rsp = connect()
    try:
        orig = rsp.xchg("m800300,1")
        check(orig is not None and len(orig) == 2,
              "read the original byte before overwriting it, got %r" % orig)

        r = rsp.xchg("M800300,1:5a")
        check(r == "OK", "data-space write OK")
        r = rsp.xchg("m800300,1")
        check(r == "5a", "data-space read reflects the write, got %r" % r)

        # Put it back. This is not just tidiness: on Basic.hex, SRAM 0x0300 is
        # a character cell in the interpreter's video RAM, so a byte left
        # behind is drawn on screen for the rest of the session (0x5a shows as
        # a stray "z") and reads as an emulator fault when it is nothing of
        # the kind.
        r = rsp.xchg("M800300,1:%s" % orig)
        check(r == "OK", "the scratch byte is restored")
        check(rsp.xchg("m800300,1") == orig,
              "and reads back as it was, got %r" % rsp.xchg("m800300,1"))
    finally:
        rsp.close()


def test_memory_eeprom_rw():
    rsp = connect()
    try:
        orig = rsp.xchg("m810010,2")
        check(orig is not None and len(orig) == 4,
              "read the original EEPROM bytes first, got %r" % orig)

        r = rsp.xchg("M810010,2:cafe")
        check(r == "OK", "EEPROM write OK")
        r = rsp.xchg("m810010,2")
        check(r == "cafe", "EEPROM read reflects the write, got %r" % r)

        # EEPROM outlives a reset and can be dumped to disk, so leaving test
        # bytes in it would follow the ROM around.
        r = rsp.xchg("M810010,2:%s" % orig)
        check(r == "OK", "the EEPROM bytes are restored")
        check(rsp.xchg("m810010,2") == orig, "and read back as they were")
    finally:
        rsp.close()


def test_memory_cross_space_error():
    rsp = connect()
    try:
        # Flash is 0x10000 bytes (GDB_FLASH_SIZE); a read starting inside
        # flash and running past its end must fail rather than silently
        # continuing into the data space.
        r = rsp.xchg("m fffc,8".replace(" ", ""))
        check(r is not None and r.startswith("E"),
              "read spanning past the end of flash must error, got %r" % r)
    finally:
        rsp.close()


def test_spiram():
    rsp = connect()
    try:
        _, reply = qrcmd(rsp, "status")
        size = 0
        for line in _.splitlines():
            if line.startswith("spiram_size:"):
                size = int(line.split(":", 1)[1].strip())
        if size == 0:
            out, reply = qrcmd(rsp, "spiram read 0 1")
            check("no SPI RAM configured" in out,
                  "spiram read with no chip attached reports a clear error, got %r" % out)
        else:
            r = rsp.xchg("M%x,1:7b" % (0x820000))
            check(r == "OK", "SPI RAM write OK")
            r = rsp.xchg("m%x,1" % (0x820000))
            check(r == "7b", "SPI RAM read reflects the write, got %r" % r)
    finally:
        rsp.close()


def test_software_breakpoint():
    rsp = connect()
    try:
        upload_probe(rsp)
        set_pc(rsp, PROBE_ADDR)
        r = rsp.xchg("Z0,%x,2" % PROBE_NOP)
        check(r == "OK", "Z0 insert OK")

        stop = rsp.xchg("c", timeout=10.0)
        check(stop is not None and stop[0] == "T",
              "continue past a breakpoint yields a stop reply, got %r" % stop)
        if stop and stop[0] == "T":
            sig, regs, extra = parse_stop(stop)
            check(sig == "05", "stop signal is SIGTRAP (05), got %r" % sig)
            check("swbreak" in extra, "stop reason is swbreak, got %r" % extra)
            pc = le_pc_bytes(regs["22"])
            check(pc == PROBE_NOP,
                  "halted exactly at the breakpoint (0x%x), got 0x%x" % (PROBE_NOP, pc))

        r = rsp.xchg("z0,%x,2" % PROBE_NOP)
        check(r == "OK", "z0 remove OK")
    finally:
        rsp.close()


def test_breakpoint_not_swallowed_after_resume():
    """The first breakpoint reached after a resume must actually fire.

    Regression test for a bug where brk_skipbp -- the flag that stops a
    breakpoint you just continued from re-triggering on the instruction you
    resumed on -- was consumed by the first *hook invocation* after a resume.
    In the fast path (breakpoints only, cu_gdb_slow clear) the hook is only
    entered at breakpointed addresses, so the flag survived all the way to
    the next breakpoint, whichever one that was, and swallowed it. Two
    breakpoints and an order check catch it deterministically, without
    depending on cycle counts (a video ISR can fire in between)."""
    rsp = connect()
    try:
        upload_probe(rsp)
        set_pc(rsp, PROBE_ADDR)
        check(rsp.xchg("Z0,%x,2" % PROBE_STS) == "OK", "Z0 at probe+2 OK")
        check(rsp.xchg("Z0,%x,2" % PROBE_NOP) == "OK", "Z0 at probe+10 OK")

        stop = rsp.xchg("c", timeout=10.0)
        check(stop is not None and stop[0] == "T",
              "continue yields a stop reply, got %r" % stop)
        if stop and stop[0] == "T":
            sig, regs, extra = parse_stop(stop)
            pc = le_pc_bytes(regs["22"])
            check(pc == PROBE_STS,
                  "stopped at the first breakpoint reached (0x%x); pc=0x%x "
                  "means it was swallowed and a later one fired instead"
                  % (PROBE_STS, pc))

        rsp.xchg("z0,%x,2" % PROBE_STS)
        rsp.xchg("z0,%x,2" % PROBE_NOP)
    finally:
        rsp.close()


def test_write_watchpoint():
    rsp = connect()
    try:
        upload_probe(rsp)
        set_pc(rsp, PROBE_ADDR)
        r = rsp.xchg("Z2,%x,1" % WATCH_ADDR)
        check(r == "OK", "Z2 (write watchpoint) insert OK")

        stop = rsp.xchg("c", timeout=10.0)
        check(stop is not None and stop[0] == "T",
              "continue past a write watchpoint yields a stop reply, got %r" % stop)
        if stop and stop[0] == "T":
            sig, regs, extra = parse_stop(stop)
            check("watch" in extra, "stop reason is watch, got %r" % extra)
            check(extra.get("watch", "").lower() == "%x" % WATCH_ADDR,
                  "watch address matches the Z2 address (0x%x), got %r"
                  % (WATCH_ADDR, extra.get("watch")))

        r = rsp.xchg("z2,%x,1" % WATCH_ADDR)
        check(r == "OK", "z2 remove OK")
    finally:
        rsp.close()


def test_read_watchpoint_skips_prior_write():
    """A read-only watchpoint (Z3) on an address that is written and then
    read must fire at the read, not the write: confirms cu_gdbbrk_watch()'s
    type match (want=WRITE at the sts, want=READ at the lds) actually
    discriminates rather than firing on the first touch."""
    rsp = connect()
    try:
        upload_probe(rsp)
        set_pc(rsp, PROBE_ADDR)
        r = rsp.xchg("Z3,%x,1" % WATCH_ADDR)
        check(r == "OK", "Z3 (read watchpoint) insert OK")

        stop = rsp.xchg("c", timeout=10.0)
        check(stop is not None and stop[0] == "T",
              "continue past a read watchpoint yields a stop reply, got %r" % stop)
        if stop and stop[0] == "T":
            sig, regs, extra = parse_stop(stop)
            check("rwatch" in extra, "stop reason is rwatch, got %r" % extra)
            pc = le_pc_bytes(regs["22"])
            # The lds (read) is at PROBE_ADDR+6..+8; halting happens at the
            # end of that instruction, i.e. PC == PROBE_ADDR+10 (the nop),
            # never at PROBE_ADDR+2..+6 where the sts (write) sits.
            check(pc >= PROBE_ADDR + 10,
                  "read watchpoint fired after the lds, not the earlier sts "
                  "(pc=0x%x)" % pc)

        r = rsp.xchg("z3,%x,1" % WATCH_ADDR)
        check(r == "OK", "z3 remove OK")
    finally:
        rsp.close()


def test_access_watchpoint_fires_on_write():
    rsp = connect()
    try:
        upload_probe(rsp)
        set_pc(rsp, PROBE_ADDR)
        r = rsp.xchg("Z4,%x,1" % WATCH_ADDR)
        check(r == "OK", "Z4 (access watchpoint) insert OK")

        stop = rsp.xchg("c", timeout=10.0)
        check(stop is not None and stop[0] == "T",
              "continue past an access watchpoint yields a stop reply, got %r" % stop)
        if stop and stop[0] == "T":
            sig, regs, extra = parse_stop(stop)
            check("awatch" in extra, "stop reason is awatch, got %r" % extra)
            pc = le_pc_bytes(regs["22"])
            # Access watchpoints match on the first touch (write), so this
            # must halt right after the sts, well before the nop.
            check(pc < PROBE_NOP,
                  "access watchpoint fired at the first touch (the sts), "
                  "pc=0x%x should be < 0x%x" % (pc, PROBE_NOP))

        r = rsp.xchg("z4,%x,1" % WATCH_ADDR)
        check(r == "OK", "z4 remove OK")
    finally:
        rsp.close()


def test_step():
    rsp = connect()
    try:
        upload_probe(rsp)
        set_pc(rsp, PROBE_ADDR)
        stop = rsp.xchg("s", timeout=5.0)
        check(stop is not None and stop[0] == "T", "single-step yields a stop reply")
        if stop and stop[0] == "T":
            sig, regs, extra = parse_stop(stop)
            pc = le_pc_bytes(regs["22"])
            check(pc == PROBE_ADDR + 2,
                  "single step over the 1-word ldi lands at +2, got 0x%x" % pc)
    finally:
        rsp.close()


def test_monitor_status_and_help():
    rsp = connect()
    try:
        out, reply = qrcmd(rsp, "help")
        check(reply == "OK", "monitor help returns OK")
        check("status" in out and "spiram" in out,
              "monitor help lists known commands")

        out, reply = qrcmd(rsp, "status")
        check(reply == "OK", "monitor status returns OK")
        check("cycle:" in out and "frame:" in out,
              "monitor status reports cycle/frame counters, got %r" % out)

        # Whether the PC plays the sound: "muted" for a hidden run and with
        # --mute, "played" otherwise (the launcher decides which this is)
        out, reply = qrcmd(rsp, "status --json")
        check(json.loads(out).get("sound") in ("muted", "played"),
              "monitor status says whether the sound is played, got %r" % out)

        out, reply = qrcmd(rsp, "bogus-command-xyz")
        check(reply == "OK", "qRcmd always answers OK even for a bad command")
        check("error:" in out, "unknown monitor command reports an error, got %r" % out)
    finally:
        rsp.close()


def run_frames(rsp, n):
    """Runs n frames through the countdown, returning the cycles they took."""
    out, _ = qrcmd(rsp, "status --json")
    before = json.loads(out)["cycle"]
    qrcmd(rsp, "frames %d" % n)
    rsp.send("c")
    pkt = rsp.read_packet(20.0)
    out, _ = qrcmd(rsp, "status --json")
    return json.loads(out)["cycle"] - before, pkt


def test_monitor_frames():
    """"frames n" must run exactly n frames and then halt by itself.

    Asserted as a ratio rather than an absolute cycle count: a frame is only
    262 x 1820 cycles for a ROM whose video kernel generates sync properly,
    and the probe fixture has no video kernel at all, so its rows run to the
    1920 cycle sync error ceiling instead.
    """
    rsp = connect()
    try:
        one, pkt = run_frames(rsp, 1)
        check(pkt is not None and pkt.startswith("T"),
              "the frame countdown halts and reports a stop, got %r" % pkt)

        out, _ = qrcmd(rsp, "status --json")
        check(json.loads(out)["state"] == "halted",
              "halted after the frame count")

        ten, _ = run_frames(rsp, 10)
        check(ten == 10 * one,
              "10 frames costs exactly 10 single frames: %d vs 10 x %d"
              % (ten, one))

        out, _ = qrcmd(rsp, "frames --json")
        check(json.loads(out)["armed"] is False, "disarms once it has fired")

        out, _ = qrcmd(rsp, "frames zzz --json")
        check("error" in json.loads(out), "a bad frame count is an error")
    finally:
        rsp.close()


def test_monitor_input_press():
    """A timed press releases itself after the requested frames."""
    rsp = connect()
    try:
        out, _ = qrcmd(rsp, "input 1 press A 3 --json")
        r = json.loads(out)
        check(r["buttons"] == 0x100, "A is 0x100, got %r" % r)
        check(r["frames_left"] == 3, "the press is armed for 3 frames")

        # Bare "input <player>" reports without changing anything
        out, _ = qrcmd(rsp, "input 1 --json")
        check(json.loads(out)["buttons"] == 0x100, "reads the controller back")

        qrcmd(rsp, "frames 2")
        rsp.send("c"); rsp.read_packet(20.0)
        out, _ = qrcmd(rsp, "input 1 --json")
        check(json.loads(out)["buttons"] == 0x100, "still held part way through")

        qrcmd(rsp, "frames 3")
        rsp.send("c"); rsp.read_packet(20.0)
        out, _ = qrcmd(rsp, "input 1 --json")
        check(json.loads(out)["buttons"] == 0, "released once the frames ran out")

        # A plain set must cancel a press in flight
        qrcmd(rsp, "input 1 press A 60 --json")
        out, _ = qrcmd(rsp, "input 1 B --json")
        check(json.loads(out)["frames_left"] == 0, "a plain set cancels a press")
        qrcmd(rsp, "input 1 none")
    finally:
        rsp.close()


def test_monitor_key_and_type():
    """Key and text injection queue events; the errors stay well formed."""
    rsp = connect()
    try:
        out, _ = qrcmd(rsp, "key F9 --json")
        r = json.loads(out)
        check(r["key"] == "F9", "SDL resolves the key name, got %r" % r)
        check(r["mode"] == "tap" and r["queued"] >= 2,
              "a tap queues both edges, got %r" % r)

        out, _ = qrcmd(rsp, "key NoSuchKey --json")
        check("error" in json.loads(out), "an unknown key name is an error")
        out, _ = qrcmd(rsp, "key F9 sideways --json")
        check("error" in json.loads(out), "an unknown mode is an error")

        out, _ = qrcmd(rsp, "type hi --json")
        r = json.loads(out)
        check(r["characters"] == 2, "two characters, got %r" % r)
        check(r["frames"] == 4, "two events each, got %r" % r)

        # A capital costs a shift either side of the key
        out, _ = qrcmd(rsp, "type H --json")
        check(json.loads(out)["frames"] == 4, "a capital carries its shift")

        # The monitor line arrives already split on whitespace, so a newline
        # has to come through an escape or it cannot be typed at all
        out, _ = qrcmd(rsp, "type a\\nb --json")
        r = json.loads(out)
        check(r["characters"] == 3, "\\n is one character, got %r" % r)
        check(r["frames"] == 6, "and needs no shift, got %r" % r)

        out, _ = qrcmd(rsp, "type a\\\\b --json")
        check(json.loads(out)["characters"] == 3, "\\\\ is a literal backslash")

        out, _ = qrcmd(rsp, "type a\\qb --json")
        check("error" in json.loads(out), "an unknown escape is an error")

        out, _ = qrcmd(rsp, "type --json")
        check("error" in json.loads(out), "type needs something to type")
    finally:
        rsp.close()


def test_monitor_mouse():
    rsp = connect()
    try:
        out, _ = qrcmd(rsp, "mouse -12 34 left --json")
        r = json.loads(out)
        check(r["dx"] == -12 and r["dy"] == 34,
              "a negative delta survives the round trip, got %r" % r)
        check(r["buttons"] == 1, "left is button mask 1, got %r" % r)

        out, _ = qrcmd(rsp, "mouse a b --json")
        check("error" in json.loads(out), "a bad delta is an error")
        out, _ = qrcmd(rsp, "mouse 1 2 sideways --json")
        check("error" in json.loads(out), "a bad button name is an error")
    finally:
        rsp.close()


def test_monitor_wait():
    """--wait arms a halt for when the ROM has actually read the input.

    Basic.hex enables the keyboard dongle, so this exercises the real
    condition: our queue empty and cu_kbd_queue drained by cu_kbd_process().
    Reset first, because an earlier test may have left the PC parked at the
    probe, and a ROM that is not running its own code never reads the
    keyboard -- which is the timeout case, not this one.
    """
    rsp = connect()
    try:
        qrcmd(rsp, "reset")
        rsp.send("c")
        time.sleep(4.0)                     # let BASIC boot and probe the dongle

        out, _ = qrcmd(rsp, "type ab --wait --json")
        r = json.loads(out)
        check(r["waiting"] is True, "--wait reports that it armed, got %r" % r)

        out, _ = qrcmd(rsp, "status --json")
        check(json.loads(out)["waited"] == "pending",
              "status reports the wait as pending")

        pkt = rsp.read_packet(30.0)
        check(pkt is not None and pkt.startswith("T"),
              "the wait halts by itself, got %r" % pkt)

        out, _ = qrcmd(rsp, "status --json")
        check(json.loads(out)["waited"] == "drained",
              "and reports how it ended, got %r" % out)
    finally:
        rsp.close()


def test_monitor_wait_parsing():
    """Flag parsing only, with the CPU halted throughout.

    Deliberately halted: cu_gdbmon_frame() does not run while the CPU is, so
    an armed wait cannot fire and drop an unsolicited stop reply into the
    middle of the next command's output.
    """
    rsp = connect()
    try:
        rsp.send_raw("\x03")
        rsp.read_packet(5.0)

        out, _ = qrcmd(rsp, "key F7 --wait=10 --json")
        check(json.loads(out)["waiting"] is True, "--wait=N arms too")

        out, _ = qrcmd(rsp, "type x --wait=zz --json")
        check("error" in json.loads(out), "a bad settle count is an error")

        # Without --wait nothing is armed, and the field still reports
        out, _ = qrcmd(rsp, "key F7 --json")
        check(json.loads(out)["waiting"] is False, "no --wait, no wait")

        qrcmd(rsp, "reset")     # discards the queued keys and the armed wait
    finally:
        rsp.close()


def test_monitor_reset_matches_R():
    """monitor reset and the R packet must leave the same machine behind.

    They did not: monitor reset skipped the widened cycle counter and the
    reverse execution history, so both went on describing the pre-reset run.
    """
    rsp = connect()
    try:
        rsp.send("c")
        time.sleep(1.0)
        rsp.send_raw("\x03")
        rsp.read_packet(5.0)

        out, _ = qrcmd(rsp, "status --json")
        before = json.loads(out)["cycle"]
        check(before > 0, "the emulator ran, so there are cycles to clear")

        # Arm injected state that a reset has to discard
        qrcmd(rsp, "input 0 press A 600")
        qrcmd(rsp, "frames 500")

        qrcmd(rsp, "reset")
        out, _ = qrcmd(rsp, "status --json")
        after = json.loads(out)
        check(after["cycle"] < before,
              "monitor reset zeroes the cycle counter: %d -> %d"
              % (before, after["cycle"]))
        check(after["frame"] == 0, "and the frame counter with it")
        check(after["waited"] == "none", "and disarms any wait")

        out, _ = qrcmd(rsp, "frames --json")
        check(json.loads(out)["armed"] is False, "and the frame countdown")
        out, _ = qrcmd(rsp, "input 0 --json")
        check(json.loads(out)["frames_left"] == 0, "and a timed press")

        # The R packet, which has no reply, must land in the same place
        rsp.send("c")
        time.sleep(1.0)
        rsp.send_raw("\x03")
        rsp.read_packet(5.0)
        out, _ = qrcmd(rsp, "status --json")
        mid = json.loads(out)["cycle"]
        rsp.send("R")
        time.sleep(0.5)
        out, _ = qrcmd(rsp, "status --json")
        check(json.loads(out)["cycle"] < mid,
              "the R packet zeroes it too, from %d" % mid)
    finally:
        rsp.close()


def test_memory_map_xfer():
    rsp = connect()
    try:
        r = rsp.xchg("qXfer:memory-map:read::0,100")
        check(r is not None and r[0] in ("m", "l"),
              "qXfer:memory-map:read replies with an m/l-prefixed chunk, got %r" % r)
        check("<memory-map>" in r, "memory-map XML has the expected root tag")
    finally:
        rsp.close()


def test_vflash_load():
    rsp = connect()
    try:
        r = rsp.xchg("vFlashErase:%x,4" % PROBE_ADDR)
        check(r == "OK", "vFlashErase OK")
        r = rsp.xchg("m%x,4" % PROBE_ADDR)
        check(r == "ffffffff", "erased flash reads as 0xFF, got %r" % r)

        r = rsp.xchg("vFlashWrite:%x:\x11\x22\x33\x44" % PROBE_ADDR)
        check(r == "OK", "vFlashWrite OK")
        r = rsp.xchg("vFlashDone")
        check(r == "OK", "vFlashDone OK")

        r = rsp.xchg("m%x,4" % PROBE_ADDR)
        check(r == "11223344", "vFlash-written bytes are readable back, got %r" % r)

        # Leave the scratch area holding the probe routine again for any
        # test run afterwards (order independence).
        upload_probe(rsp)
    finally:
        rsp.close()


def test_detach_resumes():
    rsp = connect()
    try:
        r = rsp.xchg("D")
        check(r == "OK", "detach returns OK")
    finally:
        rsp.close()
    # A second connection succeeding at all proves the listener survived
    # the previous client's detach.
    rsp2 = connect()
    try:
        r = rsp2.xchg("qSupported")
        check(r is not None, "a fresh connection is accepted after a detach")
    finally:
        rsp2.close()


ALL_TESTS = [
    test_qsupported,
    test_registers_roundtrip,
    test_memory_flash_read,
    test_memory_data_rw,
    test_memory_eeprom_rw,
    test_memory_cross_space_error,
    test_spiram,
    test_software_breakpoint,
    test_breakpoint_not_swallowed_after_resume,
    test_write_watchpoint,
    test_read_watchpoint_skips_prior_write,
    test_access_watchpoint_fires_on_write,
    test_step,
    test_monitor_status_and_help,
    test_monitor_frames,
    test_monitor_input_press,
    test_monitor_key_and_type,
    test_monitor_mouse,
    test_monitor_wait_parsing,
    test_memory_map_xfer,
    test_vflash_load,
    test_detach_resumes,
    # These reset the machine, so they run last and cannot disturb the rest.
    test_monitor_wait,
    test_monitor_reset_matches_R,
]


def cleanup():
    """Leaves the emulator running its ROM, not parked in the probe fixture.

    Several tests redirect the PC into fixtures/probe.S, which ends in a
    branch to itself. Without this the ROM never gets control back after the
    suite: the window sits on a frozen screen with the PC at PROBE_ADDR, and
    the probe goes on rewriting its scratch byte at data 0x0200 forever, so
    restoring that byte from here would not stick either. A reset is what
    actually ends it.
    """
    rsp = connect()
    try:
        qrcmd(rsp, "reset")
    finally:
        rsp.close()


def main():
    if not os.path.exists(PROBE_BIN):
        print("missing %s -- build it first (see run_tests.sh), e.g.:" % PROBE_BIN)
        print("  avr-gcc -mmcu=atmega644 -nostdlib -c fixtures/probe.S -o fixtures/probe.o")
        print("  avr-gcc -mmcu=atmega644 -nostdlib -Wl,--section-start=.text=0 \\")
        print("      -o fixtures/probe.elf fixtures/probe.o")
        print("  avr-objcopy -O binary --only-section=.text fixtures/probe.elf fixtures/probe.bin")
        return 1

    for t in ALL_TESTS:
        name = t.__name__
        try:
            t()
        except RSPError as e:
            global _fail_count
            _fail_count += 1
            print("FAIL: %s raised %s" % (name, e))
        except Exception as e:  # noqa: BLE001
            _fail_count += 1
            print("FAIL: %s raised unexpected %r" % (name, e))

    # Always, including after a failure -- a half-run suite is exactly when
    # the machine is most likely to be left parked in the probe.
    try:
        cleanup()
    except Exception as e:  # noqa: BLE001
        print("note: could not restore the emulator afterwards: %r" % e)

    print("---")
    print("%d passed, %d failed" % (_pass_count, _fail_count))
    return 1 if _fail_count else 0


if __name__ == "__main__":
    sys.exit(main())

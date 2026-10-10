#!/usr/bin/env python3
#
# Minimal GDB Remote Serial Protocol client used by the scripted end-to-end
# tests in test_stub.py. Deliberately independent of avr-gdb: it drives the
# wire protocol directly so a test failure points at the stub, never at a
# GDB client version quirk.
#
# Copyright (C) 2026
#   Alec Bourque (Uze)
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
import socket
import time


class RSPError(Exception):
    pass


class RSPClient:

    def __init__(self, host="127.0.0.1", port=1234, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""
        self.default_timeout = timeout

    def close(self):
        self.sock.close()

    @staticmethod
    def _checksum(data):
        return sum(data) & 0xFF

    def send(self, body):
        if isinstance(body, str):
            body = body.encode("latin-1")
        pkt = b"$" + body + b"#" + ("%02x" % self._checksum(body)).encode()
        self.sock.sendall(pkt)

    def send_raw(self, raw):
        if isinstance(raw, str):
            raw = raw.encode("latin-1")
        self.sock.sendall(raw)

    def _fill(self, timeout):
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return False
            self.sock.settimeout(min(remaining, 1.0))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return False
            self.buf += chunk
            return True

    def read_packet(self, timeout=None):
        """Reads one $...#XX packet body (without $, #, checksum), ACKing
        it. Silently consumes leading +/- bytes. Returns None on timeout."""
        if timeout is None:
            timeout = self.default_timeout
        deadline = time.time() + timeout
        while True:
            while self.buf[:1] in (b"+", b"-"):
                self.buf = self.buf[1:]
            if b"$" in self.buf:
                start = self.buf.index(b"$")
                self.buf = self.buf[start:]
                if b"#" in self.buf:
                    hashidx = self.buf.index(b"#")
                    if len(self.buf) >= hashidx + 3:
                        pkt = self.buf[:hashidx + 3]
                        self.buf = self.buf[hashidx + 3:]
                        self.sock.sendall(b"+")
                        return pkt[1:hashidx].decode("latin-1")
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            if not self._fill(min(remaining, 1.0)):
                if time.time() >= deadline:
                    return None

    def xchg(self, body, timeout=None):
        """Sends a packet and returns the decoded reply body."""
        self.send(body)
        return self.read_packet(timeout)

    def expect_ok(self, body, timeout=None):
        r = self.xchg(body, timeout)
        if r != "OK":
            raise RSPError("expected OK for %r, got %r" % (body, r))
        return r

    def interrupt(self):
        """Sends the raw Ctrl-C interrupt byte (no packet framing)."""
        self.send_raw(b"\x03")


def unescape_binary(data):
    """Undoes RSP binary escaping (X/vFlashWrite payloads): 0x7d is an
    escape, the following byte is XORed with 0x20."""
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0x7D:
            i += 1
            out.append(data[i] ^ 0x20)
        else:
            out.append(b)
        i += 1
    return bytes(out)


def escape_binary(data):
    out = bytearray()
    for b in data:
        if b in (0x23, 0x24, 0x2A, 0x7D):
            out.append(0x7D)
            out.append(b ^ 0x20)
        else:
            out.append(b)
    return bytes(out)

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the real Linux peer against a PTY, without QEMU or root access."""
import binascii
import os
import pathlib
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time


def encode(kind, *, epoch=42, sequence=1, operation=0, argument=0):
    body = struct.pack("<2sBBHBBIIII", b"\xa5\x5a", 1, kind, 28, 2, 0,
                       epoch, sequence, operation, argument)
    return body + struct.pack("<I", binascii.crc32(body))


class Stream:
    def __init__(self, fd):
        self.fd = fd
        self.buffer = bytearray()
        self.history = []

    def receive(self, predicate, timeout=2):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            while len(self.buffer) >= 28:
                assert self.buffer[:2] == b"\xa5\x5a", self.buffer
                body = bytes(self.buffer[:28])
                del self.buffer[:28]
                assert binascii.crc32(body[:24]) == struct.unpack_from("<I", body, 24)[0]
                values = struct.unpack("<2sBBHBBIIIII", body)
                frame = dict(zip(("magic", "version", "type", "size", "sender", "flags",
                                  "epoch", "sequence", "progress", "argument", "crc"), values))
                self.history.append(frame)
                if predicate(frame):
                    return frame
            if select.select([self.fd], [], [], min(0.1, max(0, end - time.monotonic())))[0]:
                self.buffer.extend(os.read(self.fd, 1024))
        raise TimeoutError("No matching peer frame")

    def command(self, transaction, operation, argument=0, *, epoch=42, fragmented=False):
        frame = encode(5, epoch=epoch, sequence=transaction, operation=operation, argument=argument)
        if fragmented:
            os.write(self.fd, b"\xff\xa5" + frame[:7])
            time.sleep(0.02)
            os.write(self.fd, frame[7:])
        else:
            os.write(self.fd, frame)
        return self.receive(lambda f: f["type"] == 6 and f["sequence"] == transaction)


def main(binary):
    with tempfile.TemporaryDirectory(prefix="vmcu-peer-") as temp:
        directory = pathlib.Path(temp)
        master, slave = pty.openpty()
        initial_termios = termios.tcgetattr(slave)
        command = [binary, "--device", os.ttyname(slave), "--epoch", "42",
                   "--lock-file", str(directory / "lock"), "--pid-file", str(directory / "pid")]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
            try:
                stream = Stream(master)
                assert stream.receive(lambda f: f["type"] == 1)["epoch"] == 42
                os.write(master, encode(2))
                # Locking rejects a second instance without touching the UART.
                duplicate = subprocess.run(command, capture_output=True, text=True, timeout=2)
                assert duplicate.returncode == 1 and "reason=lock" in duplicate.stderr
                assert stream.command(1, 1, fragmented=True)["flags"] == 0
                status = stream.command(2, 2)
                assert status["flags"] == 0 and status["argument"] == 0
                # AP accepts management only; PFDI reporting belongs to SI CL0.
                assert stream.command(3, 3, 0)["flags"] == 1
                assert stream.command(4, 4, 1)["flags"] == 1
                assert stream.command(5, 5)["flags"] == 1
                assert stream.command(6, 6)["flags"] == 1
                assert stream.command(7, 99)["flags"] == 1
                assert stream.command(7, 99)["flags"] == 1  # exact duplicate
                assert stream.command(7, 99, 1)["flags"] == 2  # changed duplicate
                assert stream.command(6, 1)["flags"] == 3  # stale transaction
                assert stream.command(8, 1, 2)["flags"] == 2  # bad ping argument
                bad = bytearray(encode(5, sequence=9, operation=1))
                bad[-1] ^= 1
                os.write(master, bad + encode(5, epoch=41, sequence=9, operation=1))
                try:
                    stream.receive(lambda f: f["type"] != 1, timeout=1.25)
                    raise AssertionError("Invalid traffic produced a reply or safety traffic")
                except TimeoutError:
                    pass
                assert stream.command(9, 2)["flags"] == 0
                # A newly started MCU discovers the existing sequence cursor
                # instead of resetting the peer's replay protection.
                session = stream.command(0, 7)
                assert session["flags"] == 0 and session["argument"] == 9
                assert stream.command(10, 2)["flags"] == 0
                assert stream.command(9, 2)["flags"] == 3
                # Never issue POWER_OFF to the host fixture. Its authorization
                # is covered by the pure RPC test; kernel shutdown is guest-only.
                # Re-HELLO continues, but no periodic HEALTH / synthetic progress
                # or SI safety report may appear, even after unsupported commands.
                stream.receive(lambda f: f["type"] == 1, timeout=2)
                try:
                    stream.receive(lambda f: f["type"] != 1, timeout=1.25)
                    raise AssertionError("AP generated unsolicited non-management traffic")
                except TimeoutError:
                    pass
                assert all(f["type"] in (1, 6) for f in stream.history)
                print("PTY management/CRC/replay/lock/lifecycle/no-AP-HEALTH: PASS")
            finally:
                process.terminate()
                output, errors = process.communicate(timeout=3)
                assert process.returncode == 0, errors
                assert "VMCU_AP_LINK epoch=42" in output
                assert not (directory / "pid").exists()
                assert termios.tcgetattr(slave) == initial_termios
                os.close(master)
                os.close(slave)


if __name__ == "__main__":
    main(sys.argv[1])

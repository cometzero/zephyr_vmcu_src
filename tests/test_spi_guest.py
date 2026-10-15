#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real Zephyr QSPI driver/IRQ/GPIO/ACK against a host SI fixture."""
import json
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def packet(raw, kind, data=None):
    out = bytearray(raw)
    out[4], out[5] = kind, 0
    if data is not None:
        out[28] = data
    out[60:] = struct.pack('<I', zlib.crc32(out[:60]))
    return out


def main(qemu, elf):
    with tempfile.TemporaryDirectory() as tmp:
        args = [qemu, '-M', 'KIT_AURIX_TC397B_TRB', '-S', '-display', 'none',
                '-kernel', elf, '-serial', 'null', '-serial', 'chardev:console', '-serial', 'null',
                '-qmp', f'unix:{tmp}/qmp,server=on,wait=off',
                '-chardev', f'socket,id=console,path={tmp}/console,server=on,wait=off']
        for name, model in [('spi', 'tc397-qspi'), ('gpio', 'tricore-port')]:
            args += ['-chardev', f'socket,id={name},path={tmp}/{name},server=on,wait=off',
                     '-global', f'{model}.chardev={name}']
        p = subprocess.Popen(args, stderr=subprocess.PIPE)
        try:
            def connect(name):
                for _ in range(100):
                    if os.path.exists(tmp + '/' + name): break
                    time.sleep(.02)
                s = socket.socket(socket.AF_UNIX)
                s.settimeout(2)
                s.connect(tmp + '/' + name)
                return s
            spi, gpio, qmp = connect('spi'), connect('gpio'), connect('qmp')
            console = connect('console')
            logs = bytearray()
            qmp.recv(4096)
            qmp.sendall(b'{"execute":"qmp_capabilities"}\n'); qmp.recv(4096)
            raw = spi.recv(64)
            assert len(raw) == 64 and raw[4] == 1
            spi.sendall(packet(raw, 2))
            gpio.sendall(b'GP\x01I' + struct.pack('<HH', 4, 0x27))
            qmp.sendall(b'{"execute":"cont"}\n')
            frame = bytearray([0xa5, 0x5a, 1, 7, 28, 0, 3, 1])
            frame += struct.pack('<IIII', 123, 1, 0x00010001, 0)
            frame += struct.pack('<I', zlib.crc32(frame))
            tx, miso, synced, ack = bytearray(), bytearray(), False, 0
            publish_at = None
            delayed_request = False
            command_received = False
            def health(sequence):
                out = bytearray(frame)
                struct.pack_into('<I', out, 12, sequence)
                struct.pack_into('<I', out, 24, zlib.crc32(out[:24]))
                return out
            deadline = time.monotonic() + 20
            buffer = bytearray()
            while time.monotonic() < deadline and ack < 3:
                if publish_at is not None and time.monotonic() >= publish_at:
                    gpio.sendall(b'GP\x01I' + struct.pack('<HH', 4, 0x27))
                    delayed_request = True
                    publish_at = None
                readable = select.select([spi, console], [], [], .005)[0]
                if console in readable:
                    logs += console.recv(4096)
                if spi not in readable: continue
                buffer += spi.recv(4096)
                while len(buffer) >= 64:
                    raw, buffer = buffer[:64], buffer[64:]
                    if raw[4] == 1:
                        spi.sendall(packet(raw, 2)); continue
                    assert raw[4] == 3
                    tx.append(raw[28])
                    spi.sendall(packet(raw, 4, miso.pop(0) if miso else 0xff))
                    if raw[6] & 2:
                        assert len(tx) == 28
                        if tx[3] == 9:
                            synced = True
                            miso = bytearray(frame)
                            gpio.sendall(b'GP\x01I' + struct.pack('<HH', 0x24, 0x27))
                        elif tx[3] == 5:
                            command_received = True
                            # Frame2 was consumed on MISO before its independent GPIO arrives.
                            publish_at = time.monotonic() + .15
                        elif tx[3] == 8:
                            ack += 1
                            assert struct.unpack_from('<III', tx, 8) == (123, ack, 7)
                            assert zlib.crc32(tx[:24]) == struct.unpack_from('<I', tx, 24)[0]
                            if ack == 1:
                                miso = health(2)
                                console.sendall(b'vmcu-cli safety ping\r')
                            elif ack == 2:
                                assert command_received and delayed_request, "ACK preceded retained GPIO"
                                miso = health(3)
                                gpio.sendall(b'GP\x01I' + struct.pack('<HH', 0x24, 0x27))
                        tx.clear()
            time.sleep(.1)
            while select.select([console], [], [], .05)[0]:
                logs += console.recv(4096)
            log = logs.decode(errors='replace')
            print(log)
            assert synced and ack == 3 and 'VMCU_SPI rx=3 ack=3 errors=0' in log
            print('PASS: real Zephyr QSPI IRQ, CRC/ACK, delayed command-MISO GPIO and consecutive request')
        finally:
            p.terminate(); p.wait(timeout=5)


if __name__ == '__main__':
    main(*sys.argv[1:])

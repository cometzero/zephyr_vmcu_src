/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "protocol.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = value >> (i * 8);
    }
}

uint32_t vmcu_crc32(const uint8_t *bytes, unsigned length)
{
    uint32_t crc = UINT32_MAX;

    for (unsigned i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }
    return crc ^ UINT32_MAX;
}

void vmcu_encode(uint8_t bytes[VMCU_FRAME_SIZE], const struct vmcu_frame *frame)
{
    bytes[0] = 0xa5;
    bytes[1] = 0x5a;
    bytes[2] = VMCU_VERSION;
    bytes[3] = frame->type;
    bytes[4] = VMCU_FRAME_SIZE;
    bytes[5] = 0;
    bytes[6] = frame->sender;
    bytes[7] = frame->flags;
    put32(bytes + 8, frame->epoch);
    put32(bytes + 12, frame->sequence);
    put32(bytes + 16, frame->progress);
    put32(bytes + 20, frame->argument);
    put32(bytes + 24, vmcu_crc32(bytes, 24));
}

bool vmcu_decode(const uint8_t bytes[VMCU_FRAME_SIZE], struct vmcu_frame *frame)
{
    if (bytes[0] != 0xa5 || bytes[1] != 0x5a || bytes[2] != VMCU_VERSION ||
        bytes[3] < VMCU_HELLO || bytes[3] > VMCU_SAFETY_HEALTH ||
        bytes[4] != VMCU_FRAME_SIZE || bytes[5] != 0 ||
        (bytes[6] != VMCU_AP && bytes[6] != VMCU_MCU && bytes[6] != VMCU_SI0) ||
        get32(bytes + 24) != vmcu_crc32(bytes, 24)) {
        return false;
    }
    frame->type = bytes[3];
    frame->sender = bytes[6];
    frame->flags = bytes[7];
    frame->epoch = get32(bytes + 8);
    frame->sequence = get32(bytes + 12);
    frame->progress = get32(bytes + 16);
    frame->argument = get32(bytes + 20);
    return true;
}

static void stream_shift(struct vmcu_stream *stream)
{
    for (unsigned i = 1; i < stream->used; i++) {
        stream->bytes[i - 1] = stream->bytes[i];
    }
    stream->used--;
}

bool vmcu_feed(struct vmcu_stream *stream, uint8_t byte, struct vmcu_frame *frame)
{
    stream->bytes[stream->used++] = byte;
    while (stream->used && (stream->bytes[0] != 0xa5 ||
           (stream->used >= 2 && stream->bytes[1] != 0x5a) ||
           (stream->used >= 3 && stream->bytes[2] != VMCU_VERSION) ||
           (stream->used >= 6 && (stream->bytes[4] != VMCU_FRAME_SIZE ||
                                  stream->bytes[5] != 0)))) {
        stream_shift(stream);
    }
    if (stream->used == VMCU_FRAME_SIZE) {
        if (vmcu_decode(stream->bytes, frame)) {
            stream->used = 0;
            return true;
        }
        stream_shift(stream);
    }
    return false;
}

bool vmcu_newer(uint32_t candidate, uint32_t previous)
{
    uint32_t distance = candidate - previous;

    return distance != 0 && distance < 0x80000000u;
}

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMCU_PROTOCOL_H
#define VMCU_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#define VMCU_FRAME_SIZE 28
#define VMCU_VERSION 1
#define VMCU_PERIOD_MS 100
#define VMCU_DEADLINE_MS 1000

enum vmcu_type {
    VMCU_HELLO = 1, VMCU_HELLO_ACK, VMCU_HEALTH, VMCU_STATUS,
    VMCU_COMMAND, VMCU_REPLY, VMCU_SAFETY_HEALTH
};
enum vmcu_sender { VMCU_AP = 1, VMCU_MCU = 2, VMCU_SI0 = 3 };
enum vmcu_mode { VMCU_WAIT, VMCU_SYNC, VMCU_RUN, VMCU_DEGRADED };
enum vmcu_fault { VMCU_NONE, VMCU_LINK_TIMEOUT, VMCU_WORKLOAD_STALLED };

struct vmcu_frame {
    uint8_t type, sender, flags;
    uint32_t epoch, sequence, progress, argument;
};

struct vmcu_stream {
    uint8_t bytes[VMCU_FRAME_SIZE];
    unsigned used;
};

uint32_t vmcu_crc32(const uint8_t *bytes, unsigned length);
void vmcu_encode(uint8_t bytes[VMCU_FRAME_SIZE], const struct vmcu_frame *frame);
bool vmcu_decode(const uint8_t bytes[VMCU_FRAME_SIZE], struct vmcu_frame *frame);
bool vmcu_feed(struct vmcu_stream *stream, uint8_t byte, struct vmcu_frame *frame);
bool vmcu_newer(uint32_t candidate, uint32_t previous);

#endif

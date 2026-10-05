/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMCU_CAN_POLICY_H
#define VMCU_CAN_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#define VMCU_CAN_COMMAND 0x600u
#define VMCU_CAN_REPLY 0x601u
#define VMCU_CAN_TELEMETRY 0x510u
#define VMCU_CAN_COMMAND_SIZE 24u
enum vehicle_result { VEHICLE_OK, VEHICLE_UNSUPPORTED, VEHICLE_INVALID, VEHICLE_STALE,
                      VEHICLE_IO, VEHICLE_UNAVAILABLE };
struct vehicle_session {
    uint32_t epoch, cookie, last;
    bool cached;
    uint8_t request[24], reply[24];
};
uint32_t vehicle_get32(const uint8_t *p);
void vehicle_put32(uint8_t *p, uint32_t value);
/* Returns 1 for a new command, 0 for a cached/rejected reply. */
int vehicle_request(struct vehicle_session *session, const uint8_t request[24],
                    uint8_t reply[24]);
void vehicle_complete(struct vehicle_session *session, const uint8_t request[24],
                      const uint8_t reply[24]);
#endif

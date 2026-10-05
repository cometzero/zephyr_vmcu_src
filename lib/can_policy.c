/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "can_policy.h"
uint32_t vehicle_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void vehicle_put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) { p[i] = value >> (8 * i); }
}
int vehicle_request(struct vehicle_session *s, const uint8_t request[24], uint8_t reply[24])
{
    uint32_t seq = vehicle_get32(request + 12), arg = vehicle_get32(request + 16);
    memset(reply, 0, 24);
    reply[0] = 1; reply[1] = request[1];
    vehicle_put32(reply + 4, s->epoch);
    vehicle_put32(reply + 8, s->cookie);
    vehicle_put32(reply + 12, seq);
    if (request[0] != 1 || request[2] || request[3] || vehicle_get32(request + 20)) {
        reply[2] = VEHICLE_INVALID; return 0;
    }
    if (!s->epoch || !s->cookie || vehicle_get32(request + 4) != s->epoch ||
        vehicle_get32(request + 8) != s->cookie || !seq) {
        reply[2] = VEHICLE_STALE; return 0;
    }
    if (s->cached && seq == s->last) {
        if (!memcmp(s->request, request, 24)) { memcpy(reply, s->reply, 24); }
        else { reply[2] = VEHICLE_INVALID; }
        return 0;
    }
    if (s->cached && (int32_t)(seq - s->last) <= 0) { reply[2] = VEHICLE_STALE; return 0; }
    if (request[1] < 1 || request[1] > 7) { reply[2] = VEHICLE_UNSUPPORTED; return 0; }
    if ((request[1] == 3 && arg > 8) || (request[1] == 4 && arg > 10) ||
        ((request[1] != 3 && request[1] != 4) && arg)) {
        reply[2] = VEHICLE_INVALID; return 0;
    }
    return 1;
}
void vehicle_complete(struct vehicle_session *s, const uint8_t request[24], const uint8_t reply[24])
{
    s->last = vehicle_get32(request + 12);
    s->cached = true;
    memcpy(s->request, request, 24);
    memcpy(s->reply, reply, 24);
}

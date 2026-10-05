/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "can_policy.h"

static struct vehicle_session fresh(void)
{
    return (struct vehicle_session) { .epoch = 0x12345678, .cookie = 99 };
}
static void request(uint8_t data[24], const struct vehicle_session *s,
                    uint8_t op, uint32_t sequence, uint32_t arg)
{
    memset(data, 0, 24);
    data[0] = 1; data[1] = op;
    vehicle_put32(data + 4, s->epoch);
    vehicle_put32(data + 8, s->cookie);
    vehicle_put32(data + 12, sequence);
    vehicle_put32(data + 16, arg);
}
static void reject_unchanged(struct vehicle_session *s, const uint8_t data[24],
                             uint8_t expected)
{
    struct vehicle_session before = *s;
    uint8_t reply[24];
    assert(vehicle_request(s, data, reply) == 0);
    assert(reply[0] == 1 && reply[1] == data[1] && reply[2] == expected);
    assert(vehicle_get32(reply + 4) == s->epoch);
    assert(vehicle_get32(reply + 8) == s->cookie);
    assert(vehicle_get32(reply + 12) == vehicle_get32(data + 12));
    assert(!memcmp(s, &before, sizeof(before)));
}
static void malformed(void)
{
    struct vehicle_session s = fresh();
    uint8_t data[24];
    const unsigned reserved[] = { 2, 3, 20, 21, 22, 23 };
    request(data, &s, 1, 1, 0);
    assert(data[4] == 0x78 && data[7] == 0x12);
    data[0] = 0; reject_unchanged(&s, data, VEHICLE_INVALID);
    data[0] = 2; reject_unchanged(&s, data, VEHICLE_INVALID);
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        request(data, &s, 1, 1, 0);
        data[reserved[i]] = 0x80;
        reject_unchanged(&s, data, VEHICLE_INVALID);
    }
    puts("CAN policy malformed/version/reserved: PASS");
}
static void session(void)
{
    struct vehicle_session s = fresh();
    uint8_t data[24];
    request(data, &s, 1, 0, 0);
    reject_unchanged(&s, data, VEHICLE_STALE);
    request(data, &s, 1, 1, 0);
    vehicle_put32(data + 4, s.epoch - 1);
    reject_unchanged(&s, data, VEHICLE_STALE);
    vehicle_put32(data + 4, s.epoch);
    vehicle_put32(data + 8, s.cookie - 1);
    reject_unchanged(&s, data, VEHICLE_STALE);
    request(data, &s, 1, 1, 0); s.epoch = 0;
    reject_unchanged(&s, data, VEHICLE_STALE);
    s = fresh(); s.cookie = 0;
    reject_unchanged(&s, data, VEHICLE_STALE);
    puts("CAN policy absent/stale session and zero sequence: PASS");
}
static void operations(void)
{
    struct vehicle_session s = fresh();
    uint8_t data[24], reply[24];
    for (unsigned op = 1; op <= 7; op++) {
        request(data, &s, op, op, 0);
        assert(vehicle_request(&s, data, reply) == 1);
        assert(reply[2] == VEHICLE_OK && !s.cached);
        vehicle_put32(data + 16, op == 3 ? 9 : op == 4 ? 11 : 1);
        reject_unchanged(&s, data, VEHICLE_INVALID);
    }
    request(data, &s, 3, 1, 8);
    assert(vehicle_request(&s, data, reply) == 1);
    request(data, &s, 4, 1, 10);
    assert(vehicle_request(&s, data, reply) == 1);
    const uint8_t unsupported[] = { 0, 8, 0xff };
    for (unsigned i = 0; i < sizeof(unsupported); i++) {
        request(data, &s, unsupported[i], 1, 0);
        reject_unchanged(&s, data, VEHICLE_UNSUPPORTED);
    }
    puts("CAN policy operation allowlist and argument boundaries: PASS");
}
static void replay(void)
{
    struct vehicle_session s = fresh();
    uint8_t data[24], reply[24], cached[24];
    request(data, &s, 5, 44, 0);
    assert(vehicle_request(&s, data, reply) == 1);
    reply[2] = VEHICLE_IO; /* An uncertain actuator outcome must not be rerun. */
    vehicle_put32(reply + 16, 0xdeadbeef);
    memcpy(cached, reply, sizeof(cached));
    vehicle_complete(&s, data, reply);
    assert(s.cached && s.last == 44);
    memset(reply, 0xff, sizeof(reply));
    assert(vehicle_request(&s, data, reply) == 0);
    assert(!memcmp(reply, cached, sizeof(reply)));
    data[1] = 6; reject_unchanged(&s, data, VEHICLE_INVALID);
    request(data, &s, 5, 44, 1); reject_unchanged(&s, data, VEHICLE_INVALID);
    request(data, &s, 5, 43, 0); reject_unchanged(&s, data, VEHICLE_STALE);
    request(data, &s, 5, 44 + 0x80000000U, 0);
    reject_unchanged(&s, data, VEHICLE_STALE);
    request(data, &s, 5, 45, 0);
    assert(vehicle_request(&s, data, reply) == 1 && s.last == 44);
    puts("CAN policy exact/altered duplicates, failed-action cache and stale IDs: PASS");
}
static void wrap(void)
{
    struct vehicle_session s = fresh();
    uint8_t data[24], reply[24];
    request(data, &s, 1, UINT32_MAX, 0);
    assert(vehicle_request(&s, data, reply) == 1);
    vehicle_complete(&s, data, reply);
    request(data, &s, 1, 0, 0); reject_unchanged(&s, data, VEHICLE_STALE);
    request(data, &s, 1, 1, 0);
    assert(vehicle_request(&s, data, reply) == 1);
    vehicle_complete(&s, data, reply);
    request(data, &s, 1, UINT32_MAX, 0);
    reject_unchanged(&s, data, VEHICLE_STALE);
    request(data, &s, 1, 0x80000001U, 0);
    reject_unchanged(&s, data, VEHICLE_STALE);
    puts("CAN policy sequence wrap and half-range ambiguity: PASS");
}
int main(void)
{
    malformed(); session(); operations(); replay(); wrap();
    puts("CAN policy: 5 groups PASS");
    return 0;
}

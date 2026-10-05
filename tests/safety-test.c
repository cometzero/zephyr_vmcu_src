/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native policy tests; hardware and guest traffic require separate evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "safety.h"

static struct vmcu_frame healthy(uint32_t epoch, uint32_t sequence)
{
    struct vmcu_frame f = {
        .type = VMCU_SAFETY_HEALTH, .sender = VMCU_SI0,
        .flags = VMCU_SAFETY_READY, .epoch = epoch, .sequence = sequence,
        .progress = 0x000f000fu, /* monitored low16, online high16 */
    };
    return f;
}

static void state_is(struct vmcu_safety *s, uint32_t now,
                     enum vmcu_safety_state state,
                     enum vmcu_safety_fault reason)
{
    vmcu_safety_tick(s, now);
    assert(s->state == state && s->reason == reason);
}

static void reject_unchanged(struct vmcu_safety *s, struct vmcu_frame f,
                             uint32_t now)
{
    struct vmcu_safety previous = *s;

    assert(!vmcu_safety_receive(s, &f, now));
    assert(memcmp(s, &previous, sizeof(previous)) == 0);
}

static void test_authoritative_source(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(1, 1);

    vmcu_safety_gpio(&s, false, 0);
    vmcu_safety_gpio(&s, true, 100);
    state_is(&s, 100, SAFETY_WAIT, SAFETY_NONE);
    for (unsigned sender = VMCU_AP; sender <= VMCU_MCU; sender++) {
        for (unsigned type = VMCU_HELLO; type <= VMCU_SAFETY_HEALTH; type++) {
            f.sender = sender;
            f.type = type;
            reject_unchanged(&s, f, 200);
        }
    }
    f = healthy(1, 1);
    for (unsigned type = VMCU_HELLO; type < VMCU_SAFETY_HEALTH; type++) {
        f.type = type;
        reject_unchanged(&s, f, 200);
    }
    f = healthy(1, 1);
    f.flags = 8;
    reject_unchanged(&s, f, 200);
    f = healthy(0, 1);
    reject_unchanged(&s, f, 200);
    f = healthy(1, 0);
    reject_unchanged(&s, f, 200);
    f = healthy(1, 1);
    assert(vmcu_safety_receive(&s, &f, 200));
    state_is(&s, 200, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, false, 201);
    state_is(&s, 201, SAFETY_RUN, SAFETY_NONE);
    puts("safety: authoritative SI0 sender/type and reserved fields PASS");
}

static void test_mask_states(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(1, 1);

    vmcu_safety_gpio(&s, false, 0);
    vmcu_safety_gpio(&s, true, 10);
    f.flags = 0;
    assert(vmcu_safety_receive(&s, &f, 20));
    state_is(&s, 20, SAFETY_BOOT, SAFETY_NONE);
    f.sequence++;
    f.progress = 0;
    assert(vmcu_safety_receive(&s, &f, 30));
    state_is(&s, 30, SAFETY_BOOT, SAFETY_NONE);
    f.sequence++;
    f.progress = 0x00010005u;
    f.argument = 0x000a0000u;
    assert(vmcu_safety_receive(&s, &f, 40));
    assert(s.monitored == 5 && s.online == 1 && s.off == 0xa && !s.fault);
    state_is(&s, 40, SAFETY_BOOT, SAFETY_NONE);
    f.sequence++;
    f.progress = 0x000f000fu;
    f.argument = 0;
    f.flags = VMCU_SAFETY_READY;
    assert(vmcu_safety_receive(&s, &f, 50));
    state_is(&s, 50, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, false, 51);
    state_is(&s, 51, SAFETY_RUN, SAFETY_NONE);
    f.sequence++;
    f.progress = 0;
    f.argument = 0x000f0000u;
    f.flags = VMCU_SAFETY_ALL_OFF;
    assert(vmcu_safety_receive(&s, &f, 60));
    state_is(&s, 2000, SAFETY_OFF, SAFETY_NONE);
    f = healthy(1, f.sequence + 1);
    assert(vmcu_safety_receive(&s, &f, 2001));
    state_is(&s, 2001, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, true, 2002);
    state_is(&s, 2002, SAFETY_RUN, SAFETY_NONE);
    puts("safety: packed PFDI masks and BOOT/RUN/OFF states PASS");
}

static void test_mask_consistency(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(1, 1);

    f.progress = 0x001f000fu; /* Online includes an inactive core. */
    reject_unchanged(&s, f, 1);
    f = healthy(1, 1);
    f.argument = 0x00010000u; /* Active and powered-off overlap. */
    reject_unchanged(&s, f, 1);
    f = healthy(1, 1);
    f.progress = 0; /* Empty READY report. */
    reject_unchanged(&s, f, 1);
    f.progress = 0x0007000fu; /* Incomplete READY report. */
    reject_unchanged(&s, f, 1);
    f = healthy(1, 1);
    f.argument = 1; /* Fault mask requires the FAULT summary. */
    reject_unchanged(&s, f, 1);
    f.flags = VMCU_SAFETY_READY | VMCU_SAFETY_FAULT;
    reject_unchanged(&s, f, 1);
    f.flags = VMCU_SAFETY_FAULT;
    f.argument = 0; /* FAULT summary requires a fault mask. */
    reject_unchanged(&s, f, 1);
    f.flags = VMCU_SAFETY_ALL_OFF;
    f.argument = 0x00100000u; /* ALL_OFF cannot retain active cores. */
    reject_unchanged(&s, f, 1);
    f.progress = 0;
    f.argument = 0; /* ALL_OFF must identify powered-off cores. */
    reject_unchanged(&s, f, 1);
    f.argument = 0x000f0000u;
    f.flags = VMCU_SAFETY_ALL_OFF | VMCU_SAFETY_READY;
    reject_unchanged(&s, f, 1);

    /* SI0 may retain a fault for a core which was subsequently powered off. */
    f.flags = VMCU_SAFETY_FAULT;
    f.progress = 0x00010001u;
    f.argument = 0x000e0008u;
    assert(vmcu_safety_receive(&s, &f, 2));
    assert(s.monitored == 1 && s.online == 1 && s.fault == 8 && s.off == 14);
    state_is(&s, 2, SAFETY_DEGRADED, SAFETY_PFDI_FAULT);
    puts("safety: contradictory masks rejected and powered-off faults retained PASS");
}

static void test_fault_priority(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(1, 1);

    /* Reject inconsistent fault flags; authoritative valid faults degrade. */
    f.progress = 0x000b000fu;
    f.argument = 4;
    reject_unchanged(&s, f, 9);
    f.flags = VMCU_SAFETY_FAULT;
    assert(vmcu_safety_receive(&s, &f, 10));
    state_is(&s, 10, SAFETY_DEGRADED, SAFETY_PFDI_FAULT);
    f.sequence++;
    f.progress = 0x000f000fu;
    f.argument = 1;
    assert(vmcu_safety_receive(&s, &f, 20));
    state_is(&s, 20, SAFETY_DEGRADED, SAFETY_PFDI_FAULT);
    /* GPIO transitions cannot clear SI0's authoritative fault. */
    vmcu_safety_gpio(&s, false, 20);
    vmcu_safety_gpio(&s, true, 30);
    state_is(&s, 30, SAFETY_DEGRADED, SAFETY_PFDI_FAULT);
    state_is(&s, 15020, SAFETY_DEGRADED, SAFETY_LINK_TIMEOUT);
    f.sequence++;
    f.flags = VMCU_SAFETY_READY;
    f.argument = 0;
    assert(vmcu_safety_receive(&s, &f, 15021));
    state_is(&s, 15021, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, false, 15022);
    state_is(&s, 15022, SAFETY_RUN, SAFETY_NONE);
    puts("safety: fault precedence and independent recovery PASS");
}

static void test_report_and_timeout(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(4, 1);

    assert(VMCU_SAFETY_PERIOD_MS == 5000u);
    assert(VMCU_SAFETY_DEADLINE_MS == 15000u);
    assert(vmcu_safety_receive(&s, &f, 100));
    state_is(&s, 100, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, false, 100);
    vmcu_safety_gpio(&s, true, 101);
    for (unsigned report = 1; report <= 3; report++) {
        uint32_t now = 100 + 5000 * report;

        f.sequence++;
        assert(vmcu_safety_receive(&s, &f, now));
        assert(s.interval_ms == 5000);
        vmcu_safety_gpio(&s, !(report & 1), now);
        state_is(&s, now, SAFETY_RUN, SAFETY_NONE);
    }
    /* Real pin transitions keep GPIO healthy, but cannot refresh the UART. */
    vmcu_safety_gpio(&s, true, 30098);
    state_is(&s, 30099, SAFETY_RUN, SAFETY_NONE);
    state_is(&s, 30100, SAFETY_DEGRADED, SAFETY_LINK_TIMEOUT);
    puts("safety: 5000 ms reporting versus 15000 ms link deadline PASS");
}

static void test_gpio_edge_watchdog(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(7, 1);

    assert(VMCU_SOC_ERROR_DEADLINE_MS == 1500u);
    assert(vmcu_safety_receive(&s, &f, 0));
    state_is(&s, 0, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, true, 10);
    assert(s.gpio_edges == 0 && s.gpio_edge_ms == 10);
    state_is(&s, 10, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, true, 20);
    assert(s.gpio_edges == 0 && s.gpio_edge_ms == 10);
    vmcu_safety_gpio(&s, false, 100);
    state_is(&s, 1599, SAFETY_RUN, SAFETY_NONE);
    /* Fresh SI0 reports and repeated pin samples cannot synthesize an edge. */
    f.sequence++;
    assert(vmcu_safety_receive(&s, &f, 1599));
    vmcu_safety_gpio(&s, false, 1599);
    assert(s.gpio_edges == 1 && s.gpio_edge_ms == 100);
    state_is(&s, 1600, SAFETY_DEGRADED, SAFETY_GPIO_STUCK);
    vmcu_safety_gpio(&s, true, 1601);
    assert(s.gpio_edges == 2 && s.gpio_edge_ms == 1601);
    state_is(&s, 1601, SAFETY_RUN, SAFETY_NONE);
    memset(&s, 0, sizeof(s));
    assert(vmcu_safety_receive(&s, &f, 2000));
    state_is(&s, 2000, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, true, 2010);
    state_is(&s, 3499, SAFETY_BOOT, SAFETY_NONE);
    state_is(&s, 3500, SAFETY_DEGRADED, SAFETY_GPIO_STUCK);
    vmcu_safety_gpio(&s, false, 3501);
    state_is(&s, 3501, SAFETY_RUN, SAFETY_NONE);
    puts("safety: actual SOC_ERROR edges and independent GPIO watchdog PASS");
}

static void test_replay_and_epoch(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(8, 10);

    assert(vmcu_safety_receive(&s, &f, 100));
    reject_unchanged(&s, f, 14999);
    f.sequence = 9;
    reject_unchanged(&s, f, 14999);
    f.sequence = 10 + 0x80000000u;
    reject_unchanged(&s, f, 14999);
    f = healthy(8, 100);
    f.sender = VMCU_AP;
    reject_unchanged(&s, f, 15099);
    assert(s.received_ms == 100);
    state_is(&s, 15100, SAFETY_DEGRADED, SAFETY_LINK_TIMEOUT);
    /* Epoch is an opaque boot nonce, not an ordered timer value. */
    f = healthy(1, 1);
    assert(vmcu_safety_receive(&s, &f, 15101));
    assert(s.epoch == 1 && s.retired_epoch == 8 && s.sequence == 1 &&
           s.interval_ms == 0 && !s.gpio_armed);
    f = healthy(8, 1000);
    reject_unchanged(&s, f, 16000);
    puts("safety: duplicate/half-range/retired-session/AP replay rejection PASS");
}

static void test_wrap(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_frame f = healthy(UINT32_MAX, UINT32_MAX);

    assert(vmcu_safety_receive(&s, &f, UINT32_MAX - 99));
    f.sequence = 0;
    reject_unchanged(&s, f, UINT32_MAX - 49);
    f.sequence = 1; /* Sender skips reserved sequence zero. */
    assert(vmcu_safety_receive(&s, &f, 4900));
    assert(s.interval_ms == 5000);
    f.epoch = 1; /* Sender skips reserved epoch zero. */
    assert(vmcu_safety_receive(&s, &f, 5000));
    assert(s.epoch == 1 && s.interval_ms == 0);
    f.epoch = UINT32_MAX;
    reject_unchanged(&s, f, 5001);

    memset(&s, 0, sizeof(s));
    f = healthy(1, 1);
    assert(vmcu_safety_receive(&s, &f, UINT32_MAX - 9999));
    state_is(&s, UINT32_MAX - 9999, SAFETY_BOOT, SAFETY_NONE);
    vmcu_safety_gpio(&s, false, UINT32_MAX - 499);
    vmcu_safety_gpio(&s, true, UINT32_MAX - 399);
    state_is(&s, 1099, SAFETY_RUN, SAFETY_NONE);
    state_is(&s, 1100, SAFETY_DEGRADED, SAFETY_GPIO_STUCK);
    vmcu_safety_gpio(&s, false, 4998);
    state_is(&s, 4999, SAFETY_RUN, SAFETY_NONE);
    state_is(&s, 5000, SAFETY_DEGRADED, SAFETY_LINK_TIMEOUT);
    puts("safety: serial number and millisecond wrap boundaries PASS");
}

static void test_wire_rejection(void)
{
    struct vmcu_safety s = { 0 };
    struct vmcu_stream stream = { 0 };
    struct vmcu_frame f = healthy(2, 5), decoded;
    uint8_t bytes[VMCU_FRAME_SIZE];
    unsigned decoded_count = 0;

    vmcu_encode(bytes, &f);
    bytes[20] ^= 1; /* Fault-mask corruption without a valid CRC. */
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        assert(!vmcu_feed(&stream, bytes[i], &decoded));
        assert(stream.used < VMCU_FRAME_SIZE);
    }
    assert(!s.epoch);
    vmcu_encode(bytes, &f);
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        if (vmcu_feed(&stream, bytes[i], &decoded)) {
            assert(i == sizeof(bytes) - 1);
            decoded_count++;
            assert(vmcu_safety_receive(&s, &decoded, 100));
        }
        assert(stream.used < VMCU_FRAME_SIZE);
    }
    assert(decoded_count == 1 && s.epoch == 2 && s.sequence == 5);
    f.sender = VMCU_AP;
    f.sequence++;
    vmcu_encode(bytes, &f);
    assert(vmcu_decode(bytes, &decoded));
    reject_unchanged(&s, decoded, 15099);
    state_is(&s, 15100, SAFETY_DEGRADED, SAFETY_LINK_TIMEOUT);
    puts("safety: damaged wire frame resync and CRC-valid AP rejection PASS");
}

int main(void)
{
    test_authoritative_source();
    test_mask_states();
    test_mask_consistency();
    test_fault_priority();
    test_report_and_timeout();
    test_gpio_edge_watchdog();
    test_replay_and_epoch();
    test_wrap();
    test_wire_rejection();
    puts("safety: 9 test groups PASS");
    return 0;
}

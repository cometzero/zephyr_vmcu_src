/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rpc.h"

static void test_codec(void)
{
    struct vmcu_frame original = {
        .type = VMCU_HELLO, .sender = VMCU_AP, .epoch = 0x12345678,
        .sequence = 0xfffffff0, .progress = 0xa55a1234, .argument = 0,
    };
    struct vmcu_frame decoded;
    struct vmcu_stream stream = { 0 };
    uint8_t bytes[VMCU_FRAME_SIZE], bad[VMCU_FRAME_SIZE];
    unsigned found = 0;

    assert(vmcu_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926);
    vmcu_encode(bytes, &original);
    assert(bytes[0] == 0xa5 && bytes[1] == 0x5a && bytes[4] == 28);
    assert(bytes[8] == 0x78 && bytes[11] == 0x12);
    assert(vmcu_decode(bytes, &decoded));
    assert(decoded.epoch == original.epoch &&
           decoded.sequence == original.sequence &&
           decoded.progress == original.progress);
    memcpy(bad, bytes, sizeof(bad));
    bad[15] ^= 1;
    assert(!vmcu_decode(bad, &decoded));
    /* Fragment boundaries are arbitrary; reject garbage and a broken frame. */
    assert(!vmcu_feed(&stream, 0xa5, &decoded));
    assert(!vmcu_feed(&stream, 0xa5, &decoded));
    for (unsigned i = 0; i < sizeof(bad); i++) {
        assert(!vmcu_feed(&stream, bad[i], &decoded));
        assert(stream.used < VMCU_FRAME_SIZE);
    }
    for (unsigned repeat = 0; repeat < 2; repeat++) {
        for (unsigned i = 0; i < sizeof(bytes); i++) {
            if (vmcu_feed(&stream, bytes[i], &decoded)) {
                found++;
                assert(i == sizeof(bytes) - 1);
            }
            assert(stream.used < VMCU_FRAME_SIZE);
        }
    }
    assert(found == 2);
    /* Exercise fixed-capacity resynchronization across a long noise stream. */
    for (unsigned i = 0; i < 10000; i++) {
        vmcu_feed(&stream, (uint8_t)(i * 71), &decoded);
        assert(stream.used < VMCU_FRAME_SIZE);
    }
    found = 0;
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        found += vmcu_feed(&stream, bytes[i], &decoded);
    }
    assert(found == 1);
    assert(vmcu_newer(0, UINT32_MAX));
    assert(!vmcu_newer(0x80000000, 0));
    assert(!vmcu_newer(0, 0));
    assert(!vmcu_newer(UINT32_MAX, 0));
}

static void test_rpc(void)
{
    struct vmcu_rpc_peer peer;
    struct vmcu_frame command = {
        .type = VMCU_COMMAND, .sender = VMCU_MCU, .epoch = 42,
        .sequence = 1, .progress = VMCU_OP_PING, .argument = 0
    }, reply;
    uint8_t bytes[VMCU_FRAME_SIZE];

    vmcu_rpc_init(&peer, 42);
    vmcu_encode(bytes, &command);
    assert(vmcu_decode(bytes, &reply) && reply.type == VMCU_COMMAND);
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_OK && reply.argument == 0);
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_OK); /* Exact duplicate. */
    command.argument = 1;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_INVALID);
    command.sequence = 0;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_STALE);
    command.sequence = 2;
    command.epoch = 41;
    assert(!vmcu_rpc_receive(&peer, &command, &reply));
    command.epoch = 42;
    command.sender = VMCU_SI0;
    assert(!vmcu_rpc_receive(&peer, &command, &reply));
    command.sender = VMCU_MCU;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_INVALID); /* Ping requires argument zero. */
    command.argument = 0;
    command.sequence++;
    command.progress = VMCU_OP_STATUS;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_OK && reply.argument == 0);
    const unsigned unsupported[] = { VMCU_OP_HEARTBEAT, VMCU_OP_WORKLOAD,
        VMCU_OP_RESET, VMCU_OP_GPIO, 99 };
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        command.sequence++;
        command.progress = unsupported[i];
        assert(vmcu_rpc_receive(&peer, &command, &reply));
        assert(reply.flags == VMCU_UNSUPPORTED && reply.argument == 0);
    }
    command.sequence++;
    command.progress = VMCU_OP_PING;
    command.flags = 1;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_INVALID);
    command.flags = 0;
    command.progress = VMCU_OP_SESSION;
    command.sequence = 0;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    uint32_t cursor = reply.argument;
    assert(reply.flags == VMCU_OK && cursor == peer.request.sequence);
    assert(peer.request.progress == VMCU_OP_PING); /* Query cannot reset cache. */
    command.progress = VMCU_OP_POWER_OFF;
    command.sequence = cursor + 1;
    command.argument = 1;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_INVALID && !peer.poweroff_requested);
    command.sequence++;
    command.argument = 0;
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_OK && peer.poweroff_requested);
    assert(vmcu_rpc_receive(&peer, &command, &reply));
    assert(reply.flags == VMCU_OK); /* Exact duplicate has no new side effect. */
    puts("AP management RPC bounds/replay/session/poweroff-authorization: PASS");
}

int main(void)
{
    test_codec();
    test_rpc();
    return 0;
}

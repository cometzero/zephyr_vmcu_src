/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rpc.h"

void vmcu_rpc_init(struct vmcu_rpc_peer *peer, uint32_t epoch)
{
    *peer = (struct vmcu_rpc_peer) {
        .epoch = epoch
    };
}

bool vmcu_rpc_receive(struct vmcu_rpc_peer *peer,
                      const struct vmcu_frame *request,
                      struct vmcu_frame *reply)
{
    if (request->type != VMCU_COMMAND || request->sender != VMCU_MCU ||
        request->epoch != peer->epoch || !request->epoch) {
        return false;
    }
    *reply = (struct vmcu_frame) {
        .type = VMCU_REPLY, .sender = VMCU_AP, .epoch = peer->epoch,
        .sequence = request->sequence, .progress = request->progress
    };
    if (request->progress == VMCU_OP_SESSION && !request->sequence &&
        !request->flags && !request->argument) {
        reply->argument = peer->have_transaction ? peer->request.sequence : 0;
        return true;
    }
    if (peer->have_transaction &&
        request->sequence == peer->request.sequence) {
        if (request->progress == peer->request.progress &&
            request->argument == peer->request.argument &&
            request->flags == peer->request.flags) {
            *reply = peer->reply;
        } else {
            reply->flags = VMCU_INVALID;
        }
        return true;
    }
    if (!request->sequence || (peer->have_transaction &&
        !vmcu_newer(request->sequence, peer->request.sequence))) {
        reply->flags = VMCU_STALE;
        return true;
    }
    if (request->flags) {
        reply->flags = VMCU_INVALID;
    } else {
        switch (request->progress) {
        case VMCU_OP_PING:
        case VMCU_OP_STATUS:
            reply->flags = request->argument ? VMCU_INVALID : VMCU_OK;
            break;
        case VMCU_OP_POWER_OFF:
            reply->flags = request->argument ? VMCU_INVALID : VMCU_OK;
            if (reply->flags == VMCU_OK)
                peer->poweroff_requested = true;
            break;
        default:
            reply->flags = VMCU_UNSUPPORTED;
        }
    }
    peer->request = *request;
    peer->reply = *reply;
    peer->have_transaction = true;
    return true;
}

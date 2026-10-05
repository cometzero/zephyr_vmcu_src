/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMCU_RPC_H
#define VMCU_RPC_H

#include "protocol.h"

/* COMMAND/REPLY: sequence=transaction, progress=operation, argument=value.
 * REPLY flags=result. AP management supports ping/status, session discovery and poweroff.
 */
enum vmcu_operation {
    VMCU_OP_PING = 1, VMCU_OP_STATUS, VMCU_OP_HEARTBEAT,
    VMCU_OP_WORKLOAD, VMCU_OP_RESET, VMCU_OP_GPIO,
    VMCU_OP_SESSION, VMCU_OP_PMIC_RAIL, VMCU_OP_PMIC_FAULT,
    VMCU_OP_RECOVER, VMCU_OP_RECOVERY_STATUS, VMCU_OP_POWER_OFF,
    VMCU_OP_POWER_STATUS
};
enum vmcu_result {
    VMCU_OK, VMCU_UNSUPPORTED, VMCU_INVALID, VMCU_STALE
};

struct vmcu_rpc_peer {
    uint32_t epoch;
    bool have_transaction, poweroff_requested;
    struct vmcu_frame request, reply;
};

void vmcu_rpc_init(struct vmcu_rpc_peer *peer, uint32_t epoch);
/* False means an invalid sender/session or non-command: no response. */
bool vmcu_rpc_receive(struct vmcu_rpc_peer *peer,
                      const struct vmcu_frame *request,
                      struct vmcu_frame *reply);
#endif

/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMCU_CONTROL_H
#define VMCU_CONTROL_H
#include "rpc.h"
#include "safety.h"
int vmcu_control(uint8_t peer, uint32_t operation, uint32_t value, struct vmcu_frame *response);
int vmcu_control_at(uint8_t peer, uint32_t operation, uint32_t value, struct vmcu_frame *response, uint32_t guard);
void vmcu_snapshot(struct vmcu_safety *snapshot, uint32_t *gpio);
int vmcu_power_off(struct vmcu_frame *response, uint32_t guard);
int vmcu_power_on(struct vmcu_frame *response, uint32_t guard);
#endif

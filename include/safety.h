/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMCU_SAFETY_H
#define VMCU_SAFETY_H

#include "protocol.h"

/* SI CL0 exports the existing AP PFDI monitor, not a synthetic workload. */
#define VMCU_SAFETY_PERIOD_MS 5000u
#define VMCU_SAFETY_DEADLINE_MS 15000u
#define VMCU_SOC_ERROR_DEADLINE_MS 1500u
#define VMCU_SAFETY_READY 1u
#define VMCU_SAFETY_FAULT 2u
#define VMCU_SAFETY_ALL_OFF 4u

enum vmcu_safety_state { SAFETY_WAIT, SAFETY_BOOT, SAFETY_RUN, SAFETY_OFF, SAFETY_DEGRADED };
enum vmcu_safety_fault { SAFETY_NONE, SAFETY_LINK_TIMEOUT, SAFETY_PFDI_FAULT, SAFETY_GPIO_STUCK };
struct vmcu_safety {
    uint32_t epoch, retired_epoch, sequence, received_ms, interval_ms;
    uint32_t gpio_edge_ms, gpio_edges, ready_since_ms, ready_edges;
    uint16_t monitored, online, fault, off;
    uint8_t flags;
    bool gpio_level, gpio_initialized, gpio_armed;
    enum vmcu_safety_state state;
    enum vmcu_safety_fault reason;
};

bool vmcu_safety_receive(struct vmcu_safety *state,
                         const struct vmcu_frame *frame, uint32_t now);
void vmcu_safety_gpio(struct vmcu_safety *state, bool level, uint32_t now);
void vmcu_safety_tick(struct vmcu_safety *state, uint32_t now);
#endif

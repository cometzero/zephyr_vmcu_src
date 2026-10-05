/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "safety.h"

bool vmcu_safety_receive(struct vmcu_safety *s,
                         const struct vmcu_frame *f, uint32_t now)
{
    if (f->type != VMCU_SAFETY_HEALTH || f->sender != VMCU_SI0 ||
        !f->epoch || !f->sequence || (f->flags & ~7u)) {
        return false;
    }
    uint16_t active = (uint16_t)f->progress;
    uint16_t online = (uint16_t)(f->progress >> 16);
    uint16_t faults = (uint16_t)f->argument;
    uint16_t off = (uint16_t)(f->argument >> 16);
    if ((online & ~active) || (active & off) ||
        (f->flags != 0 && f->flags != VMCU_SAFETY_READY &&
         f->flags != VMCU_SAFETY_FAULT && f->flags != VMCU_SAFETY_ALL_OFF) ||
        (!!faults != !!(f->flags & VMCU_SAFETY_FAULT)) ||
        ((f->flags & VMCU_SAFETY_READY) && (!active || online != active)) ||
        ((f->flags & VMCU_SAFETY_ALL_OFF) && (active || online || !off))) {
        return false;
    }
    if (s->epoch && (f->epoch == s->epoch ? !vmcu_newer(f->sequence, s->sequence)
                                         : f->epoch == s->retired_epoch)) {
        return false;
    }
    if (s->epoch != f->epoch || f->flags != VMCU_SAFETY_READY) {
        s->gpio_armed = false;
    }
    s->interval_ms = s->epoch == f->epoch ? now - s->received_ms : 0;
    if (s->epoch != f->epoch) {
        s->retired_epoch = s->epoch;
    }
    s->epoch = f->epoch;
    s->sequence = f->sequence;
    s->received_ms = now;
    s->monitored = (uint16_t)f->progress;
    s->online = (uint16_t)(f->progress >> 16);
    s->fault = (uint16_t)f->argument;
    s->off = (uint16_t)(f->argument >> 16);
    s->flags = f->flags;
    return true;
}

void vmcu_safety_gpio(struct vmcu_safety *s, bool level, uint32_t now)
{
    if (!s->gpio_initialized || level != s->gpio_level) {
        if (s->gpio_initialized) {
            s->gpio_edges++;
        }
        s->gpio_initialized = true;
        s->gpio_edge_ms = now;
        s->gpio_level = level;
    }
}

void vmcu_safety_tick(struct vmcu_safety *s, uint32_t now)
{
    s->reason = SAFETY_NONE;
    if (!s->epoch) {
        s->state = SAFETY_WAIT;
    } else if ((uint32_t)(now - s->received_ms) >= VMCU_SAFETY_DEADLINE_MS) {
        s->state = SAFETY_DEGRADED;
        s->reason = SAFETY_LINK_TIMEOUT;
    } else if (s->fault || (s->flags & VMCU_SAFETY_FAULT)) {
        s->state = SAFETY_DEGRADED;
        s->reason = SAFETY_PFDI_FAULT;
    } else if (s->flags & VMCU_SAFETY_ALL_OFF) {
        s->state = SAFETY_OFF;
    } else if (!(s->flags & VMCU_SAFETY_READY) || !s->monitored ||
               (s->online & s->monitored) != s->monitored) {
        s->state = SAFETY_BOOT;
    } else {
        if (!s->gpio_armed) {
            s->gpio_armed = true;
            s->ready_since_ms = now;
            s->ready_edges = s->gpio_edges;
        }
        bool edge_seen = s->gpio_edges != s->ready_edges;
        uint32_t last = edge_seen ? s->gpio_edge_ms : s->ready_since_ms;
        if ((uint32_t)(now - last) >= VMCU_SOC_ERROR_DEADLINE_MS) {
            s->state = SAFETY_DEGRADED;
            s->reason = SAFETY_GPIO_STUCK;
        } else {
            s->state = edge_seen ? SAFETY_RUN : SAFETY_BOOT;
        }
    }
}

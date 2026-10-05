/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/sys_io.h>
#include "rpc.h"
#include "safety.h"
#include "control.h"
#include "vehicle_can.h"

/* Three independent paths: AP management, console, SI CL0 safety. */
static const struct device *const ap_link = DEVICE_DT_GET(DT_ALIAS(vmcu_link));
static const struct device *const safety_link = DEVICE_DT_GET(DT_ALIAS(vmcu_safety));
#define PORT0 DT_REG_ADDR(DT_ALIAS(vmcu_port))
#define PORT_OUT 0x00u
#define PORT_OMR 0x04u
#define PORT_IOCR0 0x10u
#define PORT_IOCR4 0x14u
#define PORT_IN 0x24u
#define GPIO_RESET_N BIT(3)
#define GPIO_WAKE BIT(4)

static struct vmcu_safety safety;
static bool monitor = true, awaiting;
static uint32_t ap_epoch, tx_sequence;
static struct vmcu_frame pending, reply;
static uint8_t pending_peer;
struct request { struct vmcu_frame frame; uint8_t peer; uint32_t guard; };
K_MUTEX_DEFINE(state_lock);
K_MUTEX_DEFINE(command_lock);
K_SEM_DEFINE(reply_ready, 0, 1);
K_MSGQ_DEFINE(commands, sizeof(struct request), 1, 4);

static const char *state_name(unsigned state)
{
    static const char *const names[] = { "WAIT", "BOOT", "RUN", "OFF", "DEGRADED" };
    return state < ARRAY_SIZE(names) ? names[state] : "UNKNOWN";
}

static const char *fault_name(unsigned reason)
{
    static const char *const names[] = { "NONE", "LINK_TIMEOUT", "PFDI_FAULT", "SOC_ERROR_STUCK" };
    return reason < ARRAY_SIZE(names) ? names[reason] : "UNKNOWN";
}

static const char *peer_name(uint8_t peer)
{
    return peer == VMCU_SI0 ? "SI0" : "AP";
}

static void transmit(const struct device *uart, const struct vmcu_frame *frame)
{
    uint8_t bytes[VMCU_FRAME_SIZE];
    vmcu_encode(bytes, frame);
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        uart_poll_out(uart, bytes[i]);
    }
}

static void receive(const struct vmcu_frame *frame, uint8_t peer, uint32_t now)
{
    uint32_t epoch = peer == VMCU_SI0 ? safety.epoch : ap_epoch;
    if (frame->sender != peer) {
        return;
    }
    if (frame->type == VMCU_REPLY) {
        if (awaiting && peer == pending_peer && frame->epoch == epoch &&
            frame->epoch == pending.epoch && frame->sequence == pending.sequence &&
            frame->progress == pending.progress && frame->flags <= VMCU_STALE) {
            reply = *frame;
            awaiting = false;
            k_sem_give(&reply_ready);
        }
    } else if (peer == VMCU_SI0) {
        (void)vmcu_safety_receive(&safety, frame, now);
    } else if (frame->type == VMCU_HELLO && frame->epoch &&
               !frame->flags && !frame->progress && !frame->argument &&
               (!ap_epoch || frame->epoch == ap_epoch || vmcu_newer(frame->epoch, ap_epoch))) {
        ap_epoch = frame->epoch;
        struct vmcu_frame ack = {
            .type = VMCU_HELLO_ACK, .sender = VMCU_MCU,
            .epoch = ap_epoch, .sequence = ++tx_sequence
        };
        transmit(ap_link, &ack);
    }
    /* AP HEALTH is deliberately not an input to the safety decision. */
}

static void link_worker(void *unused1, void *unused2, void *unused3)
{
    struct vmcu_stream streams[2] = { 0 };
    const struct device *const uart[] = { ap_link, safety_link };
    const uint8_t peer[] = { VMCU_AP, VMCU_SI0 };
    unsigned previous_state = SAFETY_WAIT, previous_fault = SAFETY_NONE;
    ARG_UNUSED(unused1); ARG_UNUSED(unused2); ARG_UNUSED(unused3);
    if (!device_is_ready(ap_link) || !device_is_ready(safety_link)) {
        printk("VMCU_INIT result=FAIL reason=uart-not-ready\n");
        return;
    }
    /* Establish the inactive reset latch BEFORE enabling its output driver.
     * IN[2] pulls up; IN[0:1] float. IOCR byte 0x80 selects push-pull GPIO.
     */
    sys_write32(GPIO_RESET_N, PORT0 + PORT_OUT);
    sys_write32(0x80100000u, PORT0 + PORT_IOCR0);
    sys_write32(0x00000080u, PORT0 + PORT_IOCR4);
    printk("VMCU_INIT result=PASS shell=ASCLIN1 management=ASCLIN0 safety=ASCLIN2 period_ms=5000\n");
    printk("VMCU_STATE source=SI0_PFDI state=WAIT fault=NONE epoch=0\n");
    for (;;) {
        uint32_t now = k_uptime_get_32();
        struct request request;
        struct vmcu_safety changed;
        bool announce = false;
        k_mutex_lock(&state_lock, K_FOREVER);
        for (unsigned channel = 0; channel < ARRAY_SIZE(uart); channel++) {
            unsigned char byte;
            struct vmcu_frame frame;
            for (unsigned i = 0; i < 128 && uart_poll_in(uart[channel], &byte) == 0; i++) {
                if (vmcu_feed(&streams[channel], byte, &frame)) {
                    receive(&frame, peer[channel], now);
                }
            }
        }
        vmcu_safety_gpio(&safety, (sys_read32(PORT0 + PORT_IN) & BIT(0)) != 0, now);
        vmcu_safety_tick(&safety, now);
        if (safety.state != previous_state || safety.reason != previous_fault) {
            announce = monitor;
            changed = safety;
            previous_state = safety.state;
            previous_fault = safety.reason;
        }
        if (k_msgq_get(&commands, &request, K_NO_WAIT) == 0 && awaiting &&
            request.peer == pending_peer &&
            (!request.guard || (request.guard == safety.epoch &&
             now - safety.received_ms < VMCU_SAFETY_DEADLINE_MS)) && request.frame.epoch ==
            (request.peer == VMCU_SI0 ? safety.epoch : ap_epoch)) {
            transmit(request.peer == VMCU_SI0 ? safety_link : ap_link, &request.frame);
        }
        k_mutex_unlock(&state_lock);
        if (announce) {
            shell_print(shell_backend_uart_get_ptr(),
                        "VMCU_STATE source=SI0_PFDI state=%s fault=%s epoch=%u pfdi_fault=0x%04x",
                        state_name(changed.state), fault_name(changed.reason), changed.epoch,
                        changed.fault);
        }
        k_sleep(K_MSEC(1));
    }
}
K_THREAD_DEFINE(link_thread, 4096, link_worker, NULL, NULL, NULL, 5, 0, 0);

/* command_lock serializes shell and vehicle requests; link_worker owns UART I/O. */
static int exchange(uint8_t peer, uint32_t epoch, uint32_t sequence,
                    uint32_t operation, uint32_t value, struct vmcu_frame *response, uint32_t guard)
{
    struct request request;
    k_sem_reset(&reply_ready);
    k_mutex_lock(&state_lock, K_FOREVER);
    if (epoch != (peer == VMCU_SI0 ? safety.epoch : ap_epoch) ||
        (guard && (guard != safety.epoch ||
                   k_uptime_get_32() - safety.received_ms >= VMCU_SAFETY_DEADLINE_MS))) {
        k_mutex_unlock(&state_lock);
        return -EAGAIN;
    }
    pending = (struct vmcu_frame) {
        .type = VMCU_COMMAND, .sender = VMCU_MCU, .epoch = epoch,
        .sequence = sequence, .progress = operation, .argument = value
    };
    pending_peer = peer;
    awaiting = true;
    request = (struct request) { .frame = pending, .peer = peer, .guard = guard };
    int rc = k_msgq_put(&commands, &request, K_NO_WAIT);
    k_mutex_unlock(&state_lock);
    if (!rc) { rc = k_sem_take(&reply_ready, K_MSEC(2000)); }
    k_mutex_lock(&state_lock, K_FOREVER);
    awaiting = false;
    if (!rc) { *response = reply; }
    k_msgq_purge(&commands);
    k_mutex_unlock(&state_lock);
    if (rc) { return -ETIMEDOUT; }
    static const int errors[] = { 0, -ENOTSUP, -EINVAL, -EAGAIN };
    return errors[response->flags];
}

int vmcu_control_at(uint8_t peer, uint32_t operation, uint32_t value,
                    struct vmcu_frame *response, uint32_t guard)
{
    struct vmcu_frame cursor;
    k_mutex_lock(&command_lock, K_FOREVER);
    k_mutex_lock(&state_lock, K_FOREVER);
    uint32_t epoch = peer == VMCU_SI0 ? safety.epoch : ap_epoch;
    k_mutex_unlock(&state_lock);
    int rc = epoch ? exchange(peer, epoch, 0, VMCU_OP_SESSION, 0, &cursor, guard) : -ENOTCONN;
    if (!rc) {
        uint32_t next = cursor.argument + 1;
        if (!next) { next = 1; }
        rc = exchange(peer, epoch, next, operation, value, response, guard);
    }
    k_mutex_unlock(&command_lock);
    return rc;
}

int vmcu_control(uint8_t peer, uint32_t operation, uint32_t value, struct vmcu_frame *response)
{
    return vmcu_control_at(peer, operation, value, response, 0);
}

void vmcu_snapshot(struct vmcu_safety *snapshot, uint32_t *gpio)
{
    k_mutex_lock(&state_lock, K_FOREVER);
    *snapshot = safety;
    *gpio = sys_read32(PORT0 + PORT_IN);
    k_mutex_unlock(&state_lock);
}

int vmcu_power_off(struct vmcu_frame *response, uint32_t guard)
{
    int rc = vmcu_control_at(VMCU_SI0, VMCU_OP_POWER_OFF, 1, response, guard);
    if (rc) { return rc; }
    rc = vmcu_control_at(VMCU_AP, VMCU_OP_POWER_OFF, 0, response, guard);
    /* Timeout is ambiguous: a late AP shutdown must retain the SI guard. */
    if (rc == -ENOTSUP || rc == -EINVAL || rc == -ENOTCONN) {
        struct vmcu_frame cancel;
        (void)vmcu_control_at(VMCU_SI0, VMCU_OP_POWER_OFF, 0, &cancel, guard);
    }
    return rc;
}

int vmcu_power_on(struct vmcu_frame *response, uint32_t guard)
{
    int rc = vmcu_control_at(VMCU_SI0, VMCU_OP_POWER_STATUS, 0, response, guard);
    if (rc) { return rc; }
    if (response->argument != 3) { return -EALREADY; }
    sys_write32(GPIO_WAKE << 16, PORT0 + PORT_OMR);
    k_sleep(K_MSEC(50));
    k_mutex_lock(&state_lock, K_FOREVER);
    if (guard && (guard != safety.epoch ||
                  k_uptime_get_32() - safety.received_ms >= VMCU_SAFETY_DEADLINE_MS)) {
        k_mutex_unlock(&state_lock);
        return -EAGAIN;
    }
    sys_write32(GPIO_WAKE, PORT0 + PORT_OMR);
    k_mutex_unlock(&state_lock);
    k_sleep(K_MSEC(100));
    sys_write32(GPIO_WAKE << 16, PORT0 + PORT_OMR);
    return vmcu_control_at(VMCU_SI0, VMCU_OP_POWER_STATUS, 0, response, guard);
}

static int report_rpc(const struct shell *shell, uint8_t peer, unsigned operation,
                      int rc, const struct vmcu_frame *response)
{
    static const char *const results[] = { "OK", "UNSUPPORTED", "INVALID", "STALE" };
    if (rc) {
        shell_error(shell, "VMCU_RPC peer=%s operation=%u result=ERROR errno=%d",
                    peer_name(peer), operation, rc);
    } else {
        shell_print(shell, "VMCU_RPC peer=%s operation=%u transaction=%u result=%s value=0x%08x",
                    peer_name(peer), operation, response->sequence, results[response->flags],
                    response->argument);
    }
    return rc;
}

static int rpc(const struct shell *shell, uint8_t peer, uint32_t operation, uint32_t value)
{
    struct vmcu_frame response = { 0 };
    int rc = vmcu_control(peer, operation, value, &response);
    return report_rpc(shell, peer, operation, rc, &response);
}

static int command(const struct shell *shell, size_t argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "can")) {
        return vmcu_can_shell(shell, argc - 2, argv + 2);
    }
    if (argc == 3 && !strcmp(argv[1], "recover")) {
        if (!strcmp(argv[2], "start")) { return rpc(shell, VMCU_SI0, VMCU_OP_RECOVER, 0); }
        if (!strcmp(argv[2], "status")) { return rpc(shell, VMCU_SI0, VMCU_OP_RECOVERY_STATUS, 0); }
        return -EINVAL;
    }
    if (argc == 3 && !strcmp(argv[1], "power")) {
        struct vmcu_frame response = { 0 };
        if (!strcmp(argv[2], "status")) { return rpc(shell, VMCU_SI0, VMCU_OP_POWER_STATUS, 0); }
        int rc;
        if (!strcmp(argv[2], "off")) { rc = vmcu_power_off(&response, 0); }
        else if (!strcmp(argv[2], "on")) { rc = vmcu_power_on(&response, 0); }
        else { return -EINVAL; }
        return report_rpc(shell, VMCU_SI0, VMCU_OP_POWER_STATUS, rc, &response);
    }
    if (argc == 4 && !strcmp(argv[1], "pmic")) {
        int err = 0;
        unsigned long value = shell_strtoul(argv[3], 0, &err);
        if (err || value > 10) { return -EINVAL; }
        if (!strcmp(argv[2], "rail") && value < 9) { return rpc(shell, VMCU_SI0, VMCU_OP_PMIC_RAIL, value); }
        if (!strcmp(argv[2], "fault")) { return rpc(shell, VMCU_SI0, VMCU_OP_PMIC_FAULT, value); }
        return -EINVAL;
    }
    if (argc == 2 && !strcmp(argv[1], "status")) {
        struct vmcu_safety s;
        k_mutex_lock(&state_lock, K_FOREVER);
        s = safety;
        k_mutex_unlock(&state_lock);
        shell_print(shell, "VMCU_STATUS source=SI0_PFDI state=%s fault=%s epoch=%u sequence=%u period_ms=5000 age_ms=%u interval_ms=%u monitored=0x%04x online=0x%04x pfdi_fault=0x%04x off=0x%04x gpio_edges=%u",
                    state_name(s.state), fault_name(s.reason), s.epoch, s.sequence,
                    s.epoch ? k_uptime_get_32() - s.received_ms : 0, s.interval_ms,
                    s.monitored, s.online, s.fault, s.off, s.gpio_edges);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "monitor") &&
        (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        k_mutex_lock(&state_lock, K_FOREVER);
        monitor = !strcmp(argv[2], "on");
        k_mutex_unlock(&state_lock);
        shell_print(shell, "VMCU_MONITOR %s (state changes only)", argv[2]);
        return 0;
    }
    if (argc == 3 && (!strcmp(argv[1], "apollo") || !strcmp(argv[1], "safety"))) {
        uint8_t peer = !strcmp(argv[1], "safety") ? VMCU_SI0 : VMCU_AP;
        if (!strcmp(argv[2], "ping")) { return rpc(shell, peer, VMCU_OP_PING, 0); }
        if (!strcmp(argv[2], "status")) { return rpc(shell, peer, VMCU_OP_STATUS, 0); }
        if (peer == VMCU_SI0 && !strcmp(argv[2], "gpio")) { return rpc(shell, peer, VMCU_OP_GPIO, 0); }
    }
    if (argc == 3 && !strcmp(argv[1], "heartbeat") &&
        (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        return rpc(shell, VMCU_SI0, VMCU_OP_HEARTBEAT, !strcmp(argv[2], "on"));
    }
    if (argc >= 2 && !strcmp(argv[1], "workload")) {
        shell_error(shell, "VMCU_RPC result=UNSUPPORTED AP health is owned by the existing SI0 PFDI monitor");
        return -ENOTSUP;
    }
    if (argc == 3 && !strcmp(argv[1], "gpio") && !strcmp(argv[2], "status")) {
        shell_print(shell, "VMCU_GPIO in=0x%04x out=0x%04x SOC_ERROR=%u SOC_PWR_REQ=%u IST_DONE_N=%u SOC_RESET_N=%u MCU_SOC_WAKE=%u",
                    sys_read32(PORT0 + PORT_IN), sys_read32(PORT0 + PORT_OUT),
                    !!(sys_read32(PORT0 + PORT_IN) & BIT(0)), !!(sys_read32(PORT0 + PORT_IN) & BIT(1)),
                    !!(sys_read32(PORT0 + PORT_IN) & BIT(2)), !!(sys_read32(PORT0 + PORT_IN) & GPIO_RESET_N),
                    !!(sys_read32(PORT0 + PORT_IN) & GPIO_WAKE));
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "gpio")) {
        uint32_t mask, value;
        if (!strcmp(argv[2], "reset") &&
            (!strcmp(argv[3], "assert") || !strcmp(argv[3], "release"))) {
            mask = GPIO_RESET_N;
            value = !strcmp(argv[3], "release") ? mask : mask << 16;
        } else if (!strcmp(argv[2], "wake") &&
                   (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
            mask = GPIO_WAKE;
            value = !strcmp(argv[3], "on") ? mask : mask << 16;
        } else {
            return -EINVAL;
        }
        sys_write32(value, PORT0 + PORT_OMR);
        shell_print(shell, "VMCU_GPIO %s %s output=0x%04x", argv[2], argv[3], sys_read32(PORT0 + PORT_OUT));
        return 0;
    }
    shell_print(shell, "vmcu-cli status | monitor on|off | apollo ping|status | safety ping|status|gpio | heartbeat on|off | gpio status|reset assert|release|wake on|off | pmic rail|fault INDEX | recover start|status | power off|on|status | can status|restart|trace on|off|send ID FLAGS HEX");
    return -EINVAL;
}
/* Preserve standard iterable registration while allowing a hyphenated command. */
static const struct shell_static_entry vmcu_cli =
    SHELL_CMD_ARG(vmcu-cli, NULL, "SI CL0 PFDI safety monitor and GPIO controls", command, 1, 5);
static const TYPE_SECTION_ITERABLE(union shell_cmd_entry, shell_cmd_vmcu_cli,
                                  shell_root_cmds, shell_cmd_vmcu_cli) = { .entry = &vmcu_cli };

int main(void) { return 0; }

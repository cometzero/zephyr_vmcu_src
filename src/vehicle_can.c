/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <string.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/atomic.h>
#include "can_policy.h"
#include "control.h"
#include "vehicle_can.h"

static const struct device *const bus = DEVICE_DT_GET(DT_ALIAS(vmcu_can));
static struct vehicle_session session;
static atomic_t rx_count, tx_count, tx_errors, rx_drops, trace;
K_MSGQ_DEFINE(received, sizeof(struct can_frame), 16, 4);

static void rx(const struct device *dev, struct can_frame *frame, void *arg)
{
    ARG_UNUSED(dev); ARG_UNUSED(arg);
    atomic_inc(&rx_count);
    if (k_msgq_put(&received, frame, K_NO_WAIT)) { atomic_inc(&rx_drops); }
}
static void tx_done(const struct device *dev, int error, void *arg)
{
    ARG_UNUSED(dev); ARG_UNUSED(arg);
    atomic_inc(error ? &tx_errors : &tx_count);
}
static int send(struct can_frame *frame)
{
    int rc = can_send(bus, frame, K_MSEC(100), tx_done, NULL);
    if (rc) { atomic_inc(&tx_errors); }
    return rc;
}
static uint8_t result(int rc)
{
    if (!rc) { return VEHICLE_OK; }
    if (rc == -ENOTSUP) { return VEHICLE_UNSUPPORTED; }
    if (rc == -EINVAL) { return VEHICLE_INVALID; }
    if (rc == -EAGAIN) { return VEHICLE_STALE; }
    if (rc == -ENOTCONN || rc == -EALREADY) { return VEHICLE_UNAVAILABLE; }
    return VEHICLE_IO;
}
static void dispatch(const struct can_frame *frame, const struct vmcu_safety *s)
{
    /* Other traffic is diagnostic only. BRS is optional; IDE/RTR/ESI are not commands. */
    if (frame->id != VMCU_CAN_COMMAND || frame->dlc != 12 ||
        (frame->flags & ~CAN_FRAME_BRS) != CAN_FRAME_FDF) { return; }
    struct can_frame response = { .id = VMCU_CAN_REPLY, .dlc = 12,
                                 .flags = CAN_FRAME_FDF | CAN_FRAME_BRS };
    if (vehicle_request(&session, frame->data, response.data)) {
        struct vmcu_frame reply = { 0 };
        int rc = 0;
        uint8_t op = frame->data[1];
        uint32_t arg = vehicle_get32(frame->data + 16);
        /* A stale safety channel must not authorize an actuator request. */
        if (op >= 5 && (!s->epoch || s->reason == SAFETY_LINK_TIMEOUT)) {
            rc = -ENOTCONN;
        } else {
            switch (op) {
            case 1: reply.argument = s->state; break;
            case 2: rc = vmcu_control_at(VMCU_AP, VMCU_OP_PING, 0, &reply, session.epoch); break;
            case 3: rc = vmcu_control_at(VMCU_SI0, VMCU_OP_PMIC_RAIL, arg, &reply, session.epoch); break;
            case 4: rc = vmcu_control_at(VMCU_SI0, VMCU_OP_PMIC_FAULT, arg, &reply, session.epoch); break;
            case 5: rc = vmcu_control_at(VMCU_SI0, VMCU_OP_RECOVER, 0, &reply, session.epoch); break;
            case 6: rc = vmcu_power_off(&reply, session.epoch); break;
            case 7: rc = vmcu_power_on(&reply, session.epoch); break;
            }
        }
        response.data[2] = result(rc);
        response.data[3] = s->state;
        vehicle_put32(response.data + 16, reply.argument);
        vehicle_complete(&session, frame->data, response.data);
        shell_print(shell_backend_uart_get_ptr(), "VMCU_CAN_COMMAND op=%u seq=%u result=%u value=0x%08x", op,
               session.last, response.data[2], reply.argument);
    }
    (void)send(&response);
}
static void telemetry(const struct vmcu_safety *s, uint32_t gpio)
{
    struct can_frame frame = { .id = VMCU_CAN_TELEMETRY, .dlc = 13,
                              .flags = CAN_FRAME_FDF | CAN_FRAME_BRS };
    frame.data[0] = 1; frame.data[1] = s->state;
    frame.data[2] = s->reason; frame.data[3] = gpio;
    vehicle_put32(frame.data + 4, s->epoch);
    vehicle_put32(frame.data + 8, session.cookie);
    vehicle_put32(frame.data + 12, s->sequence);
    vehicle_put32(frame.data + 16, s->monitored | (uint32_t)s->online << 16);
    vehicle_put32(frame.data + 20, s->fault | (uint32_t)s->off << 16);
    vehicle_put32(frame.data + 24, session.last);
    vehicle_put32(frame.data + 28, session.cookie ? 1 : 0);
    (void)send(&frame);
}
static void worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    if (!device_is_ready(bus)) { printk("VMCU_CAN_INIT result=FAIL reason=device\n"); return; }
    const struct can_filter standard = { .id = 0, .mask = 0 };
    const struct can_filter extended = { .id = 0, .mask = 0, .flags = CAN_FILTER_IDE };
    int rc = can_set_mode(bus, CAN_MODE_FD);
    int std_filter = can_add_rx_filter(bus, rx, NULL, &standard);
    int ext_filter = can_add_rx_filter(bus, rx, NULL, &extended);
    if (!rc && std_filter >= 0 && ext_filter >= 0) { rc = can_start(bus); }
    else if (!rc) { rc = -ENOSPC; }
    printk("VMCU_CAN_INIT result=%s errno=%d nominal=500000 data=2000000\n", rc ? "FAIL" : "PASS", rc);
    if (rc) { return; }
    uint32_t last_report = 0, last_session_try = 0;
    unsigned last_state = UINT32_MAX, last_fault = UINT32_MAX;
    for (;;) {
        struct vmcu_safety s;
        uint32_t gpio, now = k_uptime_get_32();
        vmcu_snapshot(&s, &gpio);
        if (session.epoch != s.epoch) { session = (struct vehicle_session) { .epoch = s.epoch }; }
        if (!session.cookie && s.epoch && now - last_session_try >= 1000) {
            struct vmcu_frame reply;
            last_session_try = now;
            if (!vmcu_control(VMCU_SI0, VMCU_OP_PING, 0, &reply) && reply.epoch == s.epoch) {
                session.cookie = reply.sequence;
                shell_print(shell_backend_uart_get_ptr(), "VMCU_CAN_SESSION epoch=%u cookie=%u", session.epoch, session.cookie);
            }
        }
        if (s.epoch && (now - last_report >= 5000 || s.state != last_state || s.reason != last_fault)) {
            telemetry(&s, gpio);
            last_report = now; last_state = s.state; last_fault = s.reason;
        }
        struct can_frame frame;
        if (!k_msgq_get(&received, &frame, K_MSEC(20))) {
            if (atomic_get(&trace)) {
                char hex[129];
                static const char digits[] = "0123456789abcdef";
                unsigned len = can_dlc_to_bytes(frame.dlc);
                for (unsigned i = 0; i < len; i++) {
                    hex[i * 2] = digits[frame.data[i] >> 4]; hex[i * 2 + 1] = digits[frame.data[i] & 15];
                }
                hex[len * 2] = 0;
                shell_print(shell_backend_uart_get_ptr(), "VMCU_CAN_RX id=0x%x flags=0x%x dlc=%u data=%s", frame.id, frame.flags, frame.dlc, hex);
            }
            vmcu_snapshot(&s, &gpio);
            if (session.epoch != s.epoch) { session = (struct vehicle_session) { .epoch = s.epoch }; }
            dispatch(&frame, &s);
        }
    }
}
K_THREAD_DEFINE(can_thread, 4096, worker, NULL, NULL, NULL, 7, 0, 0);

int vmcu_can_shell(const struct shell *shell, size_t argc, char **argv)
{
    if (!device_is_ready(bus)) { return -ENODEV; }
    if (argc == 1 && !strcmp(argv[0], "status")) {
        enum can_state state;
        struct can_bus_err_cnt errors;
        int rc = can_get_state(bus, &state, &errors);
        if (rc) { return rc; }
        shell_print(shell, "VMCU_CAN_STATUS state=%u rx=%ld tx=%ld tx_errors=%ld rx_drops=%ld tec=%u rec=%u",
                    state, (long)atomic_get(&rx_count), (long)atomic_get(&tx_count),
                    (long)atomic_get(&tx_errors), (long)atomic_get(&rx_drops), errors.tx_err_cnt, errors.rx_err_cnt);
        return 0;
    }
    if (argc == 1 && !strcmp(argv[0], "restart")) {
        int rc = can_stop(bus);
        if (rc && rc != -EALREADY) { return rc; }
        rc = can_start(bus);
        shell_print(shell, "VMCU_CAN_RESTART errno=%d", rc);
        return rc;
    }
    if (argc == 2 && !strcmp(argv[0], "trace") &&
        (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        atomic_set(&trace, !strcmp(argv[1], "on"));
        shell_print(shell, "VMCU_CAN_TRACE %s", argv[1]);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[0], "send")) {
        int err = 0;
        unsigned long id = shell_strtoul(argv[1], 0, &err);
        unsigned long flags = shell_strtoul(argv[2], 0, &err);
        size_t len = strlen(argv[3]);
        if (err || flags & ~(CAN_FRAME_IDE | CAN_FRAME_FDF | CAN_FRAME_BRS) ||
            id > (flags & CAN_FRAME_IDE ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK) ||
            ((flags & CAN_FRAME_BRS) && !(flags & CAN_FRAME_FDF)) || len % 2 || len > 128 ||
            (!(flags & CAN_FRAME_FDF) && len > 16)) { return -EINVAL; }
        struct can_frame frame = { .id = id, .flags = flags, .dlc = can_bytes_to_dlc(len / 2) };
        if (can_dlc_to_bytes(frame.dlc) != len / 2) { return -EINVAL; }
        for (unsigned i = 0; i < len / 2; i++) {
            char pair[3] = { argv[3][i * 2], argv[3][i * 2 + 1], 0 };
            frame.data[i] = shell_strtoul(pair, 16, &err);
            if (err) { return -EINVAL; }
        }
        int rc = send(&frame);
        shell_print(shell, "VMCU_CAN_SEND id=0x%x dlc=%u errno=%d", frame.id, frame.dlc, rc);
        return rc;
    }
    return -EINVAL;
}

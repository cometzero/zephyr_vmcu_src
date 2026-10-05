/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT infineon_tc397_mcan
#include <zephyr/drivers/can.h>
#include <zephyr/irq.h>
#include "can_mcan.h"

/* TC3xx groups interrupts in GRINT1/2 instead of implementing ILS/ILE.
 * The common backend's logical line enables are applied to the physical IE.
 * ALRT combines RX overflow and protocol errors, so both common handlers run
 * on either hardware line. No accesses to the reserved ILS/ILE addresses.
 */
struct tc397_config { uintptr_t core, mram, common, node; };
struct tc397_data { uint32_t ie, ils, ile; };
static const struct tc397_config *hw(const struct device *dev)
{
    return ((const struct can_mcan_config *)dev->config)->custom;
}
static struct tc397_data *state(const struct device *dev)
{
    return ((struct can_mcan_data *)dev->data)->custom;
}
static int read_reg(const struct device *dev, uint16_t reg, uint32_t *value)
{
    struct tc397_data *s = state(dev);
    if (reg == CAN_MCAN_IE) { *value = s->ie; return 0; }
    if (reg == CAN_MCAN_ILS) { *value = s->ils; return 0; }
    if (reg == CAN_MCAN_ILE) { *value = s->ile; return 0; }
    return can_mcan_sys_read_reg(hw(dev)->core, reg, value);
}
static int write_reg(const struct device *dev, uint16_t reg, uint32_t value)
{
    struct tc397_data *s = state(dev);
    if (reg == CAN_MCAN_IE) { s->ie = value; }
    else if (reg == CAN_MCAN_ILS) { s->ils = value; }
    else if (reg == CAN_MCAN_ILE) { s->ile = value; }
    else { return can_mcan_sys_write_reg(hw(dev)->core, reg, value); }
    uint32_t mask = (s->ile & 1 ? ~s->ils : 0) | (s->ile & 2 ? s->ils : 0);
    return can_mcan_sys_write_reg(hw(dev)->core, CAN_MCAN_IE, s->ie & mask);
}
static int read_mram(const struct device *dev, uint16_t offset, void *dst, size_t len)
{
    return can_mcan_sys_read_mram(hw(dev)->mram, offset, dst, len);
}
static int write_mram(const struct device *dev, uint16_t offset, const void *src, size_t len)
{
    return can_mcan_sys_write_mram(hw(dev)->mram, offset, src, len);
}
static int clear_mram(const struct device *dev, uint16_t offset, size_t len)
{
    return can_mcan_sys_clear_mram(hw(dev)->mram, offset, len);
}
static int core_clock(const struct device *dev, uint32_t *rate)
{
    ARG_UNUSED(dev);
    *rate = DT_INST_PROP(0, clock_frequency);
    return 0;
}
static void interrupt(const void *arg)
{
    can_mcan_line_0_isr(arg);
    can_mcan_line_1_isr(arg);
}
static int init(const struct device *dev)
{
    const struct tc397_config *c = hw(dev);
    sys_write32(0, c->common); /* CLC: functional module clock enabled. */
    sys_write32(0, c->node + 0x8); /* STARTADR */
    sys_write32(0x7ffc, c->node + 0xc); /* ENDADR */
    sys_write32(0, c->node + 0x14); /* GRINT1: TX events and ALRT -> INT0 */
    sys_write32(0x00011000, c->node + 0x18); /* GRINT2: RX FIFO -> INT1 */
    int rc = can_mcan_configure_mram(dev, c->mram, c->mram);
    if (!rc) { rc = can_mcan_init(dev); }
    if (rc) { return rc; }
    IRQ_CONNECT(DT_INST_IRQ_BY_NAME(0, int0, irq),
                DT_INST_IRQ_BY_NAME(0, int0, priority), interrupt, DEVICE_DT_INST_GET(0), 0);
    IRQ_CONNECT(DT_INST_IRQ_BY_NAME(0, int1, irq),
                DT_INST_IRQ_BY_NAME(0, int1, priority), interrupt, DEVICE_DT_INST_GET(0), 0);
    irq_enable(DT_INST_IRQ_BY_NAME(0, int0, irq));
    irq_enable(DT_INST_IRQ_BY_NAME(0, int1, irq));
    return 0;
}
static DEVICE_API(can, api) = {
    .get_capabilities = can_mcan_get_capabilities,
    .start = can_mcan_start, .stop = can_mcan_stop,
    .set_mode = can_mcan_set_mode, .set_timing = can_mcan_set_timing,
    .send = can_mcan_send, .add_rx_filter = can_mcan_add_rx_filter,
    .remove_rx_filter = can_mcan_remove_rx_filter, .get_state = can_mcan_get_state,
    .get_core_clock = core_clock, .get_max_filters = can_mcan_get_max_filters,
    .timing_min = CAN_MCAN_TIMING_MIN_INITIALIZER,
    .timing_max = CAN_MCAN_TIMING_MAX_INITIALIZER,
    .set_timing_data = can_mcan_set_timing_data,
    .timing_data_min = CAN_MCAN_TIMING_DATA_MIN_INITIALIZER,
    .timing_data_max = CAN_MCAN_TIMING_DATA_MAX_INITIALIZER,
};
static const struct can_mcan_ops ops = {
    .read_reg = read_reg, .write_reg = write_reg,
    .read_mram = read_mram, .write_mram = write_mram, .clear_mram = clear_mram,
};
CAN_MCAN_DT_INST_BUILD_ASSERT_MRAM_CFG(0);
BUILD_ASSERT(CAN_MCAN_DT_INST_MRAM_ELEMENTS_SIZE(0) <= DT_INST_REG_SIZE_BY_NAME(0, message_ram));
static const struct tc397_config custom = {
    .core = DT_INST_REG_ADDR_BY_NAME(0, m_can),
    .mram = DT_INST_REG_ADDR_BY_NAME(0, message_ram),
    .common = DT_INST_REG_ADDR_BY_NAME(0, common),
    .node = DT_INST_REG_ADDR_BY_NAME(0, node),
};
static struct tc397_data custom_data;
CAN_MCAN_DT_INST_CALLBACKS_DEFINE(0, callbacks);
static const struct can_mcan_config config = CAN_MCAN_DT_CONFIG_INST_GET(0, &custom, &ops, &callbacks);
CAN_MCAN_DATA_DEFINE(data, &custom_data);
CAN_DEVICE_DT_INST_DEFINE(0, init, NULL, &data, &config,
                          POST_KERNEL, CONFIG_CAN_INIT_PRIORITY, &api);

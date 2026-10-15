/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT infineon_tc397_qspi
#include <zephyr/drivers/spi.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>

#define BASE DT_INST_REG_ADDR(0)
struct qspi_data { struct k_mutex lock; struct k_sem done; uint32_t status; uint32_t irqs; };
static void interrupt(const void *arg)
{
    const struct device *dev = arg;
    struct qspi_data *data = dev->data;
    uint32_t status = sys_read32(BASE + 0x40);
    data->status = status;
    data->irqs++;
    sys_write32(status & 0x7ff, BASE + 0x54);
    k_sem_give(&data->done);
}
uint32_t vmcu_spi_irq_count(const struct device *dev)
{
    return ((struct qspi_data *)dev->data)->irqs;
}
static int transceive(const struct device *dev, const struct spi_config *cfg,
                     const struct spi_buf_set *tx, const struct spi_buf_set *rx)
{
    struct qspi_data *d = dev->data;
    /* The functional profile intentionally exposes only one contiguous buffer. */
    if (cfg->frequency != 1000000 || cfg->slave || cfg->cs.gpio.port ||
        cfg->operation != SPI_WORD_SET(8) || !tx || !rx ||
        tx->count != 1 || rx->count != 1 ||
        tx->buffers[0].len != rx->buffers[0].len || !tx->buffers[0].len) {
        return -ENOTSUP;
    }
    const uint8_t *out = tx->buffers[0].buf;
    uint8_t *in = rx->buffers[0].buf;
    size_t size = tx->buffers[0].len;
    int rc = 0;
    k_mutex_lock(&d->lock, K_FOREVER);
    for (size_t i = 0; i < size; i++) {
        k_sem_reset(&d->done);
        sys_write32((1 << 21) | (7 << 23) | (i + 1 == size), BASE + 0x60);
        sys_write32(out ? out[i] : 0, BASE + 0x64);
        rc = k_sem_take(&d->done, K_MSEC(1500));
        if (rc || (d->status & 0x1ff)) { rc = rc ? -ETIMEDOUT : -EIO; break; }
        uint8_t byte = sys_read32(BASE + 0x90);
        if (in) { in[i] = byte; }
    }
    if (rc) {
        sys_write32((1u << 24) | (3u << 30), BASE + 0x10);
    }
    k_mutex_unlock(&d->lock);
    return rc;
}
static int release(const struct device *dev, const struct spi_config *cfg)
{
    ARG_UNUSED(dev); ARG_UNUSED(cfg); return 0;
}
static int init(const struct device *dev)
{
    struct qspi_data *d = dev->data;
    k_mutex_init(&d->lock); k_sem_init(&d->done, 0, 1);
    sys_write32(0, BASE);
    sys_write32(1 << 24, BASE + 0x10);
    /* fQSPI=100 MHz, TQ=0, Q=24, A=1, B=1, C=1 -> 1 MHz. */
    sys_write32(24 | (1 << 6) | (1 << 8) | (1 << 10) | (1 << 12), BASE + 0x20);
    sys_write32(1 << 16, BASE + 0x48);
    sys_write32(0x1ff | (1 << 10), BASE + 0x14);
    IRQ_CONNECT(0x3d, 5, interrupt, DEVICE_DT_INST_GET(0), 0);
    IRQ_CONNECT(0x3e, 6, interrupt, DEVICE_DT_INST_GET(0), 0);
    irq_enable(0x3d); irq_enable(0x3e);
    return 0;
}
static DEVICE_API(spi, api) = { .transceive = transceive, .release = release };
static struct qspi_data data;
DEVICE_DT_INST_DEFINE(0, init, NULL, &data, NULL, POST_KERNEL,
                      CONFIG_SPI_INIT_PRIORITY, &api);

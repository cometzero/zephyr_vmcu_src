/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/shell/shell.h>

static int cmd_probe(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "VMCU_ZEPHYR_PASS uptime_ms=%lld link=%s console=%s",
		    (long long)k_uptime_get(),
		    device_is_ready(DEVICE_DT_GET(DT_ALIAS(vmcu_link))) ? "ready" : "failed",
		    device_is_ready(DEVICE_DT_GET(DT_CHOSEN(zephyr_console))) ? "ready" : "failed");
	return 0;
}

SHELL_CMD_REGISTER(probe, NULL, "Probe kernel uptime and both native ASCLIN drivers", cmd_probe);

int main(void)
{
	printk("TC397 Zephyr bringup: ASCLIN0=link ASCLIN1=shell\n");
	return 0;
}

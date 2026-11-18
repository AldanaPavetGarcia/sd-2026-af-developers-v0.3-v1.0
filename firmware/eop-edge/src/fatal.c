/*
 * Fatal kernel errors reboot the device instead of halting it (US-403 AC4). On native_sim the
 * reboot restarts the process (CONFIG_NATIVE_SIM_REBOOT): a new boot, with a new boot_id.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(eop_fatal, LOG_LEVEL_INF);

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	LOG_PANIC();
	LOG_ERR("fatal kernel error (reason %u): rebooting", reason);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

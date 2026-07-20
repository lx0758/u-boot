// SPDX-License-Identifier: GPL-2.0+
/*
 * Reboot to Rockchip BootROM download mode
 */

#include <command.h>
#include <asm/arch-rockchip/boot_mode.h>

static int do_reboot_brom(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	set_back_to_bootrom_dnl_flag();
	do_reset(NULL, 0, 0, NULL);

	return 0;
}

U_BOOT_CMD(
	rbrom, 1, 0, do_reboot_brom,
	"Reboot device into Rockchip BROM download mode",
	""
);

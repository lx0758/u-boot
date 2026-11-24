/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Xunlong Orange Pi 3B (RK3566) 板级环境配置
 *
 * ── 默认环境是怎么拼出来的 ───────────────────────────────────────────────
 * include/env_default.h 中 default_environment 的组成顺序是：
 *
 *     CONFIG_EXTRA_ENV_TEXT      ← board/<vendor>/<board>/<xxx>.env（在前）
 *   + CFG_EXTRA_ENV_SETTINGS     ← 本文件 → rk3568_common.h（在后）
 *
 * 环境导入时同名变量「后者覆盖前者」（lib/hashtable.c 的
 * _compare_and_overwrite_entry 直接覆盖已存在的 key）。因此约定：
 *
 *   - 需要改写 rockchip 默认值的项（内存地址、启动介质列表）必须放在本文件；
 *     写进 .env 是无效的，会被这里的同名项覆盖。
 *   - .env 只放可随手改、不涉及平台内存布局与启动安全的策略项（当前仅 bootcmd），
 *     以及不该进版本库的设备身份/网络参数（留设备端 saved env，连 .env 也不放）。
 *
 * ── DRAM 区段表 ──────────────────────────────────────────────────────────
 * SDRAM_BASE = 0。代码里的地址/容量一律用十六进制整数；下表另给人类可读的
 * 容量与结束地址（单位均为二进制单位，1 MiB = 1048576 B）。
 *
 *   区段                    起始         容量(hex)   容量                  结束
 *   DDR firewall region0    0x00000000  0x00100000  1 MiB                 0x00100000
 *   U-Boot proper           0x00800000  0x00400000  4 MiB                 0x00C00000
 *   loadaddr (boot.img)     0x00C00000  0x04000000  64 MiB                0x04C00000
 *   kernel_addr_r           0x02000000  0x04000000  64 MiB                0x06000000
 *   fdt_addr_r              0x06000000  0x00100000  1 MiB                 0x06100000
 *   fdtoverlay_addr_r       0x06100000  0x00100000  1 MiB                 0x06200000
 *   ramdisk_addr_r          0x06200000  0x01000000  16 MiB                0x07200000
 *   scriptaddr              0x07200000  0x00002000  8 KiB                 0x07202000
 *   pxefile_addr_r          0x07300000  0x00100000  1 MiB                 0x07400000
 *   ---- 0x07400000 - 0x083FFFFF 空闲：16 MiB ----
 *   TZDRAM (OP-TEE/BL32)    0x08400000  0x02000000  32 MiB                0x0A400000
 *   fastboot cache          0x0A400000  0x0A000000  160 MiB               0x14400000
 *   vendor_boot_comp_addr_r 0x0A400000  0x04000000  64 MiB                0x0E400000
 *     ↑ 与 fastboot 缓冲前 64 MiB 重叠：fastboot 模式与 bootflow 扫描互斥
 *   kernel_comp_addr_r      0x14400000  0x08000000  128 MiB               0x1C400000
 *
 * U-Boot proper 容量取 4 MiB（u-boot.bin 实测 1280 KiB），结束于 0x00C00000，
 * 正好接上 loadaddr。
 *
 * vendor_boot_comp_addr_r 与 fastboot 下载缓冲有意重叠：fastboot 模式（下载/刷写/
 * fastboot boot）与 bootflow 扫描互斥，不会同时需要这两块。kernel_comp_addr_r
 * 则不与 fastboot 缓冲重叠 —— fastboot boot 一个压缩内核的 boot.img 时，booti 会把
 * 解压结果写到 kernel_comp_addr_r，源数据仍在 fastboot 缓冲内，重叠会互相破坏。
 *
 * loadaddr（= CONFIG_SYS_LOAD_ADDR）与 kernel_addr_r 区段有意重叠：boot.img 从
 * loadaddr 读入，kernel 副本随后由 booti 从 boot.img 内 memmove 到 kernel_addr_r，
 * 两者不同时使用，因此不做重叠检查。loadaddr 容量按 boot 分区上限 0x04000000
 * （64 MiB）计，结束于 0x04C00000，仍在 fdt_addr_r 之前。
 *
 * ── 内存容量要求 ─────────────────────────────────────────────────────────
 * 最高触碰 0x1C400000（452 MiB）。U-Boot 自身还要占用 ram_top 之下
 * 的 SYS_MALLOC_LEN（0x02000000，32 MiB）、代码与栈约 0x00200000（2 MiB）：
 *   - 默认配置下最低需要约 0x1C400000 + 0x02200000 = 0x1E600000（约 486 MiB），
 *     512 MiB（0x20000000）板型可放下，余量约 0x01A00000（26 MiB）；
 *   - 256 MiB 及以下不可能（TZDRAM 32 MiB + fastboot 160 MiB 已占 192 MiB）。
 *
 * 任何新增静态地址都必须避开 TZDRAM；TZDRAM 大小以 CONFIG_OPTEE_TZDRAM_SIZE
 * 为准（defconfig），内核 DT 的 reserved-memory/secure_ddr 与 DDR firewall
 * 必须与之严格一致，改它就要同步平移 fastboot 缓冲及其后的区段。
 */

#ifndef __CONFIG_ORANGEPI_3B_RK3566_H
#define __CONFIG_ORANGEPI_3B_RK3566_H

#include <linux/stringify.h>

/* 低端区段：地址 + 容量（注释内为人类可读单位） */
#define OPI3B_UBOOT_ADDR		0x00800000	/* CONFIG_TEXT_BASE，结束 0x00C00000 */
#define OPI3B_UBOOT_SIZE		0x00400000	/* 4 MiB，实测 u-boot.bin 1280 KiB */
#define OPI3B_BOOTIMG_ADDR		0x00C00000	/* CONFIG_SYS_LOAD_ADDR，结束 0x04C00000 */
#define OPI3B_BOOTIMG_SIZE		0x04000000	/* 64 MiB（boot 分区上限） */
#define OPI3B_KERNEL_ADDR		0x02000000	/* 结束 0x06000000 */
#define OPI3B_KERNEL_SIZE		0x04000000	/* 64 MiB */
#define OPI3B_FDT_ADDR			0x06000000	/* 结束 0x06100000 */
#define OPI3B_FDT_SIZE			0x00100000	/* 1 MiB */
#define OPI3B_DTBO_ADDR			0x06100000	/* 结束 0x06200000 */
#define OPI3B_DTBO_SIZE			0x00100000	/* 1 MiB */
#define OPI3B_RAMDISK_ADDR		0x06200000	/* 结束 0x07200000 */
#define OPI3B_RAMDISK_SIZE		0x01000000	/* 16 MiB */
#define OPI3B_SCRIPT_ADDR		0x07200000	/* 结束 0x07202000 */
#define OPI3B_SCRIPT_OFFSET		0x00FFE000	/* 16376 KiB */
#define OPI3B_SCRIPT_SIZE		0x00002000	/* 8 KiB */
#define OPI3B_PXE_ADDR			0x07300000	/* 结束 0x07400000 */
#define OPI3B_PXE_SIZE			0x00100000	/* 1 MiB */

/* OP-TEE / BL32 TZDRAM：非安全世界不可写，地址与大小由固件决定 */
#define OPI3B_TZDRAM_ADDR		0x08400000	/* 结束 0x0A400000 */
#define OPI3B_TZDRAM_SIZE		0x02000000	/* 32 MiB */

/* 高端区段：紧跟 TZDRAM 之后依次排布 */
#define OPI3B_FASTBOOT_ADDR		0x0A400000	/* 结束 0x14400000 */
#define OPI3B_FASTBOOT_SIZE		0x0A000000	/* 160 MiB，需与 CONFIG_FASTBOOT_BUF_SIZE 一致 */
#define OPI3B_VENDOR_BOOT_ADDR	0x0A400000	/* 结束 0x0E400000，与 fastboot 缓冲重叠 */
#define OPI3B_VENDOR_BOOT_SIZE	0x04000000	/* 64 MiB */
#define OPI3B_KERNEL_COMP_ADDR	0x14400000	/* 结束 0x1C400000 */
#define OPI3B_KERNEL_COMP_SIZE	0x08000000	/* 128 MiB，沿用 rk3568_common.h 原值 */
#define OPI3B_MEM_TOP			0x1C400000	/* 452 MiB */

/* 编译期检查：除 loadaddr/kernel_addr_r、fastboot/vendor_boot 两处有意重叠外不得相交 */
#if (OPI3B_UBOOT_ADDR + OPI3B_UBOOT_SIZE) > OPI3B_BOOTIMG_ADDR
#error "U-Boot proper 区段越过 loadaddr"
#endif
#if (OPI3B_BOOTIMG_ADDR + OPI3B_BOOTIMG_SIZE) > OPI3B_FDT_ADDR
#error "boot.img 区段越过 fdt_addr_r"
#endif
#if (OPI3B_KERNEL_ADDR + OPI3B_KERNEL_SIZE) > OPI3B_FDT_ADDR
#error "kernel_addr_r 与 fdt_addr_r 区段重叠"
#endif
#if (OPI3B_RAMDISK_ADDR + OPI3B_RAMDISK_SIZE) > OPI3B_SCRIPT_ADDR
#error "ramdisk_addr_r 区段越过 scriptaddr"
#endif
#if (OPI3B_SCRIPT_ADDR + OPI3B_SCRIPT_SIZE) > OPI3B_PXE_ADDR
#error "scriptaddr 区段越过 pxefile_addr_r"
#endif
#if (OPI3B_PXE_ADDR + OPI3B_PXE_SIZE) > OPI3B_TZDRAM_ADDR
#error "pxefile_addr_r 区段越过 TZDRAM"
#endif
#if (OPI3B_TZDRAM_ADDR + OPI3B_TZDRAM_SIZE) > OPI3B_FASTBOOT_ADDR
#error "fastboot 缓冲与 TZDRAM 重叠"
#endif
/* vendor_boot_comp 与 fastboot 缓冲有意重叠，检查它是前者完全落在后者之内 */
#if (OPI3B_VENDOR_BOOT_ADDR + OPI3B_VENDOR_BOOT_SIZE) > \
	(OPI3B_FASTBOOT_ADDR + OPI3B_FASTBOOT_SIZE)
#error "vendor_boot 区段超出 fastboot 缓冲范围"
#endif
#if (OPI3B_FASTBOOT_ADDR + OPI3B_FASTBOOT_SIZE) > OPI3B_KERNEL_COMP_ADDR
#error "fastboot 缓冲与 kernel_comp 区段重叠"
#endif
#if (OPI3B_KERNEL_COMP_ADDR + OPI3B_KERNEL_COMP_SIZE) > OPI3B_MEM_TOP
#error "kernel_comp 区段越过区段表上界"
#endif

/*
 * 启动介质顺序：只扫 eMMC(0) / SD(1) / SPI，跳过 nvme/scsi/usb/pxe/dhcp，
 * 缩短 bootflow 扫描时间。
 *
 * rockchip-common.h 用 #ifndef BOOT_TARGETS 提供默认值，必须在 include 之前
 * 定义才能真正覆盖。此前该值写在 board/{vendor}/{board}/{xxxxx.env} 里，
 * 因上文的覆盖规则从未生效。
 */
#define BOOT_TARGETS	"mmc0 mmc1 spi"

#include <configs/evb_rk3568.h>

/*
 * 完整接管 rk3568_common.h 的 ENV_MEM_LAYOUT_SETTINGS（上游新增项时需要同步）。
 * 与上游默认值的差异：
 *   1) 全部地址按上面的区段表重排，最高触碰从 0x51502C00 降到 0x1C400000；
 *   2) kernel_comp_addr_r 从 0x0A000000（TZDRAM 内）挪到 TZDRAM 之上；
 *   3) 补上 vendor_boot_comp_addr_r —— android bootmeth 用它作为 vendor_boot
 *      的载入地址，缺省为 0 会写到 DDR firewall region0；
 *   4) scriptaddr/pxefile_addr_r 挪到 ramdisk 之后，不再与 loadaddr 混在一起。
 */
#undef ENV_MEM_LAYOUT_SETTINGS
#define ENV_MEM_LAYOUT_SETTINGS						\
	"scriptaddr=" __stringify(OPI3B_SCRIPT_ADDR) "\0"		\
	"script_offset_f=" __stringify(OPI3B_SCRIPT_OFFSET) "\0"	\
	"script_size_f=" __stringify(OPI3B_SCRIPT_SIZE) "\0"		\
	"pxefile_addr_r=" __stringify(OPI3B_PXE_ADDR) "\0"		\
	"kernel_addr_r=" __stringify(OPI3B_KERNEL_ADDR) "\0"		\
	"fdt_addr_r=" __stringify(OPI3B_FDT_ADDR) "\0"			\
	"fdtoverlay_addr_r=" __stringify(OPI3B_DTBO_ADDR) "\0"		\
	"ramdisk_addr_r=" __stringify(OPI3B_RAMDISK_ADDR) "\0"		\
	"vendor_boot_comp_addr_r=" __stringify(OPI3B_VENDOR_BOOT_ADDR) "\0" \
	"kernel_comp_addr_r=" __stringify(OPI3B_KERNEL_COMP_ADDR) "\0"	\
	"kernel_comp_size=" __stringify(OPI3B_KERNEL_COMP_SIZE) "\0"

#endif /* __CONFIG_ORANGEPI_3B_RK3566_H */
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * TM4C1294NCPDT (Tiva-C, Cortex-M4F) memory map and board knobs for
 * the armbl example bootloader. Edit the addresses below if you target
 * a different application region or board configuration.
 */

#ifndef ARMBL_APP_LAYOUT_H
#define ARMBL_APP_LAYOUT_H

#include <stdint.h>

/* Application region. The bootloader occupies 0x00000000..0x00003FFF
 * (16 KiB across the first 16 KiB of on-chip flash); the application
 * runs above. The TM4C1294NCPDT has 1 MiB of flash but this example
 * sizes the application region at ~240 KiB so a typical mid-sized
 * firmware fits comfortably without reserving the full part. */
#define APP_START_ADDR      ((uint32_t)0x00004000)
#define APP_END_ADDR        ((uint32_t)0x0003FFFF)

/* Two reserved 32-bit slots near the end of the application region.
 * The version word is written by the application's link-time const;
 * the done flag is written by the bootloader after a successful
 * upgrade. */
#define VERSION_ADDR        ((uint32_t)0x0003FFF0)
#define DONE_FLAG_ADDR      ((uint32_t)0x0003FFF4)
#define DONE_FLAG_VALUE     ((uint32_t)0xCAFEBABE)

/* Flash page granularity. TM4C1294NCPDT FlashErase clears 1 KiB
 * blocks; the armbl engine asks the erase hook to clear
 * [APP_START, APP_END] inclusive and the hook rounds to this. */
#define FLASH_PAGE_SIZE     ((uint32_t)0x400)

/* UART0 baud. Match the application's transport. */
#define UART_BAUD           115200u

/* Target system clock. SysCtlClockFreqSet on the TM4C129 family is
 * happiest at 120 MHz with the 25 MHz main crystal driving the
 * 480 MHz VCO. Change this if the board uses a different crystal. */
#define SYS_CLOCK_HZ        120000000u

#endif /* ARMBL_APP_LAYOUT_H */

/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * STM32F051C8 (64K flash, 8K RAM) memory map for the armbl example
 * bootloader. Edit the addresses below if you target a different
 * density part or rearrange the application region.
 */

#ifndef ARMBL_APP_LAYOUT_H
#define ARMBL_APP_LAYOUT_H

#include <stdint.h>

/* Application region. The bootloader occupies 0x08000000..0x08001FFF
 * (8 KiB across the first 8 pages); the application runs above. */
#define APP_START_ADDR      ((uint32_t)0x08002000)
#define APP_END_ADDR        ((uint32_t)0x0800FBFF)

/* Two reserved 32-bit slots near the very end of flash. The version
 * word is written by the application's link-time const; the done
 * flag is written by the bootloader after a successful upgrade. */
#define VERSION_ADDR        ((uint32_t)0x0800FBF0)
#define DONE_FLAG_ADDR      ((uint32_t)0x0800FBF4)
#define DONE_FLAG_VALUE     ((uint32_t)0xCAFEBABE)

/* Flash page granularity for STM32F051. STM32F072 variants use 2 KiB
 * pages -- override this define if you target one. The armbl engine
 * asks the erase hook to clear [APP_START, APP_END] inclusive; the
 * hook must round to this. */
#define FLASH_PAGE_SIZE     ((uint32_t)0x400)

/* USART1 baud. Match the application's transport. */
#define UART_BAUD           57600u

/* HSI 8 MHz x PLL x6 = 48 MHz. SystemInit() in system_stm32f0xx.c is
 * assumed to have already configured this before main() runs; the
 * value is exposed here for any timing math the example wants to do. */
#define SYS_CLOCK_HZ        48000000u

#endif /* ARMBL_APP_LAYOUT_H */

/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * TM4C1294NCPDT hook implementations: flash erase / program / read on
 * the on-chip flash controller, blocking polled UART0 transmit, and a
 * vector-table-relocating jump into the application image.
 */

#include "hooks.h"
#include "armbl_app_layout.h"

#include <stdint.h>
#include <stdbool.h>

#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_nvic.h"
#include "driverlib/flash.h"
#include "driverlib/uart.h"
#include "driverlib/sysctl.h"
#include "driverlib/interrupt.h"
#include "driverlib/systick.h"

/* __set_MSP is the Cortex-M CMSIS intrinsic for writing the main
 * stack pointer. IAR exposes it via <intrinsics.h>; arm-none-eabi
 * exposes it via <cmsis_gcc.h> (pulled in by TivaWare's CMSIS
 * headers). Both compilers inline it to a single MSR instruction.
 * The forward declaration avoids tying this file to one toolchain's
 * header layout. */
#if defined(__IAR_SYSTEMS_ICC__)
#include <intrinsics.h>
#elif defined(__GNUC__)
#include "cmsis_gcc.h"
#else
extern void __set_MSP(uint32_t top_of_main_stack);
#endif

/* hooks_flash_erase clears every 1 KiB block in the closed range
 * [start, end]. TivaWare's FlashErase is synchronous and erases the
 * 1 KiB block that contains the address it is given; we step by
 * FLASH_PAGE_SIZE and bail on the first non-zero return. There is
 * no flash-unlock register on TM4C129 -- the controller is always
 * accessible from the CPU -- so unlike the STM32 hook there is no
 * lock / unlock dance to perform. */
armbl_flash_result hooks_flash_erase(uint32_t start_addr, uint32_t end_addr)
{
    uint32_t addr = start_addr & ~(FLASH_PAGE_SIZE - 1u);
    while (addr <= end_addr) {
        if (FlashErase(addr) != 0) {
            return ARMBL_FLASH_ERR;
        }
        addr += FLASH_PAGE_SIZE;
    }
    return ARMBL_FLASH_OK;
}

/* hooks_flash_write programs one 32-bit word. FlashProgram takes a
 * source pointer + destination address + byte count; we stage the
 * word on the stack so the source is word-aligned. */
armbl_flash_result hooks_flash_write(uint32_t addr, uint32_t word)
{
    uint32_t buf = word;
    int32_t s = FlashProgram(&buf, addr, 4);
    return (s == 0) ? ARMBL_FLASH_OK : ARMBL_FLASH_ERR;
}

uint32_t hooks_flash_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* hooks_write_line transmits one line over UART0 with a single
 * appended '\n'. Polled / blocking on purpose: lines are short
 * (under ~80 bytes) and infrequent (one per protocol exchange),
 * and avoiding TX-DMA / TX-IRQ keeps the bootloader trivially
 * simple to port. Never call from an ISR. */
void hooks_write_line(const char *line, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        UARTCharPut(UART0_BASE, (uint8_t)line[i]);
    }
    UARTCharPut(UART0_BASE, (uint8_t)'\n');

    /* Wait for the last byte to leave the shift register so the
     * next state's UART traffic doesn't collide if we immediately
     * disable the peripheral on jump_to_app. */
    while (UARTBusy(UART0_BASE)) { }
}

/* hooks_jump_to_app must not return. After this point the
 * application owns the vector table, MSP, NVIC and clocks. */
void hooks_jump_to_app(uint32_t app_start_addr)
{
    IntMasterDisable();

    UARTIntDisable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    UARTDisable(UART0_BASE);
    SysTickIntDisable();
    SysTickDisable();
    IntDisable(INT_UART0);

    IntVTableBaseSet(app_start_addr);

    __set_MSP(*(volatile uint32_t *)app_start_addr);
    ((void (*)(void))*(volatile uint32_t *)(app_start_addr + 4))();

    while (1) { }
}

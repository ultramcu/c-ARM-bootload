/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * STM32F0 hook implementations: flash erase / program / read on the
 * embedded flash controller, blocking polled USART1 transmit, and a
 * direct call into the application's fixed entry function.
 */

#include "hooks.h"
#include "armbl_app_layout.h"

#include "stm32f0xx.h"
#include "stm32f0xx_flash.h"
#include "stm32f0xx_usart.h"
#include "stm32f0xx_tim.h"
#include "stm32f0xx_misc.h"
#include "core_cm0.h"

/* hooks_flash_erase clears every page in the closed range
 * [start, end]. StdPeriph FLASH_ErasePage is synchronous and takes
 * a few tens of milliseconds per 1 KiB page on STM32F051, so a full
 * 55-page application erase takes well over a second -- main.c sizes
 * the IWDG to give margin. On the success path flash is left
 * unlocked so that the subsequent stream of flash_write() calls
 * doesn't pay an unlock round-trip per word; flash_write() does not
 * lock again either. */
armbl_flash_result hooks_flash_erase(uint32_t start_addr, uint32_t end_addr)
{
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP
                    | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);

    uint32_t addr = start_addr & ~(FLASH_PAGE_SIZE - 1u);
    while (addr <= end_addr) {
        if (FLASH_ErasePage(addr) != FLASH_COMPLETE) {
            FLASH_Lock();
            return ARMBL_FLASH_ERR;
        }
        addr += FLASH_PAGE_SIZE;
    }
    return ARMBL_FLASH_OK;
}

/* hooks_flash_write assumes flash is already unlocked: the erase
 * step left it that way and the engine writes thousands of words
 * back-to-back. */
armbl_flash_result hooks_flash_write(uint32_t addr, uint32_t word)
{
    FLASH_Status s = FLASH_ProgramWord(addr, word);
    return (s == FLASH_COMPLETE) ? ARMBL_FLASH_OK : ARMBL_FLASH_ERR;
}

uint32_t hooks_flash_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* hooks_write_line transmits one line over USART1 with a single
 * appended '\n'. Polled / blocking on purpose: lines are short
 * (under ~80 bytes) and infrequent (one per protocol exchange),
 * and avoiding TX-DMA / TX-IRQ keeps the bootloader trivially
 * simple to port. Never call from an ISR. */
void hooks_write_line(const char *line, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) { }
        USART_SendData(USART1, (uint16_t)(uint8_t)line[i]);
    }
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) { }
    USART_SendData(USART1, (uint16_t)'\n');

    /* Wait for the last byte to leave the shift register so the
     * next state's UART traffic doesn't collide if we immediately
     * disable the peripheral on jump_to_app. */
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) { }
}

/* hooks_jump_to_app must not return. After this point the
 * application owns the CPU; the bootloader's peripherals are
 * quiesced first so no stray IRQ fires after the handover. */
void hooks_jump_to_app(uint32_t app_start_addr)
{
    __disable_irq();

    USART_ITConfig(USART1, USART_IT_RXNE, DISABLE);
    USART_Cmd(USART1, DISABLE);
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
    TIM_Cmd(TIM3, DISABLE);

    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_DisableIRQ(TIM3_IRQn);

    /* Leave flash in a defined locked state for the application;
     * if the application needs to program flash it will FLASH_Unlock
     * itself. */
    FLASH_Lock();

    /* Cortex-M0 has no vector-table-offset register; we cannot
     * point the CPU at the application's vector table. Instead we
     * treat the application's fixed entry function at app_start_addr
     * as a normal function call. The application is responsible (via
     * SYSCFG memory remap or by simply not using its own interrupts)
     * for handling its own ISRs once running. */
    void (*app_entry)(void) = (void (*)(void))(app_start_addr | 1u);
    app_entry();

    while (1) { }
}

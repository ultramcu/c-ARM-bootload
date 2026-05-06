/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * STM32F051 bootloader entry point. Brings up clocks, GPIOs,
 * USART1, TIM3 (10 ms tick), the independent watchdog and NVIC,
 * then hands control to the armbl engine. The main loop kicks
 * the watchdog and calls armbl_tick() once per 10 ms tick.
 */

#include <stdint.h>

#include "stm32f0xx.h"
#include "stm32f0xx_rcc.h"
#include "stm32f0xx_gpio.h"
#include "stm32f0xx_usart.h"
#include "stm32f0xx_tim.h"
#include "stm32f0xx_iwdg.h"
#include "stm32f0xx_misc.h"

#include "../../armbl.h"
#include "armbl_app_layout.h"
#include "hooks.h"

static armbl_ctx        g_ctx;
static volatile uint8_t g_tick_10ms_flag;

static void rcc_init(void);
static void gpio_init(void);
static void usart1_init(void);
static void tim3_init(void);
static void iwdg_init(void);
static void nvic_init(void);

/* Cortex-M0 has no vector-table-offset register -- see
 * hooks_jump_to_app for the handover convention this example uses. */
static const armbl_config g_cfg = {
    .hooks = {
        .flash_erase = hooks_flash_erase,
        .flash_write = hooks_flash_write,
        .flash_read  = hooks_flash_read,
        .write_line  = hooks_write_line,
        .jump_to_app = hooks_jump_to_app,
        .decide      = NULL,   /* default policy */
        .log         = NULL
    },
    .memmap = {
        .app_start_addr  = APP_START_ADDR,
        .app_end_addr    = APP_END_ADDR,
        .version_addr    = VERSION_ADDR,
        .done_flag_addr  = DONE_FLAG_ADDR,
        .done_flag_value = DONE_FLAG_VALUE
    },
    .version_timeout_ticks = 100,   /* 1.0 s @ 10 ms */
    .version_retries       = 3,
    .line_timeout_ticks    = 500,   /* 5.0 s @ 10 ms */
    .line_retries          = 3
};

int main(void)
{
    rcc_init();
    gpio_init();
    usart1_init();
    tim3_init();
    nvic_init();
    iwdg_init();

    if (armbl_init(&g_ctx, &g_cfg) != 0) {
        /* Misconfigured engine. Park here and let the watchdog
         * recover us; we have no other reporting channel yet. */
        while (1) { }
    }

    while (1) {
        IWDG_ReloadCounter();

        if (g_tick_10ms_flag) {
            g_tick_10ms_flag = 0;
            armbl_tick(&g_ctx);
        }
    }
}

/* SystemInit() in system_stm32f0xx.c has already configured HSI x
 * PLL x6 = 48 MHz before main() runs, so we only have to enable the
 * peripheral buses we use. SYSCFG is enabled because the application
 * may need it after the jump (for the SRAM remap path); the
 * bootloader itself does not configure any SYSCFG register. */
static void rcc_init(void)
{
    SystemInit();

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_AHBPeriphClockCmd (RCC_AHBPeriph_GPIOA,   ENABLE);
}

/* USART1 lives on PA9 (TX) and PA10 (RX), both at AF1. STM32F0 GPIO
 * uses the modern alternate-function model: pin AF select first,
 * then GPIO_Init in AF mode with explicit OType / PuPd / Speed. */
static void gpio_init(void)
{
    GPIO_InitTypeDef gpio;

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9,  GPIO_AF_1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_1);

    gpio.GPIO_Pin   = GPIO_Pin_9 | GPIO_Pin_10;
    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);
}

static void usart1_init(void)
{
    USART_InitTypeDef u;

    u.USART_BaudRate            = UART_BAUD;
    u.USART_WordLength          = USART_WordLength_8b;
    u.USART_StopBits            = USART_StopBits_1;
    u.USART_Parity              = USART_Parity_No;
    u.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    u.USART_Mode                = USART_Mode_Rx | USART_Mode_Tx;

    USART_Init(USART1, &u);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART1, ENABLE);
}

/* TIM3 fires its update IRQ every 10 ms. With APB1 timer clock at
 * 48 MHz and prescaler 4799 (-> 10 kHz), a period of 100 yields
 * exactly 10 ms. Adjust both numbers together if SystemInit() is
 * configured for a different frequency. */
static void tim3_init(void)
{
    TIM_TimeBaseInitTypeDef tb;

    tb.TIM_Prescaler         = 4799;
    tb.TIM_CounterMode       = TIM_CounterMode_Up;
    tb.TIM_Period            = 99;
    tb.TIM_ClockDivision     = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM3, &tb);

    TIM_ClearFlag(TIM3, TIM_FLAG_Update);
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM3, ENABLE);
}

/* IWDG: 40 kHz LSI / 32 = 1.25 kHz. Reload 3750 -> 3.0 s timeout.
 * A full erase of 55 x 1 KiB pages can take well over a second, so
 * this gives us comfortable margin without being so long that a
 * hung bootloader sits there for ages. */
static void iwdg_init(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_32);
    IWDG_SetReload(3750);
    IWDG_ReloadCounter();
    IWDG_Enable();
}

/* Cortex-M0 has only 2 priority bits (4 levels) and no priority
 * grouping register, so the StdPeriph priority-group helper does
 * not exist on F0 and NVIC_InitTypeDef carries a single
 * NVIC_IRQChannelPriority field. We give the UART RX a slightly
 * higher priority than the tick so a byte arriving on the boundary
 * of a tick still gets latched promptly. */
static void nvic_init(void)
{
    NVIC_InitTypeDef nv;

    nv.NVIC_IRQChannel         = USART1_IRQn;
    nv.NVIC_IRQChannelPriority = 0;
    nv.NVIC_IRQChannelCmd      = ENABLE;
    NVIC_Init(&nv);

    nv.NVIC_IRQChannel         = TIM3_IRQn;
    nv.NVIC_IRQChannelPriority = 1;
    NVIC_Init(&nv);
}

/* USART1 RX ISR. Reads one byte and feeds the engine. The library
 * documents armbl_feed_byte() as ISR-safe; we do nothing else here
 * so we don't need to gate against the main-loop tick. */
void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) != RESET) {
        uint8_t b = (uint8_t)USART_ReceiveData(USART1);
        armbl_feed_byte(&g_ctx, b);
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
}

/* TIM3 update ISR. 10 ms cadence. */
void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
        g_tick_10ms_flag = 1;
    }
}

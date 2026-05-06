/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * TM4C1294NCPDT bootloader entry point. Brings up the system clock,
 * GPIOA pins for UART0, UART0 itself, SysTick (10 ms tick), Watchdog0
 * and the NVIC, then hands control to the armbl engine. The main loop
 * kicks the watchdog and calls armbl_tick() once per 10 ms tick.
 */

#include <stdint.h>
#include <stdbool.h>

#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/uart.h"
#include "driverlib/systick.h"
#include "driverlib/watchdog.h"
#include "driverlib/interrupt.h"
#include "driverlib/fpu.h"

#include "../../armbl.h"
#include "armbl_app_layout.h"
#include "hooks.h"

static armbl_ctx        g_ctx;
static volatile uint8_t g_tick_10ms_flag;
static uint32_t         g_sys_clock_hz;

static void clock_init(void);
static void gpio_init(void);
static void uart0_init(void);
static void systick_init(void);
static void watchdog_init(void);
static void nvic_init(void);

/* The application's reset vector is the second word of its image. */
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
    clock_init();
    gpio_init();
    uart0_init();
    systick_init();
    nvic_init();
    watchdog_init();

    if (armbl_init(&g_ctx, &g_cfg) != 0) {
        /* Misconfigured engine. Park here and let the watchdog
         * recover us; we have no other reporting channel yet. */
        while (1) { }
    }

    while (1) {
        /* Feed the watchdog. WatchdogReloadSet is the canonical
         * pet-the-dog call on TivaWare; it also handles the
         * controller's internal lock state. */
        WatchdogReloadSet(WATCHDOG0_BASE, g_sys_clock_hz * 3u);

        if (g_tick_10ms_flag) {
            g_tick_10ms_flag = 0;
            armbl_tick(&g_ctx);
        }
    }
}

/* Run from the PLL at 120 MHz with the 25 MHz main crystal driving
 * the 480 MHz VCO -- the canonical TM4C129 sweet spot. The actual
 * frequency returned by SysCtlClockFreqSet is what UART, SysTick and
 * Watchdog reload calculations are based on. */
static void clock_init(void)
{
    /* The Cortex-M4F has a hardware FPU. Lazy stacking lets us skip
     * the FPU register save in non-FP ISRs (which is all of them in
     * the bootloader) at no correctness cost. */
    FPUEnable();
    FPULazyStackingEnable();

    g_sys_clock_hz = SysCtlClockFreqSet(SYSCTL_XTAL_25MHZ
                                        | SYSCTL_OSC_MAIN
                                        | SYSCTL_USE_PLL
                                        | SYSCTL_CFG_VCO_480,
                                        SYS_CLOCK_HZ);
}

/* UART0 lives on PA0 (U0RX) and PA1 (U0TX). No other pins are touched
 * by the bootloader. */
static void gpio_init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA)) { }

    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
}

static void uart0_init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART0)) { }

    UARTConfigSetExpClk(UART0_BASE, g_sys_clock_hz, UART_BAUD,
                        UART_CONFIG_WLEN_8
                        | UART_CONFIG_STOP_ONE
                        | UART_CONFIG_PAR_NONE);
    UARTFIFOEnable(UART0_BASE);
    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    UARTEnable(UART0_BASE);
}

/* SysTick runs off the system clock. Period = sysclock / 100 yields
 * exactly a 10 ms reload. The Cortex-M SysTick exception sits in the
 * core vector table and does not need a separate NVIC enable. */
static void systick_init(void)
{
    SysTickPeriodSet(g_sys_clock_hz / 100u);
    SysTickIntEnable();
    SysTickEnable();
}

/* Watchdog0 on the system clock. Reload = sysclock * 3 -> ~3 s
 * timeout (the watchdog actually resets on the second timeout, but
 * a single-stage view is good enough at this margin). Watchdog0 is
 * preferred over Watchdog1 because it does not need an explicit
 * SysCtlClockOutputSet call to be clocked. */
static void watchdog_init(void)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_WDOG0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_WDOG0)) { }

    /* If the watchdog is already locked from a previous boot stage,
     * unlock so we can program the reload. WatchdogUnlock is a no-op
     * if it isn't locked. */
    if (WatchdogLockState(WATCHDOG0_BASE)) {
        WatchdogUnlock(WATCHDOG0_BASE);
    }

    WatchdogReloadSet(WATCHDOG0_BASE, g_sys_clock_hz * 3u);
    WatchdogResetEnable(WATCHDOG0_BASE);
    WatchdogEnable(WATCHDOG0_BASE);
}

static void nvic_init(void)
{
    IntPrioritySet(INT_UART0, 0);
    IntEnable(INT_UART0);
    IntMasterEnable();
}

/* UART0 RX ISR. Drains every byte currently in the FIFO and feeds
 * each one to the engine. The library documents armbl_feed_byte() as
 * ISR-safe; we do nothing else here so we don't need to gate against
 * the main-loop tick. */
void UART0IntHandler(void)
{
    uint32_t status = UARTIntStatus(UART0_BASE, true);
    UARTIntClear(UART0_BASE, status);

    while (UARTCharsAvail(UART0_BASE)) {
        int32_t c = UARTCharGetNonBlocking(UART0_BASE);
        if (c >= 0) {
            armbl_feed_byte(&g_ctx, (uint8_t)c);
        }
    }
}

/* SysTick ISR. 10 ms cadence. */
void SysTickIntHandler(void)
{
    g_tick_10ms_flag = 1;
}

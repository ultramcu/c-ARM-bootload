# STM32F051 worked example

This directory wires the `c-ARM-bootload` library (`armbl_*` API)
into a real STM32F051C8 (Cortex-M0, 64 KiB flash, 8 KiB RAM)
using the ST StdPeriph driver library for F0. It is not a
complete project — there is no linker script, startup file or
Makefile — only the four C files that make up the bootloader's
own translation units. Drop them into your existing IAR / Keil /
arm-none-eabi-gcc project that already has StdPeriph for F0 set
up and you have a working bootloader.

## Why this example exists

The other two ports in `example/` (STM32F1, TM4C129) target
parts that have a vector-table offset register — `SCB->VTOR` on
Cortex-M3/M4 — so the bootloader can hand control to the
application by relocating the vector table to the application's
flash region. The first instruction the CPU then fetches for
any IRQ comes from the application's own table.

**Cortex-M0 has no VTOR.** The vector table is fixed at flash
address `0x00000000` for the whole life of the device. Whatever
table is at the bottom of flash is what dispatches every
interrupt — bootloader runs, application runs, doesn't matter.

That changes the bootloader / application contract in two
non-trivial ways:

1. The bootloader cannot relocate vectors. Its `jump_to_app` is
   shorter than the M3/M4 version: just quiesce peripherals and
   call into the application.
2. The application cannot rely on its own table being live just
   by linking it at the application's base address. If the
   application wants its own ISRs it must perform the **SYSCFG
   memory-remap trick** (described below) before enabling any
   interrupts.

This example demonstrates the bootloader half. The application-
side conventions you must follow are documented under
"Application-side contract".

## Files in this directory

| File | What it does |
|---|---|
| `main.c` | Bootloader entry point. RCC/GPIO/USART1/TIM3/IWDG/NVIC bring-up, then 10 ms-driven main loop. Owns the two ISRs (USART1 RX, TIM3 update). |
| `hooks.c` | The five `armbl_hooks` callbacks. The hook of interest here is `hooks_jump_to_app` — see "The Cortex-M0 jump" below. |
| `hooks.h` | Declarations of the five hooks. |
| `armbl_app_layout.h` | STM32F0-specific knobs grouped in one header: flash addresses, page size, baud rate, target system clock. |

The library itself (`armbl.h` / `armbl.c`) sits two directories
up. Includes use the relative path `"../../armbl.h"`.

## Reference hardware

| Property | Value |
|---|---|
| MCU | STM32F051C8 (Cortex-M0, 64 KiB flash, 8 KiB RAM) |
| Flash page size | 1 KiB |
| SYSCLK | 48 MHz (HSI8 × PLL ×6, the StdPeriph default for `system_stm32f0xx.c`) |
| Bootloader UART | USART1 on PA9 (TX) / PA10 (RX), 57 600 8N1 |
| Tick source | TIM3, 10 ms update IRQ |
| Watchdog | IWDG, 3.0 s timeout |

For an STM32F072 (high-density 128 KiB) you only edit
`armbl_app_layout.h` — push `APP_END_ADDR` higher and double
`FLASH_PAGE_SIZE` to `0x800`. For an STM32F042 the existing
defaults work as-is on the smaller die.

## Flash partitioning

The example uses an 8 KiB bootloader at the bottom of flash, a
~55 KiB application above, and two reserved 4-byte slots near
the end of the application region for the version word and the
upgrade-done sentinel.

```
0x0800_0000  ┐
             │   bootloader text + rodata + vectors         8 KiB
0x0800_1FFF  ┘
0x0800_2000  ┐ <- APP_START_ADDR
             │   application image                         ~55 KiB
0x0800_FBEF  ┘
0x0800_FBF0      version word (4 B)         <- VERSION_ADDR
0x0800_FBF4      done sentinel (4 B)        <- DONE_FLAG_ADDR
0x0800_FBF8      reserved
0x0800_FBFF                                 <- APP_END_ADDR
```

The bootloader is smaller here than in the F1 example
(8 KiB vs. 16 KiB) because the M0 build of `c-ARM-bootload` plus
StdPeriph + IWDG / TIM3 / USART1 init comfortably fits.

## The Cortex-M0 jump

This is the load-bearing part of the example. Compare to the F1
example next door:

```c
/* STM32F1 (Cortex-M3) -- vector relocation IS available */
NVIC_SetVectorTable(NVIC_VectTab_FLASH, app_start - 0x08000000);
__set_MSP(*(volatile uint32_t *)app_start);
((void(*)(void))*(volatile uint32_t *)(app_start + 4))();
```

```c
/* STM32F0 (Cortex-M0) -- no VTOR, no MSP-from-vectors trick */
void (*app_entry)(void) = (void (*)(void))(app_start | 1u);
app_entry();
```

What changed:

- **No `NVIC_SetVectorTable`.** The function does not exist for
  Cortex-M0 because the underlying register doesn't exist.
- **No `__set_MSP` from the application's vector table.** On
  M3/M4 the first 32-bit word at the application's base is the
  initial main-stack-pointer value. On this M0 example the
  application's base address holds an **entry function**, not an
  MSP word — see "Application-side contract" — so loading those
  4 bytes as MSP would jump to whatever the linker happened to
  put at `*(uint32_t *)(app_start + 4)`, which is just code.
- **No `+ 4` reset-vector dereference.** Same reason. The
  application's entry function is at `app_start` itself; we call
  it directly.
- **The Thumb bit (`| 1u`).** Cortex-M only executes Thumb code,
  and a function-pointer value with bit 0 clear faults on call.
  The application's entry function is at an even flash address
  (the linker aligns it), so we set bit 0 ourselves.

The application inherits the bootloader's stack pointer when
control transfers. That's fine — the application will set up
its own SP early in its entry function.

The rest of `hooks_jump_to_app` is the usual peripheral teardown
(disable USART RX IRQ, disable USART, disable TIM3 IRQ, disable
TIM3, clear NVIC enables, lock flash) plus an `__disable_irq()`
at the top to mask the CPU during handover.

## Application-side contract

The bootloader cannot work alone — the application has to honour
two conventions for the handover to land cleanly. **None of this
is the bootloader's code; it lives in the application's project
that you build separately.**

### 1. The entry function must be at `APP_START_ADDR`

The bootloader calls the address `APP_START_ADDR | 1u` as a
function pointer. The application must place a function with the
signature

```c
void app_entry(void);
```

at exactly `APP_START_ADDR` (`0x08002000` in this example).

Two ways to do that:

**IAR (`#pragma`)**

```c
#pragma location = 0x08002000
__root void app_entry(void)
{
    __disable_irq();
    __set_MSP(0x20002000);          /* RAM top, F051 has 8 KiB */

    /* Optionally: SYSCFG remap, then enable interrupts. */
    application_main();             /* never returns */
}
```

`__root` keeps the linker from dead-stripping the function. The
linker also needs an entry in the ICF that places the section
`.app_entry` at `0x08002000`, and you have to drop the function
into that section if you split it from the rest of the image.

**arm-none-eabi-gcc (`__attribute__`)**

```c
__attribute__((section(".app_entry")))
__attribute__((used))
void app_entry(void)
{
    __disable_irq();
    __set_MSP(0x20002000);
    application_main();
}
```

In the linker script:

```
MEMORY
{
    APP_FLASH (rx) : ORIGIN = 0x08002000, LENGTH = 56K - 16
    APP_TAIL  (rx) : ORIGIN = 0x0800FBF0, LENGTH = 16
}

SECTIONS
{
    .app_entry 0x08002000 : { KEEP(*(.app_entry)) } > APP_FLASH
    .text                 : { *(.text*) *(.rodata*) } > APP_FLASH
    .app_tail 0x0800FBF0  : { KEEP(*(.app_tail)) } > APP_TAIL
}
```

The two-section layout reserves the version + done-sentinel slot
at the top of the application region so they survive a relink.

### 2. The application's vector table needs SYSCFG remap

If your application uses *any* IRQ, it has to make the CPU read
that IRQ's handler from the application's vector table, not the
bootloader's. The standard recipe on STM32F0 is:

```c
extern uint32_t __app_vectors_start;   /* linker-defined symbol */
#define APP_VECTOR_COUNT 48            /* F051: 16 system + 32 IRQ */

void app_entry(void)
{
    __disable_irq();
    __set_MSP(0x20002000);

    /* 1. Copy the application's vector table into SRAM (which on
     *    F0 always lives at 0x20000000). */
    uint32_t *src = &__app_vectors_start;
    uint32_t *dst = (uint32_t *)0x20000000;
    for (int i = 0; i < APP_VECTOR_COUNT; i++) {
        dst[i] = src[i];
    }

    /* 2. Tell SYSCFG to map SRAM at 0x00000000 -- which is what
     *    the CPU reads for vectors -- in place of the bootloader's
     *    flash that's been there since reset. The bus clock for
     *    SYSCFG must be enabled before this write. */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);
    SYSCFG->CFGR1 |= SYSCFG_CFGR1_MEM_MODE_1
                   | SYSCFG_CFGR1_MEM_MODE_0;

    /* 3. From here on, IRQs dispatch through the application's
     *    own vectors. Bring up your peripherals and __enable_irq(). */
    application_main();
}
```

If your application is purely polled and doesn't use any IRQs,
you can skip steps 1 and 2 entirely — the bootloader's vector
table will still be at `0x00000000` but no IRQ will ever fire to
go through it.

The bootloader does **not** enable the SYSCFG peripheral clock
on the application's behalf. The application enables it itself
when it does the remap, just like any other peripheral it
chooses to use.

## ISR vs main-loop split

```
USART1 RX ISR          TIM3 update ISR           main loop
-----------------      -----------------         ----------------
read RXDR              clear UIF                 IWDG_ReloadCounter()
armbl_feed_byte(&ctx)  g_tick_10ms_flag = 1
clear RXNE                                       if (flag) {
                                                     flag = 0;
                                                     armbl_tick(&ctx);
                                                 }
```

Identical to the F1 example because the engine doesn't care
about the chip. The same three rules apply:

1. No printf, no flash, no peripheral re-init inside an ISR.
2. TIM3 doesn't do work — it lifts a flag.
3. The watchdog is fed only from the main loop.

## Watchdog math

```
LSI ≈ 40 kHz
prescaler 32 ⇒ counter clock = 1.25 kHz  (0.8 ms / count)
reload   3750 ⇒ timeout       = 3000 ms
```

A full 55-page application erase takes about 1.7 s on real
silicon. Three seconds gives margin without parking a stuck
bootloader for an annoyingly long time.

## End-to-end upgrade walk-through

What happens on a reset, byte by byte:

1. CPU resets, runs `Reset_Handler` from the bootloader's vector
   table at `0x08000000`, enters `main()`. StdPeriph init brings
   up the peripherals; `armbl_init` zeros the engine context.
2. First `armbl_tick` pushes state from `INIT` to `ASK_VER`, sends
   `V?\n` over USART1.
3. The application side responds:
   - `V 1 2\n` → engine compares against the version word in
     flash; default policy boots if the major matches and
     upgrades otherwise.
   - `V 0 0\n` → explicit upgrade ask.
   - silence  → engine retries, then upgrades if no done
     sentinel is set.
4. On the upgrade path: engine sends `L? 1\n`. The host side
   replies with the first S-record from
   `arm-none-eabi-objcopy -O srec --srec-forceS3 firmware.elf firmware.s19`.
5. Engine parses the record, calls `flash_erase` for the whole
   application region (only on the first record), programs the
   record's bytes word-by-word, sends `OK\n`, asks `L? 2\n`,
   loops.
6. After the last data record the host sends an `S7…\n`
   terminator. Engine writes `0xCAFEBABE` at `DONE_FLAG_ADDR`,
   sends `DONE\n`, waits for the TC bit, locks flash, then calls
   `jump_to_app`.
7. `jump_to_app` masks IRQs, disables peripherals, then calls
   `0x08002001` as a function. The application's `app_entry`
   runs, sets up its own MSP, performs the SYSCFG remap if
   needed, and takes over.

## STM32F0 specifics, called out

| Topic | F1 (Cortex-M3) | F0 (Cortex-M0) |
|---|---|---|
| Vector relocation | `NVIC_SetVectorTable` | none — SYSCFG remap of SRAM is the workaround, done by the application not the bootloader |
| `__set_MSP` from vector table | yes | no — the application sets its own MSP |
| First word of `APP_START_ADDR` | initial MSP value | first instruction of the application's entry function |
| Reset handler at `APP_START_ADDR + 4` | yes | no — the entry function is at `APP_START_ADDR` itself |
| GPIO alternate function | `GPIO_Mode_AF_PP` (mode encodes everything) | `GPIO_PinAFConfig` then `GPIO_Mode_AF` with separate `OType` / `PuPd` (modern model) |
| NVIC priority field | preempt + sub | one field, 0..3 (M0 has 2 priority bits) |
| `NVIC_PriorityGroupConfig` | available | does not exist on M0 |
| FPU | none | none (M0 has no FP extension) |

The library doesn't see any of this — every chip-specific
quirk is contained in `hooks.c` and the init functions in
`main.c`.

## Adapting to other STM32F0 / G0 parts

Most of the changes live in `armbl_app_layout.h`:

| Part | Differences |
|---|---|
| STM32F042 (16 / 32 KB) | Halve `APP_END_ADDR`. Same page size. Same UART. |
| STM32F072 (128 KB)     | Push `APP_END_ADDR` to `0x0801FBFF`. Page size becomes `0x800` (2 KiB). |
| STM32F091 (256 KB)     | `APP_END_ADDR` to `0x0803FBFF`, page size `0x800`. |
| STM32G0xx              | StdPeriph is replaced by HAL/LL on the G0 family — rewrite `hooks.c` and the init functions against LL or HAL, but the engine and the `jump_to_app` shape stay the same. G0 *does* have VTOR, so you can switch back to the M3/M4 jump style if you want. |

## License

[MIT](../../LICENSE).

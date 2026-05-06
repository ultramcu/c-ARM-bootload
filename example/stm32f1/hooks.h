/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MaIII Themd
 *
 * STM32F1 hook implementations for the armbl engine. These wrap
 * StdPeriph FLASH_* and USART_* calls into the five callbacks the
 * library expects.
 */

#ifndef HOOKS_H
#define HOOKS_H

#include <stddef.h>
#include <stdint.h>

#include "../../armbl.h"

#ifdef __cplusplus
extern "C" {
#endif

armbl_flash_result hooks_flash_erase(uint32_t start_addr, uint32_t end_addr);
armbl_flash_result hooks_flash_write(uint32_t addr, uint32_t word);
uint32_t           hooks_flash_read(uint32_t addr);
void               hooks_write_line(const char *line, size_t len);
void               hooks_jump_to_app(uint32_t app_start_addr);

#ifdef __cplusplus
}
#endif

#endif /* HOOKS_H */

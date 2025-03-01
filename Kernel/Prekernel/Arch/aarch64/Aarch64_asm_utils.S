/*
 * Copyright (c) 2021, Nico Weber <thakis@chromium.org>
 * Copyright (c) 2021, Marcin Undak <mcinek@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

.global wait_cycles
.type wait_cycles, @function
wait_cycles:
Lstart:
  // This is probably too fast when caching and branch prediction is turned on.
  // FIXME: Make timer-based.
  subs x0, x0, #1
  bne Lstart
  ret

.global enter_el2_from_el3
.type enter_el2_from_el3, @function
enter_el2_from_el3:
  adr x0, entered_el2
  msr elr_el3, x0
  eret
entered_el2:
  ret

.global enter_el1_from_el2
.type enter_el1_from_el2, @function
enter_el1_from_el2:
  adr x0, entered_el1
  msr elr_el2, x0
  eret
entered_el1:
  ret

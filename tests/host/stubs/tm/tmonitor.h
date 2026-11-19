/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HOST-TEST STUB of <tm/tmonitor.h>: only tm_printf, implemented by the
 * test file (it captures the formatted line instead of driving a UART).
 */
#ifndef HOST_STUB_TMONITOR_H
#define HOST_STUB_TMONITOR_H

#include "tk/tkernel.h"

INT tm_printf(const UB *format, ...);

#endif /* HOST_STUB_TMONITOR_H */

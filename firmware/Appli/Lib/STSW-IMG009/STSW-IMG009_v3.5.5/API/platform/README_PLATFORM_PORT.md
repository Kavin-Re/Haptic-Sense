# The platform layer is implemented elsewhere — do not restore the stub

`vl53l1_platform.c` has been renamed to **`vl53l1_platform.c.ST_STUB_UNUSED`** so CubeIDE
stops compiling it. It was ST's unfilled template: all nine bodies `return 255`, and it was
being linked into the binary as dead code.

**The nine platform functions now live in
`firmware/Appli/Core/Src/app_vl53l1_port.c`.**

## Why a separate file instead of filling this one in

Licensing. This directory is ST SLA and every file here carries ST's copyright header. Our
code is Apache-2.0 with an SPDX line per file. CLAUDE.md §7: *"Never mix headers."* Filling
ST's template in place would have put our implementation under their header.

`vl53l1_platform.h` is untouched and still included by our file. That is ordinary interface
use — the same way `app_i2c.c` includes `stm32n6xx_hal.h`.

## If the build breaks with "undefined reference to VL53L1_WrByte"

`app_vl53l1_port.c` is not being compiled. Check it is present in `Core/Src/` and refresh the
project in CubeIDE (**F5**) so `subdir.mk` is regenerated.

## If the build breaks with "multiple definition of VL53L1_WrByte"

Something restored this stub into the build. Rename it back out; do not delete our file.

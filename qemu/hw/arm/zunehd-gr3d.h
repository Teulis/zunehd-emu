/*
 * Zune HD (Tegra AP16) 3D engine, host1x class 0x60 ("AR20", same design as Tegra 2 gr3d).
 * This code is licensed under the GNU GPL v2 or later.
 */
#ifndef HW_ARM_ZUNEHD_GR3D_H
#define HW_ARM_ZUNEHD_GR3D_H

#include <stdint.h>

/* One method write in class 0x60. Draws execute synchronously on 0x123. */
void zune_gr3d_method(uint32_t off, uint32_t val);
uint32_t zune_gr3d_reg_read(uint32_t off);

#endif

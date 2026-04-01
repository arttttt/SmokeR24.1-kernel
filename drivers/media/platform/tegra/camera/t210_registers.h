/*
 * drivers/media/platform/tegra/camera/t210_registers.h
 *
 * Tegra T210-specific VI/CSI register definitions
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef __T210_REGISTERS_H__
#define __T210_REGISTERS_H__

/*
 * T210 syncpt event formulas - these use a linear mapping based on port number.
 * Do NOT use these on T124 - T124 uses hardcoded values that differ from
 * these formulas.
 */
#define T210_VI_CSI_PP_LINE_START(port)     (4 + (port) * 4)
#define T210_VI_CSI_PP_FRAME_START(port)    (5 + (port) * 4)
#define T210_VI_CSI_MW_REQ_DONE(port)       (6 + (port) * 4)
#define T210_VI_CSI_MW_ACK_DONE(port)       (7 + (port) * 4)

/*
 * T210 CIL PHY control default value
 */
#define T210_CIL_PHY_CONTROL_DEFAULT        0xA

#endif /* __T210_REGISTERS_H__ */

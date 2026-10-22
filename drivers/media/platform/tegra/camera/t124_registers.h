/*
 * drivers/media/platform/tegra/camera/t124_registers.h
 *
 * Tegra T124-specific VI/CSI absolute register offsets
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef __T124_REGISTERS_H__
#define __T124_REGISTERS_H__

/*
 * T124 Syncpt Events (hardcoded values, NOT formulas like T210)
 * These differ from T210's linear mapping - T124 has unique event numbers.
 */
#define T124_PPA_FRAME_START    9
#define T124_PPB_FRAME_START    10
#define T124_MWA_ACK_DONE       6
#define T124_MWB_ACK_DONE       7

/*
 * T124 Pixel Parser A absolute offsets (from VI base)
 * Base: 0x838
 */
#define T124_PP_A_INPUT_STREAM_CONTROL          0x838
#define T124_PP_A_PIXEL_STREAM_CONTROL0         0x83C
#define T124_PP_A_PIXEL_STREAM_CONTROL1         0x840
#define T124_PP_A_PIXEL_STREAM_GAP              0x844
#define T124_PP_A_PIXEL_STREAM_PP_COMMAND       0x848
#define T124_PP_A_PIXEL_STREAM_EXPECTED_FRAME   0x84C
#define T124_PP_A_PIXEL_STREAM_PP_INT_MASK      0x850
#define T124_PP_A_PIXEL_PARSER_STATUS           0x854
#define T124_PP_A_CSI_SW_SENSOR_RESET           0x858

/*
 * T124 Pixel Parser B absolute offsets (from VI base)
 * Base: 0x86C
 */
#define T124_PP_B_INPUT_STREAM_CONTROL          0x86C
#define T124_PP_B_PIXEL_STREAM_CONTROL0         0x870
#define T124_PP_B_PIXEL_STREAM_CONTROL1         0x874
#define T124_PP_B_PIXEL_STREAM_GAP              0x878
#define T124_PP_B_PIXEL_STREAM_PP_COMMAND       0x87C
#define T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME   0x880
#define T124_PP_B_PIXEL_STREAM_PP_INT_MASK      0x884
#define T124_PP_B_PIXEL_PARSER_STATUS           0x888
#define T124_PP_B_CSI_SW_SENSOR_RESET           0x88C

/*
 * T124 CIL PHY control default value (R21.5 stock)
 */
#define T124_CIL_PHY_CONTROL_DEFAULT            0x09

/*
 * T124 CSI PHY CIL command (absolute from VI base)
 */
#define T124_CSI_PHY_CIL_COMMAND                0x908

/* PHY_CIL_COMMAND bitmasks for port enable/disable */
#define T124_CIL_A_ENABLE                       0x0001
#define T124_CIL_B_ENABLE                       0x0100
#define T124_CIL_AB_4LANE                       (T124_CIL_A_ENABLE | T124_CIL_B_ENABLE)
#define T124_CIL_A_2LANE                        0x0201
#define T124_CIL_C_1LANE                        0x12020000
#define T124_CIL_C_4LANE                        0x21010000
#define T124_CIL_C_2LANE                        0x22010000
#define T124_CIL_ALL_ENABLE                     0x22020202
#define T124_CIL_CMD_LO_MASK                    0x0000FFFF
#define T124_CIL_CMD_HI_MASK                    0xFFFF0000

/*
 * T124 CSI error bits (TRM 26.16) that cannot be corrected
 */
/* CSI_PIXEL_PARSER_A/B_STATUS: uncorrectable packet header error */
#define T124_PP_HPA_UNC_HDR_ERR                 0x4000
/* CSI_CSI_CIL_A..E_STATUS: start of transmission multi bit error */
#define T124_CIL_SOT_MB_ERR                     0x02
/* CSI_CSI_CILA_STATUS..: data lane 0 and 1 SOT multi bit errors */
#define T124_CIL_DATA_LANE_ERR                  0x00020020

/*
 * T124 CSI PP frame gap
 */
#define T124_PP_FRAME_MIN_GAP                   0x14

/*
 * T124 CIL A absolute offsets (from VI base)
 */
#define T124_CILA_PAD_CONFIG0                   0x92C
#define T124_PHY_CILA_CONTROL0                  0x934
#define T124_CSI_CIL_A_STATUS                   0x93C
#define T124_CSI_CILA_STATUS                    0x940
#define T124_CSI_CIL_A_INT_MASK                 0x938
#define T124_CSICIL_SW_SENSOR_A_RESET           0x94C

/*
 * T124 CIL B absolute offsets (from VI base)
 */
#define T124_CILB_PAD_CONFIG0                   0x960
#define T124_PHY_CILB_CONTROL0                  0x968
#define T124_CSI_CIL_B_STATUS                   0x970
#define T124_CSI_CILB_STATUS                    0x974
#define T124_CSI_CIL_B_INT_MASK                 0x96C

/*
 * T124 CIL C absolute offsets (from VI base)
 */
#define T124_CILC_PAD_CONFIG0                   0x994
#define T124_PHY_CILC_CONTROL0                  0x99C
#define T124_CSI_CIL_C_STATUS                   0x9A4
#define T124_CSI_CILC_STATUS                    0x9A8
#define T124_CSI_CIL_C_INT_MASK                 0x9A0

/*
 * T124 CIL D absolute offsets (from VI base)
 */
#define T124_CILD_PAD_CONFIG0                   0x9C8
#define T124_PHY_CILD_CONTROL0                  0x9D0
#define T124_CSI_CIL_D_STATUS                   0x9D8
#define T124_CSI_CILD_STATUS                    0x9DC
#define T124_CSI_CIL_D_INT_MASK                 0x9D4

/*
 * T124 CIL E absolute offsets (from VI base)
 */
#define T124_CILE_PAD_CONFIG0                   0xA08
#define T124_PHY_CILE_CONTROL0                  0xA10
#define T124_CSI_CIL_E_INT_MASK                 0xA14
#define T124_CSI_CIL_E_STATUS                   0xA18
/* CIL E is a one-lane brick: its lane errors are in CIL_E_STATUS, and
 * 0xA1C is CIL_E_ESCAPE_MODE_COMMAND, not a lane status. */
#define T124_CSICIL_SW_SENSOR_E_RESET           0xA24

/*
 * T124 CSI Test Pattern Generator (absolute from VI base)
 */
#define T124_CSI_PG_CTRL_A                      0xA68
#define T124_CSI_PG_CTRL_B                      0xA9C

/*
 * T124 CSI misc registers (absolute from VI base, TRM 26.16.85-92)
 */
#define T124_CSI_STALL_COUNTER                  0xAE8
#define T124_CSI_READONLY_STATUS                0xAEC
#define T124_CSI_SW_STATUS_RESET                0xAF0
#define T124_CSI_CLKEN_OVERRIDE                 0xAF4
#define T124_CSI_DEBUG_CONTROL                  0xAF8
#define T124_CSI_DEBUG_COUNTER(n)               (0xAFC + 4 * (n))

/*
 * CSI_DEBUG_CONTROL: one set of three counters for the CSI, each
 * counting the event its select field names, of either pixel parser.
 * DEBUG_EN powers them; CLR_DBG_CNT_n (bits 5-7) clears one, and is
 * never set here -- a reader takes differences.
 */
#define T124_CSI_DEBUG_EN                       (1 << 0)
#define T124_CSI_DBG_CNT_SEL(n, event)          ((event) << (8 + 8 * (n)))
#define T124_CSI_DBG_PPA_FRAME_STARTS           67
#define T124_CSI_DBG_PPB_FRAME_STARTS           73

/* Counter n counts pixel parser n's frame starts: the sensor's frames,
 * whether the VI captures them or not. */
#define T124_CSI_FRAME_COUNTER(pp)              T124_CSI_DEBUG_COUNTER(pp)
#define T124_CSI_DEBUG_FRAME_STARTS \
	(T124_CSI_DEBUG_EN | \
	 T124_CSI_DBG_CNT_SEL(0, T124_CSI_DBG_PPA_FRAME_STARTS) | \
	 T124_CSI_DBG_CNT_SEL(1, T124_CSI_DBG_PPB_FRAME_STARTS))

/*
 * T124 CIL offsets relative to csi->iomem[0] for csi_write/csi_read
 *
 * csi->iomem[0] points to TEGRA_CSI_PIXEL_PARSER_0_BASE (0x838 from VI base)
 * These offsets are calculated as: absolute_offset - 0x838
 */
#define T124_REL_CILC_PAD_CONFIG0               0x15C   /* 0x994 - 0x838 */
#define T124_REL_PHY_CILC_CONTROL0              0x164   /* 0x99C - 0x838 */
#define T124_REL_CILC_INT_MASK                  0x168   /* 0x9A0 - 0x838 */
#define T124_REL_CILD_PAD_CONFIG0               0x190   /* 0x9C8 - 0x838 */
#define T124_REL_PHY_CILD_CONTROL0              0x198   /* 0x9D0 - 0x838 */
#define T124_REL_CILD_INT_MASK                  0x19C   /* 0x9D4 - 0x838 */
#define T124_REL_CILE_PAD_CONFIG0               0x1D0   /* 0xA08 - 0x838 */
#define T124_REL_PHY_CILE_CONTROL0              0x1D8   /* 0xA10 - 0x838 */
#define T124_REL_CILE_INT_MASK                  0x1DC   /* 0xA14 - 0x838 */
#define T124_REL_CIL_E_STATUS                   0x1E0   /* 0xA18 - 0x838 */
#define T124_REL(abs)                           ((abs) - 0x838)

/*
 * T124 CSI-E DPD index override.
 * MC PORT_B(1) with 1 lane maps to physical CSIE (index 4),
 * not CSIB (index 1) which the default csi_port would select.
 */
#define T124_CSIE_DPD_IO_IDX                    4

#endif /* __T124_REGISTERS_H__ */

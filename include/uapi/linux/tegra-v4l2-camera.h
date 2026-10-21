/*
 * tegra-v4l2-camera.h - V4L2 controls of the tegra camera drivers
 *
 * Copyright (c) 2015-2016, NVIDIA Corporation.  All rights reserved.
 * Copyright (C) 2026 Artem Bambalov
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __UAPI_TEGRA_V4L2_CAMERA_H__
#define __UAPI_TEGRA_V4L2_CAMERA_H__

#include <linux/v4l2-controls.h>

/* The sensor drivers' and the VI channel's own controls. Userspace (the
 * camera HAL) takes them from here, through headers_install. */
#define V4L2_CID_TEGRA_CAMERA_BASE	(V4L2_CTRL_CLASS_CAMERA | 0x2000)

#define V4L2_CID_FRAME_LENGTH		(V4L2_CID_TEGRA_CAMERA_BASE+0)
#define V4L2_CID_COARSE_TIME		(V4L2_CID_TEGRA_CAMERA_BASE+1)
#define V4L2_CID_COARSE_TIME_SHORT	(V4L2_CID_TEGRA_CAMERA_BASE+2)
#define V4L2_CID_GROUP_HOLD		(V4L2_CID_TEGRA_CAMERA_BASE+3)
#define V4L2_CID_HDR_EN			(V4L2_CID_TEGRA_CAMERA_BASE+4)
#define V4L2_CID_EEPROM_DATA		(V4L2_CID_TEGRA_CAMERA_BASE+5)
#define V4L2_CID_OTP_DATA		(V4L2_CID_TEGRA_CAMERA_BASE+6)
#define V4L2_CID_FUSE_ID		(V4L2_CID_TEGRA_CAMERA_BASE+7)

#define V4L2_CID_TEGRA_CAMERA_LAST	(V4L2_CID_TEGRA_CAMERA_BASE+8)
#define V4L2_CID_VI_BYPASS_MODE		(V4L2_CID_TEGRA_CAMERA_BASE+9)
/* How long the VI channel waits for a frame to start or land, in µs.
 * Set by userspace, which knows the period of the frames in flight. */
#define V4L2_CID_VI_CAPTURE_TIMEOUT	(V4L2_CID_TEGRA_CAMERA_BASE+10)
/* The sensor's line time in the current mode at the mode's own line
 * length (the minimum of V4L2_CID_LINE_LENGTH), in ns, read-only: what
 * userspace converts exposure and frame length with. At another line
 * length the line time scales with it. */
#define V4L2_CID_LINE_TIME_NS		(V4L2_CID_TEGRA_CAMERA_BASE+11)
/* Line length in pixel clocks (HTS): from the mode's own up. Longer
 * lines make a longer line time, so an exposure longer than the
 * sensor's line counter holds at the mode's line time still fits.
 * Userspace sets it with the frame length; the exposure in µs is
 * counted in lines of the length in effect. */
#define V4L2_CID_LINE_LENGTH		(V4L2_CID_TEGRA_CAMERA_BASE+12)

#endif /* __UAPI_TEGRA_V4L2_CAMERA_H__ */

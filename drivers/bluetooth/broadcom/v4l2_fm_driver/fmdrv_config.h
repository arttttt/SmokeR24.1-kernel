/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.


 *  Copyright (C) 2009-2014 Broadcom Corporation
 */


/************************************************************************************
 *
 *  Filename:      fmdrv_config.h
 *
 *  Description:   Configuration file for V4L2 FM driver module.
 *  Configurations such as World region, Scan step, Audio mode, NFL will be set
 *  in this file as these params are not defined by the standard V4L2 driver
 *
 ***********************************************************************************/

#ifndef _FM_DRV_CONFIG_H
#define _FM_DRV_CONFIG_H

#include "fm_public.h"
#include "fmdrv_main.h"
#include <media/v4l2-common.h>

/*******************************************************************************
**  Constants & Macros
*******************************************************************************/

/* Set default World region */
#define DEF_V4L2_FM_WORLD_REGION FM_REGION_NA

/* Set default Audio mode.
 *
 * Was FM_AUTO_MODE (chip-side stereo<->mono auto-blend based on RSSI/SNR).
 * On mocha that produced an audible "loud for the first second, then drops
 * to steady-state quieter" transition each time the app powers up FM —
 * the chip starts at full separation, evaluates the signal and engages
 * the blend, attenuating the per-channel level in the process. FM_STEREO_MODE
 * pins the demodulator at manual stereo with no blend so the audio level is
 * constant from the first tuned frame. V4L2 has no separate constant for
 * forced stereo, so this is the only place to express it. */
#define DEF_V4L2_FM_AUDIO_MODE FM_STEREO_MODE

/* Set default Audio path */
#ifndef DEF_V4L2_FM_AUDIO_PATH
#define DEF_V4L2_FM_AUDIO_PATH FM_AUDIO_I2S
#endif

/*Make this TRUE if FM I2S audio to be routed over */
/*PCM lines in master mode */
#ifndef ROUTE_FM_I2S_SLAVE_TO_PCM_PINS
#define ROUTE_FM_I2S_SLAVE_TO_PCM_PINS FALSE
#endif

#ifndef ROUTE_BT_I2S_MASTER_TO_PCM_PINS
#define ROUTE_BT_I2S_MASTER_TO_PCM_PINS FALSE
#endif

/*Make this TRUE if FM I2S audio to be routed over */
/*PCM lines in slave mode */
#ifndef ROUTE_FM_I2S_MASTER_TO_PCM_PINS
#define ROUTE_FM_I2S_MASTER_TO_PCM_PINS FALSE
#endif

/*Never make both the above macros TRUE*/
#if (ROUTE_FM_I2S_SLAVE_TO_PCM_PINS) && (ROUTE_FM_I2S_MASTER_TO_PCM_PINS)
#error "I2S should be either master or slave"
#endif

/*Whenw e enable FM over PCM, audio path should be I2S*/
#if (ROUTE_FM_I2S_SLAVE_TO_PCM_PINS) || (ROUTE_FM_I2S_MASTER_TO_PCM_PINS)
#define DEF_V4L2_FM_AUDIO_PATH FM_AUDIO_I2S
#endif

/* FM driver debug flag. Set this to FALSE for Production release */
#ifndef V4L2_FM_DEBUG
#define V4L2_FM_DEBUG TRUE
#endif

/* FM enable delay time, default set to 300 millisecond */
#ifndef V4L2_FM_ENABLE_DELAY
#define V4L2_FM_ENABLE_DELAY             300
#endif

/* FM driver RDS debug flag. Set this to FALSE for Production release */
#define V4L2_RDS_DEBUG TRUE

/* Set default Noise Floor Estimation value */
#define DEF_V4L2_FM_NFE 93
#define DEF_V4L2_FM_SIGNAL_STRENGTH 105
#define DEF_V4L2_FM_RSSI 0x55 /* RSSI threshold value 85 dBm */

#endif


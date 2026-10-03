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

/* Set default World region: Europe -- 87.5-108 MHz, 50 us de-emphasis,
 * 100 kHz steps and RDS (not the North American RBDS), as broadcast in
 * Europe and Russia. North America de-emphasised with 75 us, dulling the
 * treble of every European station, and stepped by 200 kHz. */
#define DEF_V4L2_FM_WORLD_REGION FM_REGION_EUR

/* Set default Audio mode: auto-blend.
 *
 * The chip blends stereo towards mono as the signal weakens, keeping the
 * noise of the L-R subcarrier out -- what every broadcast receiver does, and
 * what V4L2's V4L2_TUNER_MODE_STEREO means ("stereo when the signal allows";
 * s_tuner maps it here too). Mono stays available as V4L2_TUNER_MODE_MONO.
 *
 * Forced stereo (FM_STEREO_MODE) was the default for a while, against a
 * level step heard just after power-up: louder for the first second, then
 * quieter as the blend engaged. Forcing stereo hid that at the cost of full
 * subcarrier noise on every weak station. The step is a start-up transient
 * to be dealt with as such -- audio held muted until the tune has settled --
 * not by giving up the blend. */
#define DEF_V4L2_FM_AUDIO_MODE FM_AUTO_MODE

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


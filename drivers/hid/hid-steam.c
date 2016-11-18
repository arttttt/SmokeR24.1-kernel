/*
 * HID driver for Valve Steam Controller
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Author: Martin Gao <marting@nvidia.com>
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
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/module.h>
#include <linux/debugfs.h>

#include "hid-steam.h"
#include "hid-ids.h"

#define REPORT_ID			(0x00)
#define STEAM_KEYMAP_SIZE		(23)
#define SHORT_MAX			(32767)
#define SHORT_MIN			(-32768)
#define ACCEL_X				(ABS_HAT1X)
#define ACCEL_Y				(ABS_HAT1Y)
#define ACCEL_Z				(ABS_HAT2X)
#define GYRO_X				(ABS_HAT2Y)
#define GYRO_Y				(ABS_HAT3X)
#define GYRO_Z				(ABS_HAT3Y)
#define GYRO_QUAT_W			(ABS_PRESSURE)
#define GYRO_QUAT_X			(ABS_DISTANCE)
#define GYRO_QUAT_Y			(ABS_TILT_X)
#define GYRO_QUAT_Z			(ABS_TILT_Y)
#define GAS_MIN				(0)
#define GAS_MAX				(0xff)
#define GAS_FUZZ			(4)
#define GAS_FLAT			(4)
#define BRAKE_MIN			(0)
#define BRAKE_MAX			(0xff)
#define BRAKE_FUZZ			(4)
#define BRAKE_FLAT			(4)
#define ABS_X_MIN			(SHORT_MIN)
#define ABS_X_MAX			(SHORT_MAX)
#define ABS_X_FUZZ			(0)
#define ABS_X_FLAT			(0)
#define ABS_Y_MIN			(SHORT_MIN)
#define ABS_Y_MAX			(SHORT_MAX)
#define ABS_Y_FUZZ			(0)
#define ABS_Y_FLAT			(0)
#define ABS_Z_MIN			(SHORT_MIN)
#define ABS_Z_MAX			(SHORT_MAX)
#define ABS_Z_FUZZ			(0)
#define ABS_Z_FLAT			(0)
#define ABS_RZ_MIN			(SHORT_MIN)
#define ABS_RZ_MAX			(SHORT_MAX)
#define ABS_RZ_FUZZ			(0)
#define ABS_RZ_FLAT			(0)
#define BYTE_TO_BIT(x)			(8*(x))
#define LTP_UP				(8)
#define LTP_RIGHT			(9)
#define LTP_LEFT			(10)
#define LTP_DOWN			(11)
#define LTP_CENTER			(17)
#define LTP_TOUCH			(19)
#define IS_LTP_TOUCHED(x)		(!!((x) & BIT(LTP_TOUCH)))
#define IS_LTP_NONCENTER_PRESSED(x) \
	(!!((x) & \
	    (BIT(LTP_UP) | BIT(LTP_RIGHT) | BIT(LTP_LEFT) | BIT(LTP_DOWN))))

#define LOCAL_TRACE			(0)
#define LTRACEF(str, x...) \
	do { \
		if (LOCAL_TRACE) { \
			pr_warn("%s:%d: " str, __func__, __LINE__, ## x); \
		} \
	} while (0)

static u16 steam_keymap[] = {
	BTN_TR2,	/* enum 0 STEAM_RIGHT_TRIGGER_BTN */
	BTN_TL2,	/* enum 1 STEAM_LEFT_TRIGGER_BTN */
	BTN_TR,		/* enum 2 STEAM_RIGHT_BUMPER */
	BTN_TL,		/* enum 3 STEAM_LEFT_BUMPER */
	BTN_Y,		/* enum 4 STEAM_BTN_Y */
	BTN_B,		/* enum 5 STEAM_BTN_B */
	BTN_X,		/* enum 6 STEAM_BTN_X */
	BTN_A,		/* enum 7 STEAM_BTN_A */
	KEY_UP,		/* enum 8 STEAM_LTP_UP */
	KEY_RIGHT,	/* enum 9 STEAM_LTP_RIGHT */
	KEY_LEFT,	/* enum 10 STEAM_LTP_LEFT */
	KEY_DOWN,	/* enum 11 STEAM_LTP_DOWN */
	KEY_BACK,	/* enum 12 STEAM_ICON_LEFT */
	KEY_POWER,	/* enum 13 STEAM_ICON */
	BTN_START,	/* enum 14 STEAM_ICON_RIGHT */
	KEY_VOLUMEDOWN,	/* enum 15 STEAM_BACKGRIP_LEFT */
	KEY_VOLUMEUP,	/* enum 16 STEAM_BACKGRIP_RIGHT */
	BTN_THUMBL,	/* enum 17 STEAM_LTP_PRESS */
	BTN_THUMBR,	/* enum 18 STEAM_RTP_PRESS */
	KEY_RESERVED,	/* enum 19 */
	KEY_RESERVED,	/* enum 20 */
	KEY_RESERVED,	/* enum 21 */
	BTN_A,		/* enum 22 STEAM_JOYSTICK_CENTER_BTN */
};

struct steam_data {
	struct hid_device *hdev;
	struct input_dev *sensor_input;
	struct dentry *steam_debugfs_root;
	struct work_struct steam_init_setup_work;
	u32 was_rtp_used;
};


static u8 steam_rdesc[] = {
	0x05, 0x01,		/*	USAGE_PAGE (Generic Desktop)	*/
	0x09, 0x05,		/*	USAGE (Game Pad)		*/
	0xa1, 0x01,		/*	COLLECTION (Application)	*/
	0xa1, 0x00,		/*	COLLECTION (Physical)		*/

	/*
	 * 8 bytes of paddings:
	 * - four bytes of paddings to skip report header
	 * - four bytes of padding to ignore packet number in u32 (4 bytes)
	 */
	0x05, 0x09,		/*	USAGE_PAGE (Button)		*/
	0x75, 0x08,		/*	REPORT_SIZE (8)			*/
	0x95, 0x08,		/*	REPORT_COUNT (8)		*/
	0x81, 0x01,		/*	INPUT (Cnst,Var,Abs)		*/

	/* offset: 8 bytes */
	0x05, 0x09,		/*	USAGE_PAGE (Button)		*/
	0x19, 0x01,		/*	USAGE_MINIMUM (Button 1)	*/
	0x29, 0x18,		/*	USAGE_MAXIMUM (Button 24)	*/
	0x15, 0x00,		/*	LOGICAL_MINIMUM (0)		*/
	0x25, 0x01,		/*	LOGICAL_MAXIMUM (1)		*/
	0x95, 0x18,		/*	REPORT_COUNT (24)		*/
	0x75, 0x01,		/*	REPORT_SIZE (1)			*/
	0x81, 0x02,		/*	INPUT (Data,Var,Abs)		*/

	/* offset: 11 bytes */
	0x05, 0x02,		/*	USAGE_PAGE (Simulation Controls)*/
	0x09, 0xc5,             /*	USAGE (Brake)			*/
	0x09, 0xc4,		/*	USAGE (Accelerator)		*/
	0x15, 0x00,		/*	LOGICAL_MINIMUM (0)		*/
	0x26, 0xff, 0x00,	/*	LOGICAL_MAXIMUM (255)		*/
	0x75, 0x08,		/*	REPORT_SIZE (8)			*/
	0x95, 0x02,		/*	REPORT_COUNT (2)		*/
	0x81, 0x02,		/*	INPUT (Data,Var,Abs)		*/

	/*
	 * 3 bytes of paddings
	 * offset: 13 bytes
	 */
	0x05, 0x09,		/*	USAGE_PAGE (Button)		*/
	0x75, 0x08,		/*	REPORT_SIZE (8)			*/
	0x95, 0x03,		/*	REPORT_COUNT (3)		*/
	0x81, 0x01,		/*	INPUT (Cnst,Var,Abs)		*/

	/* offset: 16 bytes */
	0x05, 0x01,		/*	USAGE_PAGE (Generic Desktop)	*/
	0x09, 0x30,		/*	USAGE (X)			*/
	0x09, 0x31,		/*	USAGE (Y)			*/
	0x09, 0x32,		/*	USAGE (Z)			*/
	0x09, 0x35,		/*	USAGE (Rz)			*/
	0x16, 0x00, 0x80,	/*	LOGICAL_MINIMUM (-32768)	*/
	0x26, 0xff, 0x7f,	/*	LOGICAL_MAXIMUM (32767)		*/
	0x75, 0x10,		/*	REPORT_SIZE (16)		*/
	0x95, 0x04,		/*	REPORT_COUNT (4)		*/
	0x81, 0x02,		/*	INPUT (Data,Var,Abs)		*/

	/*
	 * remaining 40 bytes paddings:
	 * - 2 bytes: trigger left in unsigned short
	 * - 2 bytes: trigger right in unsigned short
	 * - 3 * 2 bytes: accel x, y, z three axis data represented in short
	 * - 3 * 2 bytes: gyro x, y, z three axis data represented in short
	 * - 4 * 2 bytes: gyro quat w, x, y, z data represented in short
	 * - remainig: paddings etc, irrelevant data
	 */
	0x05, 0x09,		/*	USAGE_PAGE (Button)		*/
	0x75, 0x08,		/*	REPORT_SIZE (8)			*/
	0x95, 0x28,		/*	REPORT_COUNT (40)		*/
	0x81, 0x01,		/*	INPUT (Cnst,Var,Abs)		*/
	0xc0,			/*	END_COLLECTION			*/
	0xc0			/*	END_COLLECTION			*/
};

enum steam_controller_ops {
	INIT_SETTING,
	START_PAIRING,
	DONGLE_COMMIT,
	DONGLE_DISCONNECT,
	TURNOFF,
};

static int steam_ops(struct hid_device *hdev, int ops);
/* debug print buffer function */
static void pr_buffer(u8 *data, size_t len)
{
	char qwerty[128] = { 0 };
	size_t i, pos = 0;
	if (len > 43)
		len = 43;
	for (i = 0; i < len; i++) {
		if (i != 0 && i % 8 == 0)
			pos += sprintf(qwerty + pos, "\n");
		pos += sprintf(qwerty + pos, "%02x ", data[i]);
	}
	pos += sprintf(qwerty + pos, "\n");
	LTRACEF("buffer length: %d, [%s]", (int)len, qwerty);
}

/* debugfs */
static int steam_init_show(void *data, u64 *val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret = steam_ops(hdev, INIT_SETTING);

	*val = (u64)ret;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(steam_init_fops,
			steam_init_show,
			NULL, "%llu\n");

static int steam_issue_pairing_show(void *data, u64 *val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret = steam_ops(hdev, START_PAIRING);

	*val = (u64)ret;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(steam_issue_pairing_fops,
			steam_issue_pairing_show,
			NULL, "%llu\n");

static int steam_dongle_commit_show(void *data, u64 *val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret = steam_ops(hdev, DONGLE_COMMIT);

	*val = (u64)ret;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(steam_dongle_commit_fops,
			steam_dongle_commit_show,
			NULL, "%llu\n");

static int steam_dongle_disconnect_show(void *data, u64 *val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret = steam_ops(hdev, DONGLE_DISCONNECT);

	*val = (u64)ret;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(steam_dongle_disconnect_fops,
			steam_dongle_disconnect_show,
			NULL, "%llu\n");

static int steam_turnoff_show(void *data, u64 *val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret = steam_ops(hdev, TURNOFF);

	*val = (u64)ret;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(steam_turnoff_fops,
			steam_turnoff_show,
			NULL, "%llu\n");

static int steam_sound_show(void *data, u64 *val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret;
	unsigned char cmd[16] = {
		REPORT_ID, 0x8f, 0x07, 0x00, 0x5e, 0x01, 0x5e, 0x01,
		0xf4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	if (!hdev)
		return -EINVAL;

	ret = hdev->hid_output_raw_report(hdev, cmd, sizeof(cmd),
			HID_FEATURE_REPORT);
	LTRACEF("cmd ret: %d.\n", ret);

	*val = (u64)ret;
	return 0;
}

static int steam_sound_set(void *data, u64 val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret;

	unsigned char audio_index = (unsigned char)((int)val & 0xff);

	unsigned char cmd[16] = {
		REPORT_ID, PLAY_AUDIO,
		0x04, audio_index, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	if ((!hdev) || ((int)val > STEAM_AUDIO_TYPES))
		return -EINVAL;

	ret = hdev->hid_output_raw_report(hdev, cmd, sizeof(cmd),
			HID_FEATURE_REPORT);
	LTRACEF("cmd ret: %d, val: %d\n", ret, (int)audio_index);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(steam_sound_fops,
			steam_sound_show,
			steam_sound_set, "%llu\n");

static int steam_rtp_mode_show(void *data, u64 *val)
{
	*val = (u64)0;
	return 0;
}

static int steam_rtp_mode_set(void *data, u64 val)
{
	struct hid_device *hdev = (struct hid_device *)data;
	int ret;
	unsigned char rtp_mode = (unsigned char)((int)val & 0xff);
	unsigned char cmd[16] = {
		REPORT_ID, SET_SETTINGS_VALUES, 0x03,
		SETTING_RIGHT_TRACKPAD_MODE, rtp_mode, 0x00,
		0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	if ((!hdev) || ((int)val > RIGHT_TRACKPAD_MODE_MAX) ||
			((int)val < RIGHT_TRACKPAD_MODE_MIN))
		return -EINVAL;

	ret = hdev->hid_output_raw_report(hdev, cmd, sizeof(cmd),
			HID_FEATURE_REPORT);
	LTRACEF("cmd ret: %d, val: %d\n", ret, (int)rtp_mode);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(steam_rtp_mode_fops,
			steam_rtp_mode_show,
			steam_rtp_mode_set, "%llu\n");

static int steam_debugfs_init(struct hid_device *hdev)
{
	char root_dir[16] = { 0 };
	int ep = 0;
	struct usb_interface *intf;
	struct usb_host_interface *interface;
	struct steam_data *sdata;
	struct dentry *steam_debugfs_root;

	if (!hdev) {
		pr_err("%s: null hdev.\n", __func__);
		return -EINVAL;
	}

	sdata = hid_get_drvdata(hdev);
	intf = to_usb_interface(hdev->dev.parent);
	if (!intf || !sdata) {
		pr_err("%s: unexpected null pointer.\n", __func__);
		return -EINVAL;
	}

	interface = intf->cur_altsetting;
	if (!interface) {
		pr_err("%s: null usb host interface.\n", __func__);
		return -EINVAL;
	}

	if (interface->desc.bNumEndpoints != 1)
		return -EINVAL;

	ep = usb_endpoint_num(&interface->endpoint[0].desc);
	sprintf(root_dir, "steam_controller_ep%d", ep);
	steam_debugfs_root = debugfs_create_dir(root_dir, 0);
	if (!steam_debugfs_root) {
		sdata->steam_debugfs_root = NULL;
		return -ENOMEM;
	}

	if (!debugfs_create_file("init", 0444, steam_debugfs_root,
		(void *)hdev,
		&steam_init_fops)) {
		pr_err("%s: failed to create init debugfs node.\n",
				__func__);
		goto err_out;
	}

	if (!debugfs_create_file("pair", 0444, steam_debugfs_root,
		(void *)hdev,
		&steam_issue_pairing_fops)) {
		pr_err("%s: failed to create pair debugfs node.\n",
				__func__);
		goto err_out;
	}

	if (!debugfs_create_file("dongle_commit", 0444, steam_debugfs_root,
		(void *)hdev,
		&steam_dongle_commit_fops)) {
		pr_err("%s: failed to create dongle commit debugfs node.\n",
				__func__);
		goto err_out;
	}

	if (!debugfs_create_file("dongle_disconnect", 0444, steam_debugfs_root,
		(void *)hdev,
		&steam_dongle_disconnect_fops)) {
		pr_err("%s: fail to create dongle disconnect debugfs node.\n",
				__func__);
		goto err_out;
	}

	if (!debugfs_create_file("turnoff", 0444, steam_debugfs_root,
		(void *)hdev,
		&steam_turnoff_fops)) {
		pr_err("%s: failed to create turnoff debugfs node.\n",
				__func__);
		goto err_out;
	}

	if (!debugfs_create_file("sound", 0644, steam_debugfs_root,
		(void *)hdev,
		&steam_sound_fops)) {
		pr_err("%s: failed to create sound debugfs node.\n",
				__func__);
		goto err_out;
	}

	if (!debugfs_create_file("rtp_mode", 0644, steam_debugfs_root,
		(void *)hdev,
		&steam_rtp_mode_fops)) {
		pr_err("%s: failed to create rtp_mode debugfs node.\n",
				__func__);
		goto err_out;
	}

	sdata->steam_debugfs_root = steam_debugfs_root;
	return 0;

err_out:
	debugfs_remove_recursive(steam_debugfs_root);
	sdata->steam_debugfs_root = NULL;
	return -ENOMEM;
}

static void steam_init_setup_func(struct work_struct *work)
{
	struct steam_data *sdata = container_of(work,
			struct steam_data, steam_init_setup_work);
	if (sdata)
		steam_ops(sdata->hdev, INIT_SETTING);
	return;
}


static int steam_ops(struct hid_device *hdev, int ops)
{
	int ret = 0;
	struct steam_data *sdata;
	unsigned char clear_cmd[8] = {
		REPORT_ID, CLEAR_DIGITAL_MAPPINGS,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	unsigned char init_settings_cmd[32] = {
		REPORT_ID, SET_SETTINGS_VALUES, 0x12,
		SETTING_LEFT_TRACKPAD_MODE, 0x07, 0x00,
		SETTING_RIGHT_TRACKPAD_MODE, 0x00, 0x00,
		SETTING_SMOOTH_ABSOLUTE_MOUSE, 0x0f, 0x00,
		SETTING_LED_USER_BRIGHTNESS, 0x64, 0x00,
		SETTING_GYRO_MODE, 0x1c, 0x00,
		SETTING_SLEEP_INACTIVITY_TIMEOUT, 0x08, 0x07,
		0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	unsigned char enable_pairing[8] = {
		REPORT_ID, ENABLE_PAIRING, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00,
	};
	unsigned char dongle_commit[8] = {
		REPORT_ID, DONGLE_COMMIT_DEVICE, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00,
	};
	unsigned char dongle_disconnect[8] = {
		REPORT_ID, DONGLE_DISCONNECT_DEVICE, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00,
	};
	unsigned char cmd[8] = {
		REPORT_ID, TURN_OFF_CONTROLLER,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	if (!hdev)
		return -EINVAL;

	sdata = hid_get_drvdata(hdev);
	if (!sdata)
		return -EINVAL;

	switch (ops) {
	case INIT_SETTING:
		/*
		 * initialization sequence is needed for endpoint Valve
		 * (IN, GET/SET FEATURE).
		 * (1) For wired interface:
		 * There should be one such endpoind: EP 3
		 * (2) For wireless interface (via USB dongle):
		 * There should be four such endpoinds: EP 2-5
		 */
		ret = hdev->hid_output_raw_report(hdev, clear_cmd,
				sizeof(clear_cmd), HID_FEATURE_REPORT);
		LTRACEF("clear cmd ret: %d.\n", ret);

		ret = hdev->hid_output_raw_report(hdev, init_settings_cmd,
				sizeof(init_settings_cmd), HID_FEATURE_REPORT);
		LTRACEF("initialize settings cmd ret: %d.\n", ret);

		sdata->was_rtp_used = 0;
		break;
	case START_PAIRING:
		ret = hdev->hid_output_raw_report(hdev, enable_pairing,
				sizeof(enable_pairing), HID_FEATURE_REPORT);
		LTRACEF("enable pairing ret: %d.\n", ret);
		break;
	case DONGLE_COMMIT:
		ret = hdev->hid_output_raw_report(hdev, dongle_commit,
				sizeof(dongle_commit), HID_FEATURE_REPORT);
		LTRACEF("dongle commit ret: %d.\n", ret);
		break;
	case DONGLE_DISCONNECT:
		ret = hdev->hid_output_raw_report(hdev, dongle_disconnect,
				sizeof(dongle_disconnect), HID_FEATURE_REPORT);
		LTRACEF("dongle disconnect ret: %d.\n", ret);
		break;
	case TURNOFF:
		ret = hdev->hid_output_raw_report(hdev, cmd, sizeof(cmd),
				HID_FEATURE_REPORT);
		LTRACEF("turnoff cmd ret: %d.\n", ret);
		break;
	default:
		pr_err("%s: invalid ops: %d.\n", __func__, ops);
		return -EINVAL;
	}

	return ret;
}

static u32 data_to_val(u8 *data, u32 num_bytes)
{
	u32 ret = 0;
	int i = 0;

	if (num_bytes > sizeof(u32)) {
		pr_err("invalid usage of %s.\n", __func__);
		return -EINVAL;
	}

	for (i = 0; i < num_bytes/sizeof(u8); i++)
		ret |= data[i] << BYTE_TO_BIT(i);

	return ret;
}

static void negate_short_val(u8 *src, u8 *dest)
{
	short value;
	short negated_val;

	if (!src || !dest) {
		pr_err("%s: null argument.\n", __func__);
		return;
	}

	value = (short)data_to_val(src, sizeof(u16));

	if (value == SHORT_MIN)
		negated_val = SHORT_MAX;
	else if (value > 0)
		negated_val = 0 - value - 1;
	else
		negated_val = 0 - value;

	dest[0] = negated_val & 0xff;
	dest[1] = (negated_val >> 8) & 0xff;
	return;
}

static int parse_controller_state(struct hid_device *hdev,
		struct hid_report *report, u8 *data, u8 length)
{
	struct steam_data *sdata;
	u32 packetnum = 0;
	u32 buttons_data_low = 0;
	struct sensor_state *sensors;
	int ret = 0;
	short left_pad_y = 0;
	short right_pad_y = 0;

	sdata = hid_get_drvdata(hdev);
	if ((!hdev) || (!sdata) || (!data)) {
		pr_err("%s: invalid argument null pointer.\n", __func__);
		ret = -EINVAL;
		goto end;
	}

	if (length < REPORT_CONTROLLER_STATE_SIZE) {
		pr_err("%s: report insufficient length: %d.\n",
				__func__, length);
		ret = -EINVAL;
		goto end;
	}

	if (hdev != report->device) {
		pr_err("%s: unexpected device mismatch.\n", __func__);
		ret = -EINVAL;
		goto end;
	}

	if (list_empty(&report->list)) {
		pr_err("%s: empty input list.\n", __func__);
		ret = -ENODEV;
		goto end;
	}

	packetnum = (u32)data_to_val(
			data + REPORT_PACKETNUM_OFFSET, sizeof(u32));
	buttons_data_low = (u32)data_to_val(
			data + REPORT_BUTTONS_LOW_OFFSET, sizeof(u32));
	sensors = (struct sensor_state *)(data + REPORT_ACCEL_X_OFFSET);
	left_pad_y = (short)data_to_val(
			data + REPORT_LEFT_TP_Y_OFFSET, sizeof(u16));
	right_pad_y = (short)data_to_val(
			data + REPORT_RIGHT_TP_Y_OFFSET, sizeof(u16));

#ifdef SEND_SENSOR_DATA
	if (sdata->sensor_input == NULL) {
		pr_err("%s: sensor input null.\n", __func__);
		ret = -ENODEV;
		goto end;
	}
	input_report_abs(sdata->sensor_input, ACCEL_X, sensors->accel_x);
	input_report_abs(sdata->sensor_input, ACCEL_Y, sensors->accel_y);
	input_report_abs(sdata->sensor_input, ACCEL_Z, sensors->accel_z);

	input_report_abs(sdata->sensor_input, GYRO_X, sensors->gyro_x);
	input_report_abs(sdata->sensor_input, GYRO_Y, sensors->gyro_y);
	input_report_abs(sdata->sensor_input, GYRO_Z, sensors->gyro_z);

	input_report_abs(sdata->sensor_input, GYRO_QUAT_W,
			sensors->gyro_quat_w);
	input_report_abs(sdata->sensor_input, GYRO_QUAT_X,
			sensors->gyro_quat_x);
	input_report_abs(sdata->sensor_input, GYRO_QUAT_Y,
			sensors->gyro_quat_y);
	input_report_abs(sdata->sensor_input, GYRO_QUAT_Z,
			sensors->gyro_quat_z);
	input_sync(sdata->sensor_input);
#endif	/* SEND_SENSOR_DATA */

	/* allow hid framework to handle the work */
	if (IS_LTP_NONCENTER_PRESSED(buttons_data_low))
		data[REPORT_BUTTONS_LOW_OFFSET +
			LTP_CENTER / BYTE_TO_BIT(sizeof(u8))] &=
			~BIT(LTP_CENTER % BYTE_TO_BIT(sizeof(u8)));

	if (!IS_LTP_TOUCHED(buttons_data_low)) {
		/* need to negate y axis value */
		negate_short_val(data + REPORT_LEFT_TP_Y_OFFSET,
				data + REPORT_LEFT_TP_Y_OFFSET);
	} else {
		/*
		 * need to zeros abs_x and abs_y values if generated via
		 * touch events of left touchpad
		 */
		data[REPORT_LEFT_TP_X_OFFSET] = 0x00;
		data[REPORT_LEFT_TP_X_OFFSET + 1] = 0x00;
		data[REPORT_LEFT_TP_Y_OFFSET] = 0x00;
		data[REPORT_LEFT_TP_Y_OFFSET + 1] = 0x00;
	}

	negate_short_val(data + REPORT_RIGHT_TP_Y_OFFSET,
			data + REPORT_RIGHT_TP_Y_OFFSET);

end:
	return ret;
}

static int parse_controller_wireless(struct hid_device *hdev,
	u8 *data, u8 length)
{
	struct sc_wl_event *wl_event =
		(struct sc_wl_event *)(data + REPORT_PAYLOAD_OFFSET);
	unsigned char event_type;
	int ep = -1;
	struct usb_interface *intf;
	struct usb_host_interface *interface;
	u8 num_eps;
	struct steam_data *sdata;

	if ((!hdev) || (!wl_event)) {
		pr_err("%s: invalid null argument.\n", __func__);
		return -EINVAL;
	}

	sdata = hid_get_drvdata(hdev);
	intf = to_usb_interface(hdev->dev.parent);
	if (!intf) {
		pr_err("%s: null usb interface.\n", __func__);
		return -EINVAL;
	}

	interface = intf->cur_altsetting;
	if (!interface) {
		pr_err("%s: null usb host interface.\n", __func__);
		return -EINVAL;
	}

	num_eps = interface->desc.bNumEndpoints;
	if (num_eps != 1) {
		pr_err("%s: interface desc has %d endpoint?\n",
				__func__, (int)num_eps);
		return -EINVAL;
	}

	if (num_eps > 0)
		ep = usb_endpoint_num(&interface->endpoint[0].desc);

	event_type = wl_event->wl_event_type;
	switch (event_type) {
	case WIRELESS_EVENT_DISCONNECT:
		pr_info("%s: ep %d disconnect.\n", __func__, ep);
		break;
	case WIRELESS_EVENT_CONNECT:
		pr_info("%s: ep %d connect.\n", __func__, ep);
		schedule_work(&sdata->steam_init_setup_work);
		break;
	case WIRELESS_EVENT_PAIR:
		pr_info("%s: ep %d pair.\n", __func__, ep);
		break;
	default:
		pr_err("%s: unknown wireless event type.\n", __func__);
		pr_buffer(data, length);
		return -EINVAL;
	}

	return 1;
}

static int parse_controller_status(u8 *data, u8 length)
{
	/* Current packet number at offset 0 */
	unsigned int packetnum;
	/* Event codes at offset 4 */
	unsigned short event_code;
	/* State information at offset 6 */
	unsigned short state_flags;
	/* Current battery voltage (mV) at offset 8 */
	unsigned short battery_voltage;
	/* Current battery level (0-100) at offset 10 */
	unsigned char battery_level;
	struct sc_status_event *status =
		(struct sc_status_event *)(data + REPORT_PAYLOAD_OFFSET);

	if (!status) {
		pr_err("%s: null data argument.\n", __func__);
		return -EINVAL;
	}

	packetnum = status->packetnum;
	event_code = status->event_code;
	state_flags = status->state_flags;
	battery_voltage = status->battery_voltage;
	battery_level = status->battery_level;

	return 1;
}

static int steam_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct steam_data *sdata = hid_get_drvdata(hdev);
	unsigned char uc_type = 0;
	unsigned char length = 0;

	if (!report) {
		pr_err("%s null report.\n", __func__);
		return -EINVAL;
	}

	if (report->id != REPORT_ID) {
		LTRACEF("bypassed. report id: %d.\n", report->id);
		return 0;
	}

	/*
	 * check report_version field in struct valve_in_report_header
	 * it should always be 1
	 */
	if (data[0] != 0x1 || data[1] != 0x00) {
		LTRACEF("bypassed. mismatch steam report header.\n");
		return 0;
	}

	if (!sdata) {
		pr_err("%s null steam data.\n", __func__);
		return -EINVAL;
	}

	uc_type = data[2];
	length = data[3];

	switch (uc_type) {
	case ID_CONTROLLER_STATE:
		return parse_controller_state(hdev, report, data, length);
	case ID_CONTROLLER_WIRELESS:
		return parse_controller_wireless(hdev, data, length);
	case ID_CONTROLLER_STATUS:
		return parse_controller_status(data, length);
	default:
		LTRACEF("Invalid uc_type: 0x%02X.\n", uc_type);
		return -EINVAL;
	}

	return 0;
}

static int steam_input_mapping(struct hid_device *hdev, struct hid_input *hi,
	struct hid_field *field, struct hid_usage *usage,
	unsigned long **bit, int *max)
{
	u32 key = 0;
	u32 keycode = 0;

	if (!usage) {
		pr_err("%s: usage null.\n", __func__);
		return -1;
	}

	if (!field) {
		pr_err("%s: field null.\n", __func__);
		return -1;
	}

	if (usage->collection_index != 1) {
		pr_err("%s: collection index mismatch: %d.\n",
				__func__, usage->collection_index);
		return -1;
	}

	key = usage->hid & HID_USAGE;

	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON) {
		if (key >= STEAM_KEYMAP_SIZE)
			return -1;

		/*
		 * Because key refers to BUTTON logical value defined
		 * report descriptor and starting from 1, need to subtract
		 * 1 in order to convert steam_keymap's index
		 */
		keycode = steam_keymap[key - 1];
		if (keycode == KEY_RESERVED)
			return -1;

		hid_map_usage_clear(hi, usage, bit, max, EV_KEY, keycode);
		LTRACEF("%s: hid usage: 0x%08X, key to keycode: %d->%d.\n",
				__func__, usage->hid, key, keycode);
		return 1;
	}

	/* Let hid-core decide for the others */
	return 0;
}

static __u8 *steam_report_fixup(struct hid_device *hdev, __u8 *rdesc,
		unsigned int *rsize)
{
	LTRACEF("%s: rsize: %d", __func__, *rsize);
	pr_buffer(rdesc, *rsize);
	*rsize = sizeof(steam_rdesc);
	return steam_rdesc;
}

static int steam_input_open(struct input_dev *dev)
{
	struct steam_data *sdata = input_get_drvdata(dev);
	int ret = 0;

	if (!sdata || !dev)
		return -EINVAL;

	ret = hid_hw_open(sdata->hdev);
	pr_info("%s: %s, ret: %d.\n", __func__, dev->name, ret);
	return ret;
}

static void steam_input_close(struct input_dev *dev)
{
	struct steam_data *sdata = input_get_drvdata(dev);

	if (!sdata || !dev)
		return;

	pr_info("%s: %s.\n", __func__, dev->name);
	hid_hw_close(sdata->hdev);
}

static struct steam_data *steam_create(struct hid_device *hdev)
{
	struct steam_data *sdata;

	sdata = kzalloc(sizeof(struct steam_data), GFP_KERNEL);
	if (!sdata)
		return NULL;

	sdata->hdev = hdev;

#ifdef SEND_SENSOR_DATA
	sdata->sensor_input = input_allocate_device();
	if (!sdata->sensor_input)
		goto err;

	input_set_drvdata(sdata->sensor_input, sdata);
	sdata->sensor_input->open = steam_input_open;
	sdata->sensor_input->close = steam_input_close;
	sdata->sensor_input->dev.parent = &hdev->dev;
	sdata->sensor_input->id.bustype = hdev->bus;
	sdata->sensor_input->id.vendor = hdev->vendor;
	sdata->sensor_input->id.product = hdev->product;
	sdata->sensor_input->id.version = hdev->version;
	sdata->sensor_input->name = "Valve Software Wired Controller Sensor";

	set_bit(EV_KEY, sdata->sensor_input->evbit);
	set_bit(EV_ABS, sdata->sensor_input->evbit);

	set_bit(ACCEL_X, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, ACCEL_X,
			SHORT_MIN, SHORT_MAX, 0, 0);
	set_bit(ACCEL_Y, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, ACCEL_Y,
			SHORT_MIN, SHORT_MAX, 0, 0);
	set_bit(ACCEL_Z, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, ACCEL_Z,
			SHORT_MIN, SHORT_MAX, 0, 0);

	set_bit(GYRO_X, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, GYRO_X,
			SHORT_MIN, SHORT_MAX, 0, 0);
	set_bit(GYRO_Y, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, GYRO_Y,
			SHORT_MIN, SHORT_MAX, 0, 0);
	set_bit(GYRO_Z, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, GYRO_Z,
			SHORT_MIN, SHORT_MAX, 0, 0);

	set_bit(GYRO_QUAT_W, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, GYRO_QUAT_W,
			SHORT_MIN, SHORT_MAX, 0, 0);
	set_bit(GYRO_QUAT_X, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, GYRO_QUAT_X,
			SHORT_MIN, SHORT_MAX, 0, 0);
	set_bit(GYRO_QUAT_Y, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, GYRO_QUAT_Y,
			SHORT_MIN, SHORT_MAX, 0, 0);
	set_bit(GYRO_QUAT_Z, sdata->sensor_input->absbit);
	input_set_abs_params(sdata->sensor_input, GYRO_QUAT_Z,
			SHORT_MIN, SHORT_MAX, 0, 0);
#else
	sdata->sensor_input = NULL;
#endif	/* SEND_SENSOR_DATA */

	/* init work struct, used for wireless initialization */
	INIT_WORK(&sdata->steam_init_setup_work,
			steam_init_setup_func);

	/* initialize private data values */
	sdata->was_rtp_used = 0;
	sdata->steam_debugfs_root = NULL;

	hid_set_drvdata(hdev, sdata);

	return sdata;

#ifdef SEND_SENSOR_DATA
err:
#endif	/* SEND_SENSOR_DATA */
	kfree(sdata);
	return NULL;
}


static int steam_input_configured(struct hid_device *hdev,
		struct hid_input *hi)

{
	struct input_dev *input;
	int i;

	if (!hi || !hi->input) {
		pr_err("%s: null hid input or input device.\n", __func__);
		return -EINVAL;
	}

	input = hi->input;
	LTRACEF("%s: input dev name: %s.\n", __func__, input->name);

	/* enable both button and axis */
	set_bit(EV_KEY, input->evbit);
	set_bit(EV_ABS, input->evbit);

	/* buttons */
	for (i = 0; i < STEAM_KEYMAP_SIZE; i++) {
		if (steam_keymap[i] != KEY_RESERVED)
			set_bit(steam_keymap[i], input->keybit);
	}

	/* triggers */
	set_bit(ABS_BRAKE, input->absbit);
	set_bit(ABS_GAS, input->absbit);
	input_set_abs_params(input, ABS_BRAKE,
			BRAKE_MIN, BRAKE_MAX, BRAKE_FUZZ, BRAKE_FLAT);
	input_set_abs_params(input, ABS_GAS,
			GAS_MIN, GAS_MAX, GAS_FUZZ, GAS_FLAT);

	/* left joystick */
	set_bit(ABS_X, input->absbit);
	set_bit(ABS_Y, input->absbit);
	input_set_abs_params(input, ABS_X,
			ABS_X_MIN, ABS_X_MAX,
			ABS_X_FUZZ, ABS_X_FLAT);
	input_set_abs_params(input, ABS_Y,
			ABS_Y_MIN, ABS_Y_MAX,
			ABS_Y_FUZZ, ABS_Y_FLAT);

	/* right trackpad */
	set_bit(ABS_Z, input->absbit);
	set_bit(ABS_RZ, input->absbit);
	input_set_abs_params(input, ABS_Z,
			ABS_Z_MIN, ABS_Z_MAX,
			ABS_Z_FUZZ, ABS_Z_FLAT);
	input_set_abs_params(input, ABS_RZ,
			ABS_RZ_MIN, ABS_RZ_MAX,
			ABS_RZ_FUZZ, ABS_RZ_FLAT);
	return 0;
}


static int steam_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret = 0;
	struct usb_interface *intf;
	struct usb_host_interface *interface;
	struct usb_interface_descriptor desc;
	struct steam_data *sdata;
#ifdef SEND_SENSOR_DATA
	int err_unregister_sensor_input = 0;
#endif	/* SEND_SENSOR_DATA */

	if (!hdev) {
		pr_err("%s: null hdev.\n", __func__);
		return -EINVAL;
	}

	intf = to_usb_interface(hdev->dev.parent);
	if (!intf) {
		pr_err("%s: null usb interface.\n", __func__);
		return -EINVAL;
	}

	interface = intf->cur_altsetting;
	if (!interface) {
		pr_err("%s: null usb host interface.\n", __func__);
		return -EINVAL;
	}

	desc = interface->desc;

	/*
	 * As Steam Controller is sending full report via control endpoint,
	 * if endpoint doesn't fit the criteria, simply return -ENODEV
	 *
	 * Based on USB HID class is the Device Class Definition for HID 1.11
	 * Subclass Code 0: no subclass
	 *	Other possible subclass codes
	 *	1 : boot interface subclass
	 *	2 - 255 : reserved
	 *
	 * Protocal Code 0: none
	 *	Other posssible protocol code
	 *	1 : Keyboard
	 *	2 : Mouse
	 *	3 - 255 : Reserved
	 */
	if ((desc.bInterfaceClass == USB_INTERFACE_CLASS_HID) &&
			(desc.bInterfaceSubClass == 0) &&
			(desc.bInterfaceProtocol == 0)) {
		sdata = steam_create(hdev);
		if (!sdata) {
			hid_err(hdev, "Can't alloc device.\n");
			return -ENOMEM;
		}

		ret = hid_parse(hdev);
		if (ret) {
			hid_err(hdev, "parse failed.\n");
			goto err_parse;
		}

		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret) {
			hid_err(hdev, "hw start failed.\n");
			goto err_start;
		}

#ifdef SEND_SENSOR_DATA
		ret = input_register_device(sdata->sensor_input);
		if (ret) {
			hid_err(hdev, "failed to register sensor input dev.\n");
			goto err_register_sensor_input;
		}
#endif	/* SEND_SENSOR_DATA */

		ret = steam_debugfs_init(hdev);
		if (ret) {
			hid_err(hdev, "debug fs failed to init.\n");
			goto err_debugfs;
		}

		steam_ops(sdata->hdev, INIT_SETTING);
		ret = hid_hw_open(hdev);
		if (ret < 0) {
			hid_err(hdev, "ll driver failed to open ret: %d.\n",
					ret);
			goto err_hid_open;
		}
	} else {
		hid_info(hdev, "%s: ignoreing ifnum %d.\n",
				__func__, desc.bInterfaceNumber);
		return -ENODEV;
	}

	hid_info(hdev, "probe success.\n");
	return 0;

err_hid_open:
	debugfs_remove_recursive(sdata->steam_debugfs_root);
err_debugfs:
#ifdef SEND_SENSOR_DATA
	input_unregister_device(sdata->sensor_input);
	err_unregister_sensor_input = 1;
err_register_sensor_input:
#endif	/* SEND_SENSOR_DATA */
	hid_hw_stop(hdev);
err_start:
err_parse:
#ifdef SEND_SENSOR_DATA
	if (err_unregister_sensor_input == 0)
		input_free_device(sdata->sensor_input);
#endif	/* SEND_SENSOR_DATA */
	kfree(sdata);
	hid_set_drvdata(hdev, NULL);
	return ret;
}

static void steam_remove(struct hid_device *hdev)
{
	struct steam_data *sdata = hid_get_drvdata(hdev);
	hid_info(hdev, "steam removed.\n");

	hid_hw_close(hdev);
	debugfs_remove_recursive(sdata->steam_debugfs_root);

#ifdef SEND_SENSOR_DATA
	if (sdata->sensor_input)
		input_unregister_device(sdata->sensor_input);
	else
		pr_err("%s: unexpected null sensor input.\n", __func__);
#endif	/* SEND_SENSOR_DATA */

	cancel_work_sync(&sdata->steam_init_setup_work);
	hid_hw_stop(hdev);
	kfree(sdata);
	hid_set_drvdata(hdev, NULL);

	return;
}

static const struct hid_device_id steam_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_VALVE,
			USB_DEVICE_ID_VALVE_STEAM_CONTROLLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_VALVE,
			USB_DEVICE_ID_VALVE_STEAM_DONGLE) },
	{ }
};
MODULE_DEVICE_TABLE(hid, steam_devices);

static struct hid_driver steam_driver = {
	.name = "hid-steam",
	.id_table = steam_devices,
	.input_configured = steam_input_configured,
	.input_mapping = steam_input_mapping,
	.report_fixup = steam_report_fixup,
	.raw_event = steam_raw_event,
	.probe = steam_probe,
	.remove = steam_remove,
};
module_hid_driver(steam_driver);

MODULE_LICENSE("GPLv2");

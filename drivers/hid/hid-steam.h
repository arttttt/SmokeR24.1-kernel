#ifndef __HID_VALVE_STEAM_CONTROLLER_H
#define __HID_VALVE_STEAM_CONTROLLER_H
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


/* report header uc_type */
enum valve_in_report_message_ids {
	/* Payload is struct sc_state */
	ID_CONTROLLER_STATE = 1,

	/* Payload is struct sc_wl_event */
	ID_CONTROLLER_WIRELESS = 3,

	/* Payload is struct sc_status_event */
	ID_CONTROLLER_STATUS = 4,
};

/* report header struct */
struct valve_in_report_header {
	/* Always 1 */
	unsigned short report_version;

	/* See valve_in_report_message_ids enum */
	unsigned char uc_type;

	/* Length of message (not including the header) */
	unsigned char length;
};

#define REPORT_PAYLOAD_OFFSET		(4)
#define REPORT_CONTROLLER_STATE_SIZE	(44)
#define REPORT_PACKETNUM_OFFSET		(REPORT_PAYLOAD_OFFSET)
#define REPORT_BUTTONS_LOW_OFFSET	(4 + REPORT_PACKETNUM_OFFSET)
#define REPORT_BUTTONS_HIGH_OFFSET	(4 + REPORT_BUTTONS_LOW_OFFSET)
#define REPORT_LEFT_TP_X_OFFSET		(4 + REPORT_BUTTONS_HIGH_OFFSET)
#define REPORT_LEFT_TP_Y_OFFSET		(2 + REPORT_LEFT_TP_X_OFFSET)
#define REPORT_RIGHT_TP_X_OFFSET	(2 + REPORT_LEFT_TP_Y_OFFSET)
#define REPORT_RIGHT_TP_Y_OFFSET	(2 + REPORT_RIGHT_TP_X_OFFSET)
#define REPORT_LEFT_TRIGGER_OFFSET	(2 + REPORT_RIGHT_TP_Y_OFFSET)
#define REPORT_RIGHT_TRIGGER_OFFSET	(2 + REPORT_LEFT_TRIGGER_OFFSET)
#define REPORT_ACCEL_X_OFFSET		(2 + REPORT_RIGHT_TRIGGER_OFFSET)
#define REPORT_ACCEL_Y_OFFSET		(2 + REPORT_ACCEL_X_OFFSET)
#define REPORT_ACCEL_Z_OFFSET		(2 + REPORT_ACCEL_Y_OFFSET)
#define REPORT_GYRO_X_OFFSET		(2 + REPORT_ACCEL_Z_OFFSET)
#define REPORT_GYRO_Y_OFFSET		(2 + REPORT_GYRO_X_OFFSET)
#define REPORT_GYRO_Z_OFFSET		(2 + REPORT_GYRO_Y_OFFSET)
#define REPORT_GYRO_QUAT_W_OFFSET	(2 + REPORT_GYRO_Z_OFFSET)
#define REPORT_GYRO_QUAT_X_OFFSET	(2 + REPORT_GYRO_QUAT_W_OFFSET)
#define REPORT_GYRO_QUAT_Y_OFFSET	(2 + REPORT_GYRO_QUAT_X_OFFSET)
#define REPORT_GYRO_QUAT_Z_OFFSET	(2 + REPORT_GYRO_QUAT_Y_OFFSET)

struct sensor_state {
	/* Scale is +-2G */
	short accel_x;
	short accel_y;
	short accel_z;

	/* Scale is +- 2000 DPS */
	short gyro_x;
	short gyro_y;
	short gyro_z;

	short gyro_quat_w;
	short gyro_quat_x;
	short gyro_quat_y;
	short gyro_quat_z;
};

/* report payload: controller_state */
struct sc_state {
	/* Current packet number */
	u32 packetnum;

	/* Button bitmask and trigger data */
	union {
		u64 buttons_data;
		struct {
			unsigned char pad0[3];
			unsigned char left_trigger_data;
			unsigned char right_trigger_data;
			unsigned char pad1[3];
		} triggers_data;
	} buttons_triggers_data;

	/* Left pad coordinates */
	short left_pad_x;
	short left_pad_y;

	/* Right pad coordinates */
	short right_pad_x;
	short right_pad_y;

	unsigned short trigger_left;
	unsigned short trigger_right;

	struct sensor_state sensors;
};


/* report payload: controller_wireless_event */
enum wireless_event_types {
	/* Controller has disconnected from a wireless receiver endpoint */
	WIRELESS_EVENT_DISCONNECT	= 1,

	/* Controller has connected or pairing committed */
	WIRELESS_EVENT_CONNECT		= 2,

	/* Controller has just paired. Pairing is not yet permanent */
	WIRELESS_EVENT_PAIR		= 3,
};

struct sc_wl_event {
	/* See wireless_event_types enum */
	unsigned char wl_event_type;
};


/* report payload: controller_status_event */
struct sc_status_event {
	/* Current packet number */
	unsigned int packetnum;

	/* Event codes and state information */
	unsigned short event_code;
	unsigned short state_flags;

	/* Current battery voltage (mV) */
	unsigned short battery_voltage;

	/* Current battery level (0-100) */
	unsigned char battery_level;
};

/* steam report struct */
struct valve_in_report {
	struct valve_in_report_header header;
	union {
		/* Controller state */
		struct sc_state controller_state;

		/* Wireless events (only from wireless receiver) */
		struct sc_wl_event controller_wireless_event;

		/* Periodic status and events */
		struct sc_status_event controller_status_event;
	} payload;
};


/* steam command message format for both command and response */
#define MAX_CMD_LENGTH			(64)
struct valve_command {
	/* Command opcode */
	unsigned char type;

	/* Length of payload */
	unsigned char length;

	/*
	 * Payload (size and structure will depend on command, some commands
	 * have no payload, while all responses contain a payload)
	 */
	unsigned char payload[MAX_CMD_LENGTH];
};


/*
 *
 * COMMAND OPCODE
 *
 */


/*
 * Description: Clears keyboard/mouse bindings
 * Payload:	No
 * Response:	No
 */
#define CLEAR_DIGITAL_MAPPINGS		(0x81)

/*
 * Description:	Retrieves controller information such as FW version, etc
 * Payload:	No
 * Response:	Yes
 */
#define GET_ATTRIBUTES_VALUES		(0x83)

/*
 * Description: Sets controller setting(s)
 * Payload:	Yes
 * Response:	No
 */
#define SET_SETTINGS_VALUES		(0x87)

/*
 * Description: Retrieves current setting(s)
 * Payload:	No
 * Response:	Yes
 */
#define GET_SETTINGS_VALUES		(0x89)

/*
 * Description: Resets settings to defaults
 * Payload:	No
 * Response:	No
 */
#define LOAD_DEFAULT_SETTINGS		(0x8E)

/*
 * Description: Trigger a haptic pulse train
 * Payload:	Yes
 * Response:	No
 */
#define TRIGGER_HAPTIC_PULSE		(0x8F)

/*
 * Description: Power off the controller (wireless only)
 * Payload:	No
 * Response:	No
 */
#define TURN_OFF_CONTROLLER		(0x9F)

/*
 * Description: Puts wireless receiver in pairing mode
 * Payload:	Yes
 * Response:	No
 */
#define ENABLE_PAIRING			(0xAD)

/*
 * Description: Retrieves controller string based information
 * Payload:	Yes
 * Response:	Yes
 */
#define GET_STRING_ATTRIBUTE		(0xAE)

/*
 * Description: Set a setting value on the wireless receiver
 * Payload:	Yes
 * Response:	No
 */
#define SET_DONGLE_SETTING		(0xB1)

/*
 * Description: Force a controller to disconnect from wireless receiver
 * Payload:	No
 * Response:	No
 */
#define DONGLE_DISCONNECT_DEVICE	(0xB2)

/*
 * Description: Makes new controller pairing permanent
 * Payload:	No
 * Response:	No
 */
#define DONGLE_COMMIT_DEVICE		(0xB3)

/*
 * Description: Play one of the preloaded audio files
 * Payload:	Yes
 * Response:	No
 */
#define PLAY_AUDIO			(0xB6)

/* Response Payload: GET_ATTRIBUTES_VALUES */
struct controller_attribute {
	unsigned char attribute_tag;
	unsigned long attribute_value;
};

struct get_attributes_values_payload {
	struct controller_attribute attributes[7];
};

/* Description: Deprecated */
#define ATTRIB_UNIQUE_ID			(0)
/* Description: Same as USB PID */
#define ATTRIB_PRODUCT_ID			(1)
/* Description: Bitmask describing controller capabilities (always 0x3) */
#define ATTRIB_CAPABILITIES			(2)
/* Description: Main FW version timestamp (epoch) */
#define ATTRIB_FIRMWARE_BUILD_TIME		(4)
/* Description: Radio FW version timestamp (epoch) */
#define ATTRIB_RADIO_FIRMWARE_BUILD_TIME	(5)
/* Description: HW revision */
#define ATTRIB_BOARD_REVISION			(9)
/* Description: Bootloader FW version timestamp (epoch) */
#define ATTRIB_BOOTLOADER_BUILD_TIME		(10)

/* Command and Response Payload: SET_SETTINGS_VALUES and GET_SETTINGS_VALUES */
struct controller_setting {
	unsigned char setting_num;
	unsigned short setting_value;
};

struct settings_values_payload {
	struct controller_setting settings[20];
};

/*
 * Default: 0
 * Value Range: [0, 8]
 * Description: Should be set to 7 on initialization
 */
#define SETTING_LEFT_TRACKPAD_MODE		(7)

/*
 * Default: 0
 * Value Range: [0, 8]
 * Description: Should be set to 7 on initialization
 */
#define SETTING_RIGHT_TRACKPAD_MODE		(8)
#define RIGHT_TRACKPAD_MODE_MAX			(8)
#define RIGHT_TRACKPAD_MODE_MIN			(0)

/*
 * Default: 15
 * Value Range: [0, 20]
 * Description: Controls the degree of filtering and smoothing of mouse inputs
 */
#define SETTING_SMOOTH_ABSOLUTE_MOUSE		(24)

/*
 * Default: 100
 * Value Range: [0, 100]
 * Description: Steam button LED brightness level
 */
#define SETTING_LED_USER_BRIGHTNESS		(45)

/*
 * Default: 0x0000
 * Value: Bitmaks
 * Description:
 * STEERING: 0x0001
 * TILT: 0x0002
 * Send joystick input based on controller orientation.
 *
 * QUATERNION: 0x0004
 * Send the orientation quaternion.
 *
 * RAW ACCEL: 0x0008
 * RAW GYRO: 0x0010
 * Send RAW acccel/gyro data
 */
#define SETTING_GYRO_MODE			(48)

/*
 * Default: 1800
 * Value Range: INT16_MAX
 * Description: Number of inactive seconds before powering off the controller
 */
#define SETTING_SLEEP_INACTIVITY_TIMEOUT	(50)

/* Command Payload: TRIGGER_HAPTIC_PULSE */
struct trigger_haptic_pulse_payload {
	/* 0 = right, 1 = left */
	unsigned char which_pad;

	/* Length of pulse in us */
	unsigned short pulse_duration;

	/* Time between pulses in us */
	unsigned short pulse_interval;

	/* Number of pulses */
	unsigned short pulse_count;

	/*
	 * Command priority:
	 * higher/equal priority will interrupt current pulse train
	 */
	unsigned char priority;
};

/* Command Payload: GET_STRING_ATTRIBUTE */
struct get_string_attribute_payload {
	unsigned char attribute_tag;
	char attribute_value[20];
};

/* Description: Unique board serial number */
#define ATTRIB_STR_BOARD_SERIAL			(0)
/* Description: Unique unit serial number. Matches label on controller */
#define ATTRIB_STR_UNIT_SERIAL			(1)


/* Command Payload: SET_DONGLE_SETTING */
struct set_dongle_setting_payload {
	unsigned char setting_id;
	unsigned char setting_value;
};

/*
 * Default: 1
 * Value Range: [0, 1]
 * Description: If 1 then dongle will forward keyboard/mouse events over EP 1
 */
#define MOUSE_KEYBOARD_ENABLED			(0)


/* Command Payload: PLAY_AUDIO */
struct play_audio_payload {
	unsigned int index;
};

/* Audio indices */
#define STEAM_AUDIO_TYPES			(6)

#define STARTUP_AUDIO				(0)
#define SHUTDOWN_AUDIO				(1)
#define PAIR_AUDIO				(2)
#define PAIR_SUCCESS_AUDIO			(3)
#define IDENTIFY_AUDIO				(4)
#define LIZARD_MODE_AUDIO			(5)
#define NORMAL_MODE_AUDIO			(6)

#endif


/*
 * This file holds the definitions of quirks found in USB devices.
 * Only quirks that affect the whole device, not an interface,
 * belong here.
 */

#ifndef __LINUX_USB_QUIRKS_H
#define __LINUX_USB_QUIRKS_H

/* string descriptors must not be fetched using a 255-byte read */
#define USB_QUIRK_STRING_FETCH_255	0x00000001

/* device can't resume correctly so reset it instead */
#define USB_QUIRK_RESET_RESUME		0x00000002

/* device can't handle Set-Interface requests */
#define USB_QUIRK_NO_SET_INTF		0x00000004

/* device can't handle its Configuration or Interface strings */
#define USB_QUIRK_CONFIG_INTF_STRINGS	0x00000008

/* device can't be reset(e.g morph devices), don't use reset */
#define USB_QUIRK_RESET			0x00000010

/* device has more interface descriptions than the bNumInterfaces count,
   and can't handle talking to these interfaces */
#define USB_QUIRK_HONOR_BNUMINTERFACES	0x00000020

/* device needs a pause during initialization, after we read the device
   descriptor */
#define USB_QUIRK_DELAY_INIT		0x00000040

/* device generates spurious wakeup, ignore remote wakeup capability */
#define USB_QUIRK_IGNORE_REMOTE_WAKEUP	0x00000200

#define USB_QUIRK_OTG_COMPLIANCE	0x00000080

/* device can handle u1/u2 power states well */
#define USB_QUIRK_ENABLE_U1U2		0x00000400

/* device can't handle Link Power Management */
#define USB_QUIRK_NO_LPM			BIT(10)

/* Downgrade SS device to USB2 mode */
#define USB_QUIRK_DOWNGRADE_USB3	BIT(12)

#endif /* __LINUX_USB_QUIRKS_H */

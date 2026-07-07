/*
 * USB teleport, client side: binds the SBC's eligible USB devices to the
 * usbip-host driver and makes sure usbipd serves them on TCP 3240 — the
 * sremfb server attaches them (vhci-hcd) while this client streams and
 * detaches them when it leaves.
 *
 * Policy (no configuration needed, overridable):
 *   - eligible by default: HID, mass storage and USB-serial devices;
 *   - never: hubs, any device an *active* network interface hangs off
 *     (a Pi 3's Ethernet is USB!), any device with a mounted filesystem
 *     or active swap on it;
 *   - SREMFB_USB_ALLOW / SREMFB_USB_DENY: comma-separated vendor[:product]
 *     hex ids, force-teleport / force-ignore (deny wins, guards always win);
 *   - SREMFB_USB=0/1 or --no-usb/--usb: master switch. Default: on,
 *     except on a Raspberry Pi 3 (off unless forced on).
 */
#ifndef SREMFB_USBEXPORT_H
#define SREMFB_USBEXPORT_H

/* mode: -1 auto, 0 off, 1 on. Returns 1 when the export is active. */
int  usb_export_init(int mode);
void usb_export_tick(void);   /* rescan (~2 s): bind new eligible devices */
void usb_export_stop(void);   /* unbind ours, give the devices back */

#endif /* SREMFB_USBEXPORT_H */

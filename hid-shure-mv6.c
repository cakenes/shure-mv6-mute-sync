// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/slab.h>

#define SHURE_VENDOR_ID         0x14ed
#define SHURE_MV6_PRODUCT       0x1026
/*
 * The mute button sends 6-byte interrupt reports on EP 4 IN (0x84),
 * which belongs to the AudioControl interface (1). usbhid never sees
 * these because it only polls the HID interface (0). We register as a
 * HID driver so we get probed reliably via the HID subsystem, then
 * submit our own URB directly to EP 0x84 to receive the button events.
 */
#define SHURE_MV6_EP_IN         0x84
#define SHURE_MV6_PKT_LEN       6
#define SHURE_MV6_INTERVAL      10  /* ms poll interval */

struct shure_mv6 {
struct hid_device   *hdev;
struct usb_device   *udev;
struct input_dev    *idev;
struct urb          *urb;
u8                  *buf;
dma_addr_t           buf_dma;
};

static void shure_mv6_urb_complete(struct urb *urb)
{
struct shure_mv6 *mv6 = urb->context;
u8 *data = mv6->buf;
int ret;

if (urb->status) {
if (urb->status != -ENOENT &&
    urb->status != -ECONNRESET &&
    urb->status != -ESHUTDOWN)
hid_dbg(mv6->hdev, "URB error: %d\n", urb->status);
return;
}

hid_dbg(mv6->hdev, "raw_event size=%d data=%*ph\n",
urb->actual_length, urb->actual_length, data);

if (urb->actual_length == SHURE_MV6_PKT_LEN &&
    data[0] == 0x00 &&
    data[1] == 0x01 &&
    data[2] == 0x00 &&
    data[3] == 0x01 &&
    data[4] == 0x01 &&
    data[5] == 0x02) {

input_report_key(mv6->idev, KEY_MICMUTE, 1);
input_sync(mv6->idev);
input_report_key(mv6->idev, KEY_MICMUTE, 0);
input_sync(mv6->idev);

hid_info(mv6->hdev, "emitted KEY_MICMUTE\n");
}

ret = usb_submit_urb(urb, GFP_ATOMIC);
if (ret)
hid_err(mv6->hdev, "resubmit URB failed: %d\n", ret);
}

static int shure_mv6_probe(struct hid_device *hdev,
   const struct hid_device_id *id)
{
struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
struct shure_mv6 *mv6;
int ret;

/* Only bind to the HID interface */
if (intf->cur_altsetting->desc.bInterfaceNumber != 0) {
hid_info(hdev, "skipping interface %d\n",
 intf->cur_altsetting->desc.bInterfaceNumber);
return -ENODEV;
}

mv6 = devm_kzalloc(&hdev->dev, sizeof(*mv6), GFP_KERNEL);
if (!mv6)
return -ENOMEM;

mv6->hdev = hdev;
mv6->udev = interface_to_usbdev(intf);
hid_set_drvdata(hdev, mv6);

ret = hid_parse(hdev);
if (ret) {
hid_err(hdev, "hid parse failed: %d\n", ret);
return ret;
}

ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
if (ret) {
hid_err(hdev, "hw start failed: %d\n", ret);
return ret;
}

/* Set up input device for the mute key */
mv6->idev = devm_input_allocate_device(&hdev->dev);
if (!mv6->idev) {
ret = -ENOMEM;
goto err_stop;
}

mv6->idev->name       = "Shure MV6 Mute";
mv6->idev->phys       = hdev->phys;
mv6->idev->id.bustype = BUS_USB;
mv6->idev->id.vendor  = SHURE_VENDOR_ID;
mv6->idev->id.product = SHURE_MV6_PRODUCT;

__set_bit(EV_KEY, mv6->idev->evbit);
__set_bit(KEY_MICMUTE, mv6->idev->keybit);

ret = input_register_device(mv6->idev);
if (ret)
goto err_stop;

/* Allocate URB to poll EP 4 IN on the AudioControl interface */
mv6->buf = usb_alloc_coherent(mv6->udev, SHURE_MV6_PKT_LEN,
      GFP_KERNEL, &mv6->buf_dma);
if (!mv6->buf) {
ret = -ENOMEM;
goto err_stop;
}

mv6->urb = usb_alloc_urb(0, GFP_KERNEL);
if (!mv6->urb) {
ret = -ENOMEM;
goto err_free_buf;
}

usb_fill_int_urb(mv6->urb, mv6->udev,
 usb_rcvintpipe(mv6->udev, SHURE_MV6_EP_IN),
 mv6->buf, SHURE_MV6_PKT_LEN,
 shure_mv6_urb_complete, mv6,
 SHURE_MV6_INTERVAL);
mv6->urb->transfer_dma   = mv6->buf_dma;
mv6->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

ret = usb_submit_urb(mv6->urb, GFP_KERNEL);
if (ret) {
hid_err(hdev, "submit URB failed: %d\n", ret);
goto err_free_urb;
}

hid_info(hdev, "Shure MV6 mute driver bound\n");
return 0;

err_free_urb:
usb_free_urb(mv6->urb);
err_free_buf:
usb_free_coherent(mv6->udev, SHURE_MV6_PKT_LEN, mv6->buf, mv6->buf_dma);
err_stop:
hid_hw_stop(hdev);
return ret;
}

static void shure_mv6_remove(struct hid_device *hdev)
{
struct shure_mv6 *mv6 = hid_get_drvdata(hdev);

usb_kill_urb(mv6->urb);
usb_free_urb(mv6->urb);
usb_free_coherent(mv6->udev, SHURE_MV6_PKT_LEN, mv6->buf, mv6->buf_dma);
hid_hw_stop(hdev);
hid_info(hdev, "Shure MV6 mute driver removed\n");
}

static const struct hid_device_id shure_mv6_devices[] = {
{ HID_USB_DEVICE(SHURE_VENDOR_ID, SHURE_MV6_PRODUCT) },
{ }
};
MODULE_DEVICE_TABLE(hid, shure_mv6_devices);

static struct hid_driver shure_mv6_driver = {
.name     = "hid-shure-mv6",
.id_table = shure_mv6_devices,
.probe    = shure_mv6_probe,
.remove   = shure_mv6_remove,
};

module_hid_driver(shure_mv6_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cakenes");
MODULE_DESCRIPTION("Shure MV6 mute key driver");

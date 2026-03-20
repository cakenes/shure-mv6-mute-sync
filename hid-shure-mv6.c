/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shure MV6 mute driver
 *
 * Exposes a sysfs "mute" attribute backed by UAC2 GET/SET CUR on
 * Feature Unit 2 (microphone path, AudioControl interface 1).
 * The hardware button and LED are managed by the device firmware.
 * Userspace polls this file to detect state changes and writes 0/1
 * to set the mute state remotely.
 */
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define SHURE_VENDOR_ID 0x14ed
#define SHURE_MV6_PRODUCT 0x1026

/* UAC2 CUR request for Feature Unit 2 (mic path) on AudioControl interface 1 */
#define SHURE_MV6_AC_IFACE 1
#define SHURE_MV6_MIC_FU_ID 2
#define UAC2_CUR 0x01
#define UAC2_CS_MUTE 0x01
#define UAC2_CN_MASTER 0x00

struct shure_mv6 {
  struct hid_device *hdev;
  struct usb_device *udev;
};

/* Read the current hardware mute state via UAC2 GET CUR. May sleep. */
static int shure_mv6_get_mute(struct shure_mv6 *mv6, u8 *out) {
  u8 *val;
  int ret;

  val = kmalloc(1, GFP_KERNEL);
  if (!val)
    return -ENOMEM;

  ret = usb_control_msg(mv6->udev, usb_rcvctrlpipe(mv6->udev, 0), UAC2_CUR,
                        USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_IN,
                        (UAC2_CS_MUTE << 8) | UAC2_CN_MASTER,
                        (SHURE_MV6_MIC_FU_ID << 8) | SHURE_MV6_AC_IFACE, val, 1,
                        USB_CTRL_GET_TIMEOUT);
  if (ret == 1) {
    *out = *val;
    ret = 0;
  } else {
    ret = ret < 0 ? ret : -EIO;
  }

  kfree(val);
  return ret;
}

/* Set the hardware mute state via UAC2 SET CUR. May sleep. */
static int shure_mv6_set_mute(struct shure_mv6 *mv6, u8 mute) {
  u8 *val;
  int ret;

  val = kmalloc(1, GFP_KERNEL);
  if (!val)
    return -ENOMEM;
  *val = !!mute;

  ret = usb_control_msg(mv6->udev, usb_sndctrlpipe(mv6->udev, 0), UAC2_CUR,
                        USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,
                        (UAC2_CS_MUTE << 8) | UAC2_CN_MASTER,
                        (SHURE_MV6_MIC_FU_ID << 8) | SHURE_MV6_AC_IFACE, val, 1,
                        USB_CTRL_SET_TIMEOUT);
  if (ret == 1)
    ret = 0;
  else
    ret = ret < 0 ? ret : -EIO;

  kfree(val);
  return ret;
}

static ssize_t mute_show(struct device *dev, struct device_attribute *attr,
                         char *buf) {
  struct shure_mv6 *mv6 = hid_get_drvdata(to_hid_device(dev));
  u8 val;

  if (shure_mv6_get_mute(mv6, &val))
    return -EIO;
  return sysfs_emit(buf, "%d\n", val);
}

/* sysfs: set hardware mute state */
static ssize_t mute_store(struct device *dev, struct device_attribute *attr,
                          const char *buf, size_t count) {
  struct shure_mv6 *mv6 = hid_get_drvdata(to_hid_device(dev));
  u8 val;
  int ret;

  if (kstrtou8(buf, 10, &val) || val > 1)
    return -EINVAL;

  ret = shure_mv6_set_mute(mv6, val);
  return ret ? ret : count;
}

static DEVICE_ATTR_RW(mute);

static int shure_mv6_probe(struct hid_device *hdev,
                           const struct hid_device_id *id) {
  struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
  struct shure_mv6 *mv6;
  int ret;

  /* Only bind to the HID interface */
  if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
    return -ENODEV;

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

  ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
  if (ret) {
    hid_err(hdev, "hw start failed: %d\n", ret);
    return ret;
  }

  ret = device_create_file(&hdev->dev, &dev_attr_mute);
  if (ret) {
    hid_err(hdev, "sysfs create failed: %d\n", ret);
    hid_hw_stop(hdev);
    return ret;
  }

  hid_info(hdev, "Shure MV6 mute driver bound\n");
  return 0;
}

static void shure_mv6_remove(struct hid_device *hdev) {
  device_remove_file(&hdev->dev, &dev_attr_mute);
  hid_hw_stop(hdev);
  hid_info(hdev, "Shure MV6 mute driver removed\n");
}

static const struct hid_device_id shure_mv6_devices[] = {
    {HID_USB_DEVICE(SHURE_VENDOR_ID, SHURE_MV6_PRODUCT)}, {}};
MODULE_DEVICE_TABLE(hid, shure_mv6_devices);

static struct hid_driver shure_mv6_driver = {
    .name = "hid-shure-mv6",
    .id_table = shure_mv6_devices,
    .probe = shure_mv6_probe,
    .remove = shure_mv6_remove,
};

module_hid_driver(shure_mv6_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cakenes");
MODULE_DESCRIPTION("Shure MV6 mute key driver");

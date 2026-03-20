/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define SHURE_VENDOR_ID 0x14ed
#define SHURE_MV6_PRODUCT 0x1026
/*
 * The mute button sends 6-byte interrupt reports on EP 4 IN (0x84),
 * which belongs to the AudioControl interface (1). usbhid never sees
 * these because it only polls the HID interface (0). We register as a
 * HID driver so we get probed reliably via the HID subsystem, then
 * submit our own URB directly to EP 0x84 to receive the button events.
 *
 * Button presses are forwarded as KEY_MICMUTE so PipeWire/WirePlumber
 * handles the mute toggle (including the hardware Feature Unit via ALSA).
 *
 * The sysfs "mute" attribute allows remote get/set via UAC2 SET/GET CUR
 * on Feature Unit 2 (microphone path) and emits SW_MUTE_DEVICE so the
 * desktop reflects the change without going through KEY_MICMUTE.
 */
#define SHURE_MV6_EP_IN 0x84
#define SHURE_MV6_PKT_LEN 6
#define SHURE_MV6_INTERVAL 10 /* ms poll interval */
#define SHURE_MV6_DEBOUNCE_MS 300

/*
 * UAC2 control request parameters for Feature Unit 2 (microphone path).
 * From lsusb: bUnitID=2, Mute Control read/write, on AudioControl interface 1.
 *
 * wValue = (CS << 8) | CN  where CS=Mute Control Selector (0x01), CN=master
 * (0x00) wIndex = (entityID << 8) | interface  where entityID=2, interface=1
 */
#define SHURE_MV6_AC_IFACE 1
#define SHURE_MV6_MIC_FU_ID 2
#define UAC2_CUR 0x01
#define UAC2_CS_MUTE 0x01
#define UAC2_CN_MASTER 0x00

struct shure_mv6 {
  struct hid_device *hdev;
  struct usb_device *udev;
  struct input_dev *idev;
  struct urb *urb;
  u8 *buf;
  dma_addr_t buf_dma;
  ktime_t last_button; /* debounce */
  struct work_struct mute_work;
  atomic_t mute; /* cached hardware mute state */
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

static void shure_mv6_mute_work(struct work_struct *work) {
  struct shure_mv6 *mv6 = container_of(work, struct shure_mv6, mute_work);
  u8 new_mute;

  /* Firmware already toggled the hardware state on button press.
   * Just read the new state and report it — no SET_CUR needed. */
  if (shure_mv6_get_mute(mv6, &new_mute))
    return;

  atomic_set(&mv6->mute, new_mute);
  input_report_switch(mv6->idev, SW_MUTE_DEVICE, new_mute);
  input_sync(mv6->idev);
}

static void shure_mv6_urb_complete(struct urb *urb) {
  struct shure_mv6 *mv6 = urb->context;
  u8 *data = mv6->buf;
  ktime_t now;
  int ret;

  if (urb->status) {
    if (urb->status != -ENOENT && urb->status != -ECONNRESET &&
        urb->status != -ESHUTDOWN)
      hid_dbg(mv6->hdev, "URB error: %d\n", urb->status);
    goto resubmit;
  }

  hid_dbg(mv6->hdev, "raw_event size=%d data=%*ph\n", urb->actual_length,
          urb->actual_length, data);

  if (urb->actual_length == SHURE_MV6_PKT_LEN && data[0] == 0x00 &&
      data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x01 &&
      data[4] == 0x01 && data[5] == 0x02) {
    now = ktime_get();
    if (ktime_ms_delta(now, mv6->last_button) >= SHURE_MV6_DEBOUNCE_MS) {
      mv6->last_button = now;
      schedule_work(&mv6->mute_work);
    }
  }

resubmit:
  ret = usb_submit_urb(urb, GFP_ATOMIC);
  if (ret)
    hid_err(mv6->hdev, "resubmit URB failed: %d\n", ret);
}

/* sysfs: live read of hardware mute state */
static ssize_t mute_show(struct device *dev, struct device_attribute *attr,
                         char *buf) {
  struct shure_mv6 *mv6 = hid_get_drvdata(to_hid_device(dev));
  u8 val;

  if (shure_mv6_get_mute(mv6, &val))
    return -EIO;
  return sysfs_emit(buf, "%d\n", val);
}

/* sysfs: set hardware mute and notify the desktop via SW_MUTE_DEVICE */
static ssize_t mute_store(struct device *dev, struct device_attribute *attr,
                          const char *buf, size_t count) {
  struct shure_mv6 *mv6 = hid_get_drvdata(to_hid_device(dev));
  u8 val;
  int ret;

  if (kstrtou8(buf, 10, &val) || val > 1)
    return -EINVAL;

  ret = shure_mv6_set_mute(mv6, val);
  if (ret)
    return ret;

  atomic_set(&mv6->mute, val);
  input_report_switch(mv6->idev, SW_MUTE_DEVICE, val);
  input_sync(mv6->idev);
  return count;
}

static DEVICE_ATTR_RW(mute);

static int shure_mv6_probe(struct hid_device *hdev,
                           const struct hid_device_id *id) {
  struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
  struct shure_mv6 *mv6;
  u8 init_mute = 0;
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

  ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
  if (ret) {
    hid_err(hdev, "hw start failed: %d\n", ret);
    return ret;
  }

  mv6->idev = devm_input_allocate_device(&hdev->dev);
  if (!mv6->idev) {
    ret = -ENOMEM;
    goto err_stop;
  }
  mv6->idev->name = "Shure MV6 Mute";
  mv6->idev->phys = hdev->phys;
  mv6->idev->id.bustype = BUS_USB;
  mv6->idev->id.vendor = SHURE_VENDOR_ID;
  mv6->idev->id.product = SHURE_MV6_PRODUCT;
  __set_bit(EV_KEY, mv6->idev->evbit);
  __set_bit(KEY_MICMUTE, mv6->idev->keybit);
  __set_bit(EV_SW, mv6->idev->evbit);
  __set_bit(SW_MUTE_DEVICE, mv6->idev->swbit);
  ret = input_register_device(mv6->idev);
  if (ret)
    goto err_stop;

  /* Report initial state so the desktop starts in sync. */
  if (shure_mv6_get_mute(mv6, &init_mute) == 0) {
    hid_info(hdev, "initial mute state: %s\n", init_mute ? "muted" : "unmuted");
    atomic_set(&mv6->mute, init_mute);
    input_report_switch(mv6->idev, SW_MUTE_DEVICE, init_mute);
    input_sync(mv6->idev);
  } else {
    hid_warn(hdev, "could not read initial mute state\n");
  }

  INIT_WORK(&mv6->mute_work, shure_mv6_mute_work);

  mv6->buf = usb_alloc_coherent(mv6->udev, SHURE_MV6_PKT_LEN, GFP_KERNEL,
                                &mv6->buf_dma);
  if (!mv6->buf) {
    ret = -ENOMEM;
    goto err_stop;
  }

  mv6->urb = usb_alloc_urb(0, GFP_KERNEL);
  if (!mv6->urb) {
    ret = -ENOMEM;
    goto err_free_buf;
  }

  usb_fill_int_urb(
      mv6->urb, mv6->udev, usb_rcvintpipe(mv6->udev, SHURE_MV6_EP_IN), mv6->buf,
      SHURE_MV6_PKT_LEN, shure_mv6_urb_complete, mv6, SHURE_MV6_INTERVAL);
  mv6->urb->transfer_dma = mv6->buf_dma;
  mv6->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

  ret = usb_submit_urb(mv6->urb, GFP_KERNEL);
  if (ret) {
    hid_err(hdev, "submit URB failed: %d\n", ret);
    goto err_free_urb;
  }

  ret = device_create_file(&hdev->dev, &dev_attr_mute);
  if (ret) {
    hid_err(hdev, "sysfs create failed: %d\n", ret);
    goto err_kill_urb;
  }

  hid_info(hdev, "Shure MV6 mute driver bound\n");
  return 0;

err_kill_urb:
  usb_kill_urb(mv6->urb);
err_free_urb:
  usb_free_urb(mv6->urb);
err_free_buf:
  usb_free_coherent(mv6->udev, SHURE_MV6_PKT_LEN, mv6->buf, mv6->buf_dma);
err_stop:
  hid_hw_stop(hdev);
  return ret;
}

static void shure_mv6_remove(struct hid_device *hdev) {
  struct shure_mv6 *mv6 = hid_get_drvdata(hdev);

  device_remove_file(&hdev->dev, &dev_attr_mute);
  cancel_work_sync(&mv6->mute_work);
  usb_kill_urb(mv6->urb);
  usb_free_urb(mv6->urb);
  usb_free_coherent(mv6->udev, SHURE_MV6_PKT_LEN, mv6->buf, mv6->buf_dma);
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

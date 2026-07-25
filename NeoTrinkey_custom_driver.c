#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#define VENDOR_ID  0x239a
#define PRODUCT_ID 0x80ff

#define CMD_SET_LED   0x01

/* Per-device state.
 * touch_state is atomic: it is written from raw_event(), which runs in
 * interrupt/softirq context (URB completion), so it must not sleep.
 * The mutex only protects led_store() and the disconnected flag.
 */
struct trinkey_dev {
    struct hid_device *hdev;
    struct mutex       lock;
    bool               disconnected;
    atomic_t           touch_state;
};


// SYSFS INTERFACE

// cat /sys/.../trinkey_touch  (last state received via interrupt)
static ssize_t trinkey_touch_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct hid_device  *hdev = to_hid_device(dev);
    struct trinkey_dev *tdev = hid_get_drvdata(hdev);
    u8 state;

    if (mutex_lock_interruptible(&tdev->lock))
        return -ERESTARTSYS;
    if (tdev->disconnected) {
        mutex_unlock(&tdev->lock);
        return -ENODEV;
    }
    state = (u8)atomic_read(&tdev->touch_state);
    mutex_unlock(&tdev->lock);

    return sysfs_emit(buf, "%d\n", state);
}

// echo "R G B" > /sys/.../trinkey_led
static ssize_t trinkey_led_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct hid_device   *hdev = to_hid_device(dev);
    struct trinkey_dev  *tdev = hid_get_drvdata(hdev);
    struct usb_interface *intf;
    struct usb_device   *udev;
    int r, g, b, retval;
    u8 *data;

    if (!hid_is_usb(hdev))
        return -ENODEV;

    if (sscanf(buf, "%d %d %d", &r, &g, &b) != 3)
        return -EINVAL;

    /* Explicit range check: without it, out-of-range values would be
     * silently truncated by the cast to u8.
     */
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
        return -EINVAL;

    data = kmalloc(3, GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data[0] = (u8)r;
    data[1] = (u8)g;
    data[2] = (u8)b;

    if (mutex_lock_interruptible(&tdev->lock)) {
        kfree(data);
        return -ERESTARTSYS;
    }
    if (tdev->disconnected) {
        mutex_unlock(&tdev->lock);
        kfree(data);
        return -ENODEV;
    }


    intf = to_usb_interface(hdev->dev.parent);
    udev = interface_to_usbdev(intf);
    retval = usb_control_msg_send(udev, 0, CMD_SET_LED,
                                  USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                                  0, 0, data, 3, 100, GFP_KERNEL);
    mutex_unlock(&tdev->lock);
    kfree(data);

    if (retval) {
        dev_err(dev, "LED write error: %d\n", retval);
        return retval;
    }

    return count;
}

static DEVICE_ATTR_RO(trinkey_touch);
static DEVICE_ATTR_WO(trinkey_led);

static struct attribute *trinkey_attrs[] = {
    &dev_attr_trinkey_touch.attr,
    &dev_attr_trinkey_led.attr,
    NULL,
};

static const struct attribute_group trinkey_group = {
    .attrs = trinkey_attrs,
};

/* struct hid_driver has no dev_groups field (unlike struct usb_driver),
 * so the sysfs group is created/removed explicitly in probe()/remove().
 */


// HID CORE CALLBACKS

/* Called by the HID core for every report received on the interrupt IN
 * endpoint. This runs in interrupt/softirq context (URB completion).
 */
static int trinkey_raw_event(struct hid_device *hdev, struct hid_report *report,
                             u8 *data, int size)
{
    struct trinkey_dev *tdev = hid_get_drvdata(hdev);

    if (size < 1)
        return 0;

    atomic_set(&tdev->touch_state, data[0]);

    // Wake up anyone doing poll() on the sysfs attribute.
    sysfs_notify(&hdev->dev.kobj, NULL, "trinkey_touch");

    return 0;
}

static int trinkey_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    struct trinkey_dev *tdev;
    int ret;

    tdev = devm_kzalloc(&hdev->dev, sizeof(*tdev), GFP_KERNEL);
    if (!tdev)
        return -ENOMEM;

    tdev->hdev         = hdev;
    tdev->disconnected = false;
    atomic_set(&tdev->touch_state, 0);
    mutex_init(&tdev->lock);
    hid_set_drvdata(hdev, tdev);

    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "hid_parse failed: %d\n", ret);
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        hid_err(hdev, "hid_hw_start failed: %d\n", ret);
        return ret;
    }

    /* hid_hw_open() is what actually starts the interrupt IN transfer
     * (the HID core submits the URB internally from here).
     */
    ret = hid_hw_open(hdev);
    if (ret) {
        hid_hw_stop(hdev);
        return ret;
    }

    ret = sysfs_create_group(&hdev->dev.kobj, &trinkey_group);
    if (ret) {
        hid_hw_close(hdev);
        hid_hw_stop(hdev);
        return ret;
    }

    hid_info(hdev, "device attached\n");
    return 0;
}

static void trinkey_remove(struct hid_device *hdev)
{
    struct trinkey_dev *tdev = hid_get_drvdata(hdev);

    /* Set the disconnected flag while holding the lock to avoid
     * use-after-free on concurrent sysfs access.
     */
    mutex_lock(&tdev->lock);
    tdev->disconnected = true;
    mutex_unlock(&tdev->lock);

    //send one last notify to avoid processes left on poll() on a disconnected device
    sysfs_notify(&hdev->dev.kobj, NULL, "trinkey_touch");

    sysfs_remove_group(&hdev->dev.kobj, &trinkey_group);
    hid_hw_close(hdev);
    hid_hw_stop(hdev);

    hid_info(hdev, "device disconnected\n");
}

static const struct hid_device_id trinkey_table[] = {
    { HID_USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(hid, trinkey_table);

static struct hid_driver trinkey_driver = {
    .name       = "adafruit_trinkey_custom",
    .id_table   = trinkey_table,
    .probe      = trinkey_probe,
    .remove     = trinkey_remove,
    .raw_event  = trinkey_raw_event,
};
module_hid_driver(trinkey_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Palu&&Passo");
MODULE_DESCRIPTION("Custom HID driver for NeoKey Trinkey");

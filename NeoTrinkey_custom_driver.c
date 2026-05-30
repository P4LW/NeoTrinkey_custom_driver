#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#define VENDOR_ID  0x239a
#define PRODUCT_ID 0x80ff

#define CMD_SET_LED   0x01
#define CMD_GET_TOUCH 0x02

/* Per-device state. The mutex serialises sysfs calls; disconnected
 * is set before freeing to prevent use-after-free on concurrent access.
 */
struct trinkey_dev {
    struct usb_device *udev;
    struct mutex       lock;
    bool               disconnected;
};


/* SYSFS INTERFACE                                                     */

/* cat /sys/.../trinkey_touch — returns 0 or 1 */
static ssize_t trinkey_touch_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct usb_interface *intf = to_usb_interface(dev);
    struct trinkey_dev   *tdev = usb_get_intfdata(intf);
    int retval;
    u8 *data;

    /* USB transfers require heap-allocated buffers (no stack DMA). */
    data = kmalloc(1, GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    if (mutex_lock_interruptible(&tdev->lock)) {
        kfree(data);
        return -ERESTARTSYS;
    }
    if (tdev->disconnected) {
        mutex_unlock(&tdev->lock);
        kfree(data);
        return -ENODEV;
    }

    retval = usb_control_msg_recv(tdev->udev, 0, CMD_GET_TOUCH,
                                  USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                                  0, 0, data, 1, 1000, GFP_KERNEL);
    mutex_unlock(&tdev->lock);

    if (retval) {
        dev_err(dev, "touch read error: %d\n", retval);
        kfree(data);
        return retval;
    }

    retval = sysfs_emit(buf, "%d\n", data[0]);
    kfree(data);
    return retval;
}

/* echo "R G B" > /sys/.../trinkey_led */
static ssize_t trinkey_led_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
    struct usb_interface *intf = to_usb_interface(dev);
    struct trinkey_dev   *tdev = usb_get_intfdata(intf);
    int r, g, b, retval;
    u8 *data;

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

    retval = usb_control_msg_send(tdev->udev, 0, CMD_SET_LED,
                                  USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                                  0, 0, data, 3, 1000, GFP_KERNEL);
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

/* Registered via .dev_groups so the USB core creates and removes
 * sysfs files automatically on probe/disconnect.
 */
static const struct attribute_group *trinkey_groups[] = {
    &trinkey_group,
    NULL,
};



/* USB DRIVER CORE                                                     */

static int trinkey_probe(struct usb_interface *interface,
                         const struct usb_device_id *id)
{
    struct trinkey_dev *tdev;

    tdev = kmalloc(sizeof(struct trinkey_dev), GFP_KERNEL);
    if (!tdev)
        return -ENOMEM;

    tdev->udev         = usb_get_dev(interface_to_usbdev(interface));
    tdev->disconnected = false;
    mutex_init(&tdev->lock);

    usb_set_intfdata(interface, tdev);

    dev_info(&interface->dev, "device attached\n");
    return 0;
}

static void trinkey_disconnect(struct usb_interface *interface)
{
    struct trinkey_dev *tdev = usb_get_intfdata(interface);

    /* Signal disconnection while holding the lock so any in-progress
     * sysfs operation sees -ENODEV rather than a freed pointer.
     */
    mutex_lock(&tdev->lock);
    tdev->disconnected = true;
    mutex_unlock(&tdev->lock);

    usb_set_intfdata(interface, NULL);
    usb_put_dev(tdev->udev);

    dev_info(&interface->dev, "device disconnected\n");

    kfree(tdev);
}

static const struct usb_device_id trinkey_table[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb, trinkey_table);

static struct usb_driver trinkey_driver = {
    .name       = "adafruit_trinkey_custom",
    .probe      = trinkey_probe,
    .disconnect = trinkey_disconnect,
    .id_table   = trinkey_table,
    .dev_groups = trinkey_groups,
};

module_usb_driver(trinkey_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Palu&&Passo");
MODULE_DESCRIPTION("Custom USB driver for NeoKey Trinkey");


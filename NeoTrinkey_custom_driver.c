#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <net/genetlink.h>

#include "trinkey_nl.h"

#define VENDOR_ID  0x239a
#define PRODUCT_ID 0x80ff

#define CMD_SET_LED   0x01
#define CMD_GET_TOUCH 0x02

/* Per-device state. The mutex serialises sysfs calls & worker access. */
struct trinkey_dev {
    struct usb_device   *udev;
    struct mutex        lock;
    bool                disconnected;
    u8                  last_touch;
    struct delayed_work poll_work;
};

static struct trinkey_dev *g_tdev = NULL;
static DEFINE_MUTEX(g_dev_lock);

/* Netlink Multicast Group definitions */
enum trinkey_nl_mcgrp_ids {
    TRINKEY_MCGRP_TOUCH,
};

static const struct genl_multicast_group trinkey_nl_mcgrps[] = {
    [TRINKEY_MCGRP_TOUCH] = { .name = TRINKEY_NL_MC_GROUP_TOUCH },
};

/* Netlink Attribute Validation Policy */
static const struct nla_policy trinkey_nl_policy[TRINKEY_ATTR_MAX + 1] = {
    [TRINKEY_ATTR_TOUCH_STATE] = { .type = NLA_U8 },
    [TRINKEY_ATTR_TIMESTAMP]   = { .type = NLA_U64 },
    [TRINKEY_ATTR_LED_R]       = { .type = NLA_U8 },
    [TRINKEY_ATTR_LED_G]       = { .type = NLA_U8 },
    [TRINKEY_ATTR_LED_B]       = { .type = NLA_U8 },
};

static int trinkey_nl_cmd_set_led(struct sk_buff *skb, struct genl_info *info);

static const struct genl_ops trinkey_nl_ops[] = {
    {
        .cmd      = TRINKEY_CMD_SET_LED,
        .validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
        .doit     = trinkey_nl_cmd_set_led,
    },
};

static struct genl_family trinkey_nl_family = {
    .name       = TRINKEY_NL_FAMILY_NAME,
    .version    = TRINKEY_NL_FAMILY_VERSION,
    .maxattr    = TRINKEY_ATTR_MAX,
    .policy     = trinkey_nl_policy,
    .ops        = trinkey_nl_ops,
    .n_ops      = ARRAY_SIZE(trinkey_nl_ops),
    .mcgrps     = trinkey_nl_mcgrps,
    .n_mcgrps   = ARRAY_SIZE(trinkey_nl_mcgrps),
    .module     = THIS_MODULE,
};

/* Netlink Multicast Publisher */
static void trinkey_broadcast_touch(u8 touch_state)
{
    struct sk_buff *skb;
    void *msg_head;

    skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
    if (!skb)
        return;

    msg_head = genlmsg_put(skb, 0, 0, &trinkey_nl_family, 0, TRINKEY_CMD_TOUCH_EVENT);
    if (!msg_head) {
        nlmsg_free(skb);
        return;
    }

    if (nla_put_u8(skb, TRINKEY_ATTR_TOUCH_STATE, touch_state) ||
        nla_put_u64_64bit(skb, TRINKEY_ATTR_TIMESTAMP, ktime_get_real_ns(), TRINKEY_ATTR_UNSPEC)) {
        nlmsg_free(skb);
        return;
    }

    genlmsg_end(skb, msg_head);
    genlmsg_multicast(&trinkey_nl_family, skb, 0, TRINKEY_MCGRP_TOUCH, GFP_ATOMIC);
}

/* Netlink Unicast Command Callback for LED control */
static int trinkey_nl_cmd_set_led(struct sk_buff *skb, struct genl_info *info)
{
    u8 r, g, b;
    u8 *data;
    int retval = 0;
    struct trinkey_dev *tdev;

    if (!info->attrs[TRINKEY_ATTR_LED_R] ||
        !info->attrs[TRINKEY_ATTR_LED_G] ||
        !info->attrs[TRINKEY_ATTR_LED_B]) {
        return -EINVAL;
    }

    r = nla_get_u8(info->attrs[TRINKEY_ATTR_LED_R]);
    g = nla_get_u8(info->attrs[TRINKEY_ATTR_LED_G]);
    b = nla_get_u8(info->attrs[TRINKEY_ATTR_LED_B]);

    mutex_lock(&g_dev_lock);
    tdev = g_tdev;
    if (!tdev) {
        mutex_unlock(&g_dev_lock);
        return -ENODEV;
    }

    data = kmalloc(3, GFP_KERNEL);
    if (!data) {
        mutex_unlock(&g_dev_lock);
        return -ENOMEM;
    }

    data[0] = r;
    data[1] = g;
    data[2] = b;

    if (mutex_lock_interruptible(&tdev->lock)) {
        kfree(data);
        mutex_unlock(&g_dev_lock);
        return -ERESTARTSYS;
    }

    if (tdev->disconnected) {
        mutex_unlock(&tdev->lock);
        kfree(data);
        mutex_unlock(&g_dev_lock);
        return -ENODEV;
    }

    retval = usb_control_msg_send(tdev->udev, 0, CMD_SET_LED,
                                  USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                                  0, 0, data, 3, 100, GFP_KERNEL);
    mutex_unlock(&tdev->lock);
    kfree(data);
    mutex_unlock(&g_dev_lock);

    return retval;
}

/* Kernel Delayed Work for Touch Polling and Edge Detection */
static void trinkey_poll_work_fn(struct work_struct *work)
{
    struct trinkey_dev *tdev = container_of(work, struct trinkey_dev, poll_work.work);
    u8 *data;
    int retval;
    u8 current_touch;

    if (tdev->disconnected)
        return;

    data = kmalloc(1, GFP_KERNEL);
    if (!data)
        goto reschedule;

    if (mutex_lock_interruptible(&tdev->lock)) {
        kfree(data);
        goto reschedule;
    }

    if (tdev->disconnected) {
        mutex_unlock(&tdev->lock);
        kfree(data);
        return;
    }

    retval = usb_control_msg_recv(tdev->udev, 0, CMD_GET_TOUCH,
                                  USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                                  0, 0, data, 1, 100, GFP_KERNEL);
    if (retval == 0) {
        current_touch = data[0];
        if (current_touch != tdev->last_touch) {
            tdev->last_touch = current_touch;
            trinkey_broadcast_touch(current_touch);
        }
    }
    mutex_unlock(&tdev->lock);
    kfree(data);

reschedule:
    if (!tdev->disconnected)
        schedule_delayed_work(&tdev->poll_work, msecs_to_jiffies(50)); /* 20 Hz kernel polling */
}


// SYSFS INTERFACE (Preserved for backwards compatibility & debugging)

static ssize_t trinkey_touch_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct usb_interface *intf = to_usb_interface(dev);
    struct trinkey_dev   *tdev = usb_get_intfdata(intf);
    int retval;
    u8 *data;

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
                                  0, 0, data, 1, 100, GFP_KERNEL);
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

static const struct attribute_group *trinkey_groups[] = {
    &trinkey_group,
    NULL,
};


// USB DRIVER CORE

static int trinkey_probe(struct usb_interface *interface,
                         const struct usb_device_id *id)
{
    struct trinkey_dev *tdev;

    tdev = kmalloc(sizeof(struct trinkey_dev), GFP_KERNEL);
    if (!tdev)
        return -ENOMEM;

    tdev->udev         = usb_get_dev(interface_to_usbdev(interface));
    tdev->disconnected = false;
    tdev->last_touch   = 0xFF; /* Initial force state trigger */
    mutex_init(&tdev->lock);

    INIT_DELAYED_WORK(&tdev->poll_work, trinkey_poll_work_fn);
    usb_set_intfdata(interface, tdev);

    mutex_lock(&g_dev_lock);
    g_tdev = tdev;
    mutex_unlock(&g_dev_lock);

    /* Start kernel polling delayed work */
    schedule_delayed_work(&tdev->poll_work, msecs_to_jiffies(50));

    dev_info(&interface->dev, "NeoTrinkey device attached & Pub/Sub active\n");
    return 0;
}

static void trinkey_disconnect(struct usb_interface *interface)
{
    struct trinkey_dev *tdev = usb_get_intfdata(interface);

    mutex_lock(&tdev->lock);
    tdev->disconnected = true;
    mutex_unlock(&tdev->lock);

    cancel_delayed_work_sync(&tdev->poll_work);

    mutex_lock(&g_dev_lock);
    if (g_tdev == tdev)
        g_tdev = NULL;
    mutex_unlock(&g_dev_lock);

    usb_set_intfdata(interface, NULL);
    usb_put_dev(tdev->udev);

    dev_info(&interface->dev, "NeoTrinkey device disconnected\n");
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

static int __init trinkey_init(void)
{
    int ret;

    ret = genl_register_family(&trinkey_nl_family);
    if (ret) {
        pr_err("trinkey: failed to register Generic Netlink family: %d\n", ret);
        return ret;
    }

    ret = usb_register(&trinkey_driver);
    if (ret) {
        pr_err("trinkey: usb_register failed: %d\n", ret);
        genl_unregister_family(&trinkey_nl_family);
        return ret;
    }

    pr_info("trinkey: module loaded (Generic Netlink Pub/Sub ready)\n");
    return 0;
}

static void __exit trinkey_exit(void)
{
    usb_deregister(&trinkey_driver);
    genl_unregister_family(&trinkey_nl_family);
    pr_info("trinkey: module unloaded\n");
}

module_init(trinkey_init);
module_exit(trinkey_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Palu&&Passo");
MODULE_DESCRIPTION("Custom USB driver for NeoKey Trinkey with Netlink Pub/Sub");

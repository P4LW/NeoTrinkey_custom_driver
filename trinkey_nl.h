#ifndef TRINKEY_NL_H
#define TRINKEY_NL_H

#define TRINKEY_NL_FAMILY_NAME     "TRINKEY_NL"
#define TRINKEY_NL_FAMILY_VERSION  1
#define TRINKEY_NL_MC_GROUP_TOUCH  "touch_events"

enum trinkey_nl_commands {
    TRINKEY_CMD_UNSPEC,
    TRINKEY_CMD_TOUCH_EVENT, /* Kernel -> Userspace (Multicast) */
    TRINKEY_CMD_SET_LED,     /* Userspace -> Kernel (Unicast) */
    __TRINKEY_CMD_MAX,
};
#define TRINKEY_CMD_MAX (__TRINKEY_CMD_MAX - 1)

enum trinkey_nl_attrs {
    TRINKEY_ATTR_UNSPEC,
    TRINKEY_ATTR_TOUCH_STATE, /* u8: 0 = Released, 1 = Pressed */
    TRINKEY_ATTR_TIMESTAMP,   /* u64: Nanoseconds timestamp */
    TRINKEY_ATTR_LED_R,       /* u8: Red 0-255 */
    TRINKEY_ATTR_LED_G,       /* u8: Green 0-255 */
    TRINKEY_ATTR_LED_B,       /* u8: Blue 0-255 */
    __TRINKEY_ATTR_MAX,
};
#define TRINKEY_ATTR_MAX (__TRINKEY_ATTR_MAX - 1)

#endif /* TRINKEY_NL_H */

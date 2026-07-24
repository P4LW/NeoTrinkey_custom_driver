#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#include "trinkey_nl.h"

#define CONFIG_FILE "/etc/trinkey/config"
#define PID_FILE    "/run/trinkey/trinkey.pid"

typedef enum {
    MODE_STATIC,
    MODE_BLINK,
    MODE_BREATH
} LedMode;

typedef struct {
    LedMode mode;
    int color_idle[3];
    int color_touch[3];
} AppConfig;

char led_file[512];
char touch_file[512];
volatile int keep_running  = 1;
volatile int reload_config = 0;
AppConfig config;

static int nl_fd = -1;
static int nl_family_id = -1;
static int nl_mcast_group_id = -1;
static uint8_t current_touch_state = 0;


// SIGNAL HANDLING

static void sig_handler(int signo)
{
    if (signo == SIGINT)
        keep_running = 0;
    else if (signo == SIGHUP)
        reload_config = 1;
}


// NETLINK PUB/SUB HELPERS

static int nl_send_msg(int fd, uint16_t type, uint8_t cmd, uint16_t flags, const void *payload, int payload_len)
{
    char buf[512];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct genlmsghdr *genl = (struct genlmsghdr *)(buf + NLMSG_HDRLEN);

    nlh->nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN + payload_len);
    nlh->nlmsg_type  = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq   = 1;
    nlh->nlmsg_pid   = getpid();

    genl->cmd     = cmd;
    genl->version = TRINKEY_NL_FAMILY_VERSION;

    if (payload_len > 0 && payload) {
        memcpy(buf + NLMSG_HDRLEN + GENL_HDRLEN, payload, payload_len);
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    return sendto(fd, buf, nlh->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static int nl_init_pubsub(void)
{
    nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (nl_fd < 0)
        return -1;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid    = getpid();

    if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(nl_fd);
        nl_fd = -1;
        return -1;
    }

    /* Build CTRL_CMD_GETFAMILY request for TRINKEY_NL */
    char attr_buf[256];
    memset(attr_buf, 0, sizeof(attr_buf));
    struct nlattr *nla = (struct nlattr *)attr_buf;
    int name_len = strlen(TRINKEY_NL_FAMILY_NAME) + 1;
    nla->nla_len  = NLA_HDRLEN + name_len;
    nla->nla_type = CTRL_ATTR_FAMILY_NAME;
    memcpy(attr_buf + NLA_HDRLEN, TRINKEY_NL_FAMILY_NAME, name_len);

    int payload_len = NLA_ALIGN(nla->nla_len);
    if (nl_send_msg(nl_fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, NLM_F_REQUEST, attr_buf, payload_len) < 0) {
        close(nl_fd);
        nl_fd = -1;
        return -1;
    }

    /* Read response from Netlink Controller */
    char rcv_buf[4096];
    int len = recv(nl_fd, rcv_buf, sizeof(rcv_buf), 0);
    if (len < 0) {
        close(nl_fd);
        nl_fd = -1;
        return -1;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)rcv_buf;
    if (!NLMSG_OK(nlh, len)) {
        close(nl_fd);
        nl_fd = -1;
        return -1;
    }

    struct genlmsghdr *genl = (struct genlmsghdr *)NLMSG_DATA(nlh);
    struct nlattr *na = (struct nlattr *)((char *)genl + GENL_HDRLEN);
    int attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

    nl_family_id = -1;
    nl_mcast_group_id = -1;

    while (attr_len >= NLA_HDRLEN && na->nla_len >= NLA_HDRLEN) {
        if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
            nl_family_id = *(uint16_t *)((char *)na + NLA_HDRLEN);
        } else if (na->nla_type == CTRL_ATTR_MCAST_GROUPS) {
            struct nlattr *grp_na = (struct nlattr *)((char *)na + NLA_HDRLEN);
            int grp_len = na->nla_len - NLA_HDRLEN;
            while (grp_len >= NLA_HDRLEN && grp_na->nla_len >= NLA_HDRLEN) {
                struct nlattr *item = (struct nlattr *)((char *)grp_na + NLA_HDRLEN);
                int item_len = grp_na->nla_len - NLA_HDRLEN;
                uint32_t grp_id = 0;
                char grp_name[64] = {0};

                while (item_len >= NLA_HDRLEN && item->nla_len >= NLA_HDRLEN) {
                    if (item->nla_type == CTRL_ATTR_MCAST_GRP_ID) {
                        grp_id = *(uint32_t *)((char *)item + NLA_HDRLEN);
                    } else if (item->nla_type == CTRL_ATTR_MCAST_GRP_NAME) {
                        snprintf(grp_name, sizeof(grp_name), "%s", (char *)item + NLA_HDRLEN);
                    }
                    int item_size = NLA_ALIGN(item->nla_len);
                    item = (struct nlattr *)((char *)item + item_size);
                    item_len -= item_size;
                }

                if (strcmp(grp_name, TRINKEY_NL_MC_GROUP_TOUCH) == 0) {
                    nl_mcast_group_id = grp_id;
                }

                int grp_size = NLA_ALIGN(grp_na->nla_len);
                grp_na = (struct nlattr *)((char *)grp_na + grp_size);
                grp_len -= grp_size;
            }
        }
        int na_size = NLA_ALIGN(na->nla_len);
        na = (struct nlattr *)((char *)na + na_size);
        attr_len -= na_size;
    }

    if (nl_family_id < 0 || nl_mcast_group_id < 0) {
        close(nl_fd);
        nl_fd = -1;
        return -1;
    }

    /* Join Netlink Multicast Group */
    if (setsockopt(nl_fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &nl_mcast_group_id, sizeof(nl_mcast_group_id)) < 0) {
        close(nl_fd);
        nl_fd = -1;
        return -1;
    }

    printf("Netlink Pub/Sub initialized: Family ID=%d, Multicast Group ID=%d\n",
           nl_family_id, nl_mcast_group_id);
    return 0;
}

static void set_led_netlink(int r, int g, int b)
{
    if (nl_fd < 0 || nl_family_id < 0)
        return;

    char attr_buf[256];
    memset(attr_buf, 0, sizeof(attr_buf));
    int offset = 0;
    struct nlattr *nla;

    nla = (struct nlattr *)(attr_buf + offset);
    nla->nla_len  = NLA_HDRLEN + 1;
    nla->nla_type = TRINKEY_ATTR_LED_R;
    *(uint8_t *)((char *)nla + NLA_HDRLEN) = (uint8_t)r;
    offset += NLA_ALIGN(nla->nla_len);

    nla = (struct nlattr *)(attr_buf + offset);
    nla->nla_len  = NLA_HDRLEN + 1;
    nla->nla_type = TRINKEY_ATTR_LED_G;
    *(uint8_t *)((char *)nla + NLA_HDRLEN) = (uint8_t)g;
    offset += NLA_ALIGN(nla->nla_len);

    nla = (struct nlattr *)(attr_buf + offset);
    nla->nla_len  = NLA_HDRLEN + 1;
    nla->nla_type = TRINKEY_ATTR_LED_B;
    *(uint8_t *)((char *)nla + NLA_HDRLEN) = (uint8_t)b;
    offset += NLA_ALIGN(nla->nla_len);

    nl_send_msg(nl_fd, nl_family_id, TRINKEY_CMD_SET_LED, NLM_F_REQUEST, attr_buf, offset);
}

static int nl_read_touch_event(int fd, uint8_t *touch_state, uint64_t *timestamp)
{
    char buf[4096];
    int len = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (len <= 0)
        return -1;

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    if (!NLMSG_OK(nlh, len))
        return -1;

    struct genlmsghdr *genl = (struct genlmsghdr *)NLMSG_DATA(nlh);
    if (genl->cmd != TRINKEY_CMD_TOUCH_EVENT)
        return -1;

    struct nlattr *na = (struct nlattr *)((char *)genl + GENL_HDRLEN);
    int attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
    int found = 0;

    while (attr_len >= NLA_HDRLEN && na->nla_len >= NLA_HDRLEN) {
        if (na->nla_type == TRINKEY_ATTR_TOUCH_STATE) {
            if (touch_state)
                *touch_state = *(uint8_t *)((char *)na + NLA_HDRLEN);
            found = 1;
        } else if (na->nla_type == TRINKEY_ATTR_TIMESTAMP) {
            if (timestamp)
                *timestamp = *(uint64_t *)((char *)na + NLA_HDRLEN);
        }
        int na_size = NLA_ALIGN(na->nla_len);
        na = (struct nlattr *)((char *)na + na_size);
        attr_len -= na_size;
    }

    return found ? 0 : -1;
}


// SYSFS I/O FALLBACK

static void set_led_sysfs(int r, int g, int b)
{
    FILE *f = fopen(led_file, "w");
    if (!f) {
        fprintf(stderr, "error: lost connection to device, exiting\n");
        keep_running = 0;
        return;
    }
    fprintf(f, "%d %d %d\n", r, g, b);
    fclose(f);
}

static void set_led(int r, int g, int b)
{
    if (nl_fd >= 0) {
        set_led_netlink(r, g, b);
    } else {
        set_led_sysfs(r, g, b);
    }
}


// CONFIGURATION

static int load_configuration(AppConfig *cfg)
{
    cfg->mode           = MODE_STATIC;
    cfg->color_idle[0]  = 255; cfg->color_idle[1]  = 0; cfg->color_idle[2]  = 0;
    cfg->color_touch[0] = 0;   cfg->color_touch[1] = 255; cfg->color_touch[2] = 0;

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        perror("warning: cannot open config file, using defaults");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63[^\n]", key, val) != 2)
            continue;

        if (strcmp(key, "mode") == 0) {
            if (strcmp(val, "static") == 0)       cfg->mode = MODE_STATIC;
            else if (strcmp(val, "blink") == 0)   cfg->mode = MODE_BLINK;
            else if (strcmp(val, "breath") == 0)  cfg->mode = MODE_BREATH;
            else fprintf(stderr, "warning: unknown mode '%s', ignoring\n", val);
        } else if (strcmp(key, "idle_color") == 0) {
            if (sscanf(val, "%d %d %d",
                &cfg->color_idle[0],
                &cfg->color_idle[1],
                &cfg->color_idle[2]) != 3)
                fprintf(stderr, "warning: invalid idle_color, using default\n");
        } else if (strcmp(key, "touch_color") == 0) {
            if (sscanf(val, "%d %d %d",
                &cfg->color_touch[0],
                &cfg->color_touch[1],
                &cfg->color_touch[2]) != 3)
                fprintf(stderr, "warning: invalid touch_color, using default\n");
        }
    }

    fclose(f);
    printf("config loaded: mode=%d idle=%d,%d,%d touch=%d,%d,%d\n",
           cfg->mode,
           cfg->color_idle[0],  cfg->color_idle[1],  cfg->color_idle[2],
           cfg->color_touch[0], cfg->color_touch[1], cfg->color_touch[2]);
    return 0;
}


// DEVICE DISCOVERY AND DRIVER SETUP

static char *find_trinkey_bus(void)
{
    static char bus_name[64];
    DIR *d = opendir("/sys/bus/usb/devices/");
    if (!d)
        return NULL;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        char vendor_path[512], prod_path[512];

        snprintf(vendor_path, sizeof(vendor_path),
                 "/sys/bus/usb/devices/%s/idVendor", dir->d_name);
        snprintf(prod_path, sizeof(prod_path),
                 "/sys/bus/usb/devices/%s/idProduct", dir->d_name);

        FILE *vf = fopen(vendor_path, "r");
        if (!vf)
            continue;
        char vendor[10];
        int vendor_match = fgets(vendor, sizeof(vendor), vf) && strstr(vendor, "239a");
        fclose(vf);
        if (!vendor_match)
            continue;

        FILE *pf = fopen(prod_path, "r");
        if (!pf)
            continue;
        char product[10];
        int product_match = fgets(product, sizeof(product), pf) && strstr(product, "80ff");
        fclose(pf);
        if (!product_match)
            continue;

        strncpy(bus_name, dir->d_name, sizeof(bus_name) - 1);
        bus_name[sizeof(bus_name) - 1] = '\0';
        closedir(d);
        return bus_name;
    }

    closedir(d);
    return NULL;
}

static int setup_driver(const char *bus)
{
    char interface[128];
    snprintf(interface, sizeof(interface), "%s:1.0", bus);

    snprintf(led_file, sizeof(led_file),
             "/sys/bus/usb/devices/%s/trinkey_led", interface);
    snprintf(touch_file, sizeof(touch_file),
             "/sys/bus/usb/devices/%s/trinkey_touch", interface);

    int retries = 20;
    while (retries--) {
        if (access(led_file, F_OK) == 0 &&
            access(touch_file, F_OK) == 0)
            return 0;
        usleep(100000);
    }

    fprintf(stderr, "warning: sysfs files not found\n");
    return -1;
}


// MAIN LOOP WITH EPOLL AND NETLINK PUB/SUB

int main(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGHUP, sig_handler);

    load_configuration(&config);

    char *bus = find_trinkey_bus();
    if (bus) {
        setup_driver(bus);
    }

    /* Initialize Netlink Pub/Sub */
    if (nl_init_pubsub() != 0) {
        printf("Notice: Generic Netlink Pub/Sub unavailable, falling back to Sysfs polling.\n");
    } else {
        printf("Generic Netlink Pub/Sub connected successfully.\n");
    }

    FILE *pid_f = fopen(PID_FILE, "w");
    if (pid_f) {
        fprintf(pid_f, "%d\n", getpid());
        fclose(pid_f);
    }

    printf("Trinkey connected.\nLed active.\n");

    int epoll_fd = -1;
    if (nl_fd >= 0) {
        epoll_fd = epoll_create1(0);
        if (epoll_fd >= 0) {
            struct epoll_event ev;
            ev.events  = EPOLLIN;
            ev.data.fd = nl_fd;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, nl_fd, &ev);
        }
    }

    unsigned int tick = 0;

    while (keep_running) {
        if (reload_config) {
            load_configuration(&config);
            reload_config = 0;
        }

        if (epoll_fd >= 0) {
            struct epoll_event events[2];
            int nfds = epoll_wait(epoll_fd, events, 2, 100); /* 100ms timeout for 10 Hz animations */
            for (int i = 0; i < nfds; i++) {
                if (events[i].data.fd == nl_fd) {
                    uint8_t touch = 0;
                    uint64_t ts = 0;
                    if (nl_read_touch_event(nl_fd, &touch, &ts) == 0) {
                        current_touch_state = touch;
                        printf("[PUB/SUB EVENT] Touch state changed: %d (Timestamp: %llu ns)\n",
                               current_touch_state, (unsigned long long)ts);
                    }
                }
            }
        } else {
            usleep(100000); /* Fallback polling delay */
        }

        if (current_touch_state) {
            set_led(config.color_touch[0],
                    config.color_touch[1],
                    config.color_touch[2]);
        } else {
            switch (config.mode) {
                case MODE_STATIC:
                    set_led(config.color_idle[0],
                            config.color_idle[1],
                            config.color_idle[2]);
                    break;

                case MODE_BLINK:
                    if ((tick % 10) < 5)
                        set_led(config.color_idle[0],
                                config.color_idle[1],
                                config.color_idle[2]);
                    else
                        set_led(0, 0, 0);
                    break;

                case MODE_BREATH:
                    {
                        double radians   = (tick % 30) * (2.0 * M_PI / 30.0);
                        double intensity = (sin(radians - M_PI / 2.0) + 1.0) / 2.0;
                        set_led((int)(config.color_idle[0] * intensity),
                                (int)(config.color_idle[1] * intensity),
                                (int)(config.color_idle[2] * intensity));
                    }
                    break;
            }
        }

        tick++;
    }

    printf("\nexiting, turning off LED\n");
    set_led(0, 0, 0);
    if (nl_fd >= 0) {
        close(nl_fd);
    }
    if (epoll_fd >= 0) {
        close(epoll_fd);
    }
    unlink(PID_FILE);

    return 0;
}

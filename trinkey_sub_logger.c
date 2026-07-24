#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#include "trinkey_nl.h"

static volatile int keep_running = 1;

static void sig_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
        keep_running = 0;
}

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

int main(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("=====================================================\n");
    printf(" NeoTrinkey Netlink Secondary Subscriber Logger App  \n");
    printf("=====================================================\n");

    int nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (nl_fd < 0) {
        perror("Error creating Netlink socket");
        return 1;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid    = getpid();

    if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("Error binding Netlink socket");
        close(nl_fd);
        return 1;
    }

    /* Request Family ID and Multicast Group ID for TRINKEY_NL */
    char attr_buf[256];
    memset(attr_buf, 0, sizeof(attr_buf));
    struct nlattr *nla = (struct nlattr *)attr_buf;
    int name_len = strlen(TRINKEY_NL_FAMILY_NAME) + 1;
    nla->nla_len  = NLA_HDRLEN + name_len;
    nla->nla_type = CTRL_ATTR_FAMILY_NAME;
    memcpy(attr_buf + NLA_HDRLEN, TRINKEY_NL_FAMILY_NAME, name_len);

    int payload_len = NLA_ALIGN(nla->nla_len);
    if (nl_send_msg(nl_fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, NLM_F_REQUEST, attr_buf, payload_len) < 0) {
        perror("Error sending GETFAMILY request");
        close(nl_fd);
        return 1;
    }

    char rcv_buf[4096];
    int len = recv(nl_fd, rcv_buf, sizeof(rcv_buf), 0);
    if (len < 0) {
        perror("Error receiving GETFAMILY response");
        close(nl_fd);
        return 1;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)rcv_buf;
    if (!NLMSG_OK(nlh, len)) {
        fprintf(stderr, "Invalid Netlink response\n");
        close(nl_fd);
        return 1;
    }

    struct genlmsghdr *genl = (struct genlmsghdr *)NLMSG_DATA(nlh);
    struct nlattr *na = (struct nlattr *)((char *)genl + GENL_HDRLEN);
    int attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

    int family_id = -1;
    int mcast_grp_id = -1;

    while (attr_len >= NLA_HDRLEN && na->nla_len >= NLA_HDRLEN) {
        if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
            family_id = *(uint16_t *)((char *)na + NLA_HDRLEN);
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
                    mcast_grp_id = grp_id;
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

    if (family_id < 0 || mcast_grp_id < 0) {
        fprintf(stderr, "TRINKEY_NL family or multicast group not found. Ensure driver is loaded.\n");
        close(nl_fd);
        return 1;
    }

    if (setsockopt(nl_fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &mcast_grp_id, sizeof(mcast_grp_id)) < 0) {
        perror("Error joining Netlink Multicast group");
        close(nl_fd);
        return 1;
    }

    printf("Subscribed to Multicast Group '%s' (ID=%d, Family=%d)\n",
           TRINKEY_NL_MC_GROUP_TOUCH, mcast_grp_id, family_id);
    printf("Listening for touch events in real-time...\n\n");

    int epoll_fd = epoll_create1(0);
    if (epoll_fd >= 0) {
        struct epoll_event ev;
        ev.events  = EPOLLIN;
        ev.data.fd = nl_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, nl_fd, &ev);
    }

    while (keep_running) {
        struct epoll_event events[1];
        int nfds = epoll_wait(epoll_fd, events, 1, 500);
        if (nfds > 0 && events[0].data.fd == nl_fd) {
            char buf[4096];
            int rlen = recv(nl_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (rlen > 0) {
                struct nlmsghdr *msg_hdr = (struct nlmsghdr *)buf;
                if (NLMSG_OK(msg_hdr, rlen)) {
                    struct genlmsghdr *ghdr = (struct genlmsghdr *)NLMSG_DATA(msg_hdr);
                    if (ghdr->cmd == TRINKEY_CMD_TOUCH_EVENT) {
                        struct nlattr *attr = (struct nlattr *)((char *)ghdr + GENL_HDRLEN);
                        int alen = msg_hdr->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
                        uint8_t state = 0;
                        uint64_t ts = 0;

                        while (alen >= NLA_HDRLEN && attr->nla_len >= NLA_HDRLEN) {
                            if (attr->nla_type == TRINKEY_ATTR_TOUCH_STATE) {
                                state = *(uint8_t *)((char *)attr + NLA_HDRLEN);
                            } else if (attr->nla_type == TRINKEY_ATTR_TIMESTAMP) {
                                ts = *(uint64_t *)((char *)attr + NLA_HDRLEN);
                            }
                            int asize = NLA_ALIGN(attr->nla_len);
                            attr = (struct nlattr *)((char *)attr + asize);
                            alen -= asize;
                        }

                        time_t sec = ts / 1000000000ULL;
                        struct tm *tm_info = localtime(&sec);
                        char time_str[32];
                        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

                        printf("[%s] MULTICAST EVENT: Touch %s (Raw TS: %llu ns)\n",
                               time_str, state ? ">>> PRESSED <<<" : "--- RELEASED ---", (unsigned long long)ts);
                        fflush(stdout);
                    }
                }
            }
        }
    }

    printf("\nExiting secondary subscriber logger.\n");
    close(epoll_fd);
    close(nl_fd);
    return 0;
}

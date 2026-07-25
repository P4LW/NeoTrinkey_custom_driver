#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>

#define CONFIG_FILE "/etc/trinkey/config"

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
char pid_file[512];
volatile int keep_running   = 1;
volatile int reload_config  = 0;
AppConfig config;

static int touch_fd = -1;
static int current_touch = 0;
static int last_r = -1, last_g = -1, last_b = -1;


// SIGNAL HANDLING

static void sig_handler(int signo)
{
    if (signo == SIGINT)
        keep_running = 0;
    else if (signo == SIGHUP)
        reload_config = 1;
}


// SYSFS I/O

static void set_led(int r, int g, int b)
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

static void set_led_cached(int r, int g, int b)
{
    if (r == last_r && g == last_g && b == last_b)
        return;

    set_led(r, g, b);
    last_r = r;
    last_g = g;
    last_b = b;
}

static int touch_open(void)
{
    touch_fd = open(touch_file, O_RDONLY);
    if (touch_fd < 0)
        return -1;

    char buf[8];
    ssize_t n = read(touch_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        current_touch = atoi(buf);
    }

    return 0;
}

static int touch_wait(int timeout_ms)
{
    struct pollfd pfd = { .fd = touch_fd, .events = POLLPRI | POLLERR };

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }
    if (ret == 0)
        return 0;

    if (lseek(touch_fd, 0, SEEK_SET) < 0)
        return -1;

    char buf[8];
    ssize_t n = read(touch_fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return -1; // device disconnected

    buf[n] = '\0';
    current_touch = atoi(buf);
    return 0;
}


// CONFIGURATION

static int load_configuration(AppConfig *cfg)
{
    cfg->mode         = MODE_STATIC;
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

static char *find_trinkey_hid_dir(void)
{
    static char hid_dir[512];

    DIR *d = opendir("/sys/bus/hid/devices/");
    if (!d)
        return NULL;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.')
            continue;

        char led_path[600], touch_path[600];
        snprintf(led_path, sizeof(led_path),
                 "/sys/bus/hid/devices/%s/trinkey_led", dir->d_name);
        snprintf(touch_path, sizeof(touch_path),
                 "/sys/bus/hid/devices/%s/trinkey_touch", dir->d_name);

        if (access(led_path, F_OK) == 0 && access(touch_path, F_OK) == 0) {
            snprintf(hid_dir, sizeof(hid_dir),
                     "/sys/bus/hid/devices/%s", dir->d_name);
            closedir(d);
            return hid_dir;
        }
    }

    closedir(d);
    return NULL;
}

static int setup_driver(const char *target_device)
{
    if (target_device && strlen(target_device) > 0) {
        char base_dir[512];
        if (target_device[0] == '/') {
            snprintf(base_dir, sizeof(base_dir), "%s", target_device);
        } else {
            snprintf(base_dir, sizeof(base_dir), "/sys/bus/hid/devices/%s", target_device);
        }

        snprintf(led_file, sizeof(led_file), "%s/trinkey_led", base_dir);
        snprintf(touch_file, sizeof(touch_file), "%s/trinkey_touch", base_dir);

        const char *dev_name = strrchr(base_dir, '/');
        dev_name = dev_name ? dev_name + 1 : base_dir;
        snprintf(pid_file, sizeof(pid_file), "/run/trinkey/trinkey_%s.pid", dev_name);

        if (access(led_file, F_OK) == 0 && access(touch_file, F_OK) == 0) {
            printf("Target device bound: %s\n", base_dir);
            return 0;
        }
        fprintf(stderr, "error: specified trinkey sysfs files not found at %s\n", base_dir);
        return -1;
    }

    // Fallback: auto-discover first available device
    snprintf(pid_file, sizeof(pid_file), "/run/trinkey/trinkey.pid");
    int retries = 20;
    while (retries--) {
        char *hid_dir = find_trinkey_hid_dir();
        if (hid_dir) {
            snprintf(led_file, sizeof(led_file), "%s/trinkey_led", hid_dir);
            snprintf(touch_file, sizeof(touch_file), "%s/trinkey_touch", hid_dir);
            return 0;
        }
        usleep(100000);
    }

    fprintf(stderr, "error: trinkey sysfs files not found after 2s\n");
    return -1;
}


// LED RENDERING

static void update_led(unsigned int tick)
{
    if (current_touch) {
        set_led_cached(config.color_touch[0],
                       config.color_touch[1],
                       config.color_touch[2]);
        return;
    }

    switch (config.mode) {
        case MODE_STATIC:
            set_led_cached(config.color_idle[0],
                           config.color_idle[1],
                           config.color_idle[2]);
            break;

        case MODE_BLINK:
            if ((tick % 10) < 5)
                set_led_cached(config.color_idle[0],
                               config.color_idle[1],
                               config.color_idle[2]);
            else
                set_led_cached(0, 0, 0);
            break;

        case MODE_BREATH:
        {
            double radians   = (tick % 30) * (2.0 * M_PI / 30.0);
            double intensity = (sin(radians - M_PI / 2.0) + 1.0) / 2.0;
            set_led_cached((int)(config.color_idle[0] * intensity),
                           (int)(config.color_idle[1] * intensity),
                           (int)(config.color_idle[2] * intensity));
        }
        break;
    }
}


// MAIN LOOP

int main(int argc, char *argv[])
{
    signal(SIGINT, sig_handler);
    signal(SIGHUP, sig_handler);

    load_configuration(&config);

    const char *target_device = (argc > 1) ? argv[1] : NULL;
    if (setup_driver(target_device) != 0) {
        fprintf(stderr, "error: driver setup failed\n");
        return 1;
    }

    if (touch_open() != 0) {
        fprintf(stderr, "error: cannot open %s\n", touch_file);
        return 1;
    }

    FILE *pid_f = fopen(pid_file, "w");
    if (pid_f) {
        fprintf(pid_f, "%d\n", getpid());
        fclose(pid_f);
    }

    printf("Trinkey instance started for %s (PID: %d)\n", led_file, getpid());

    unsigned int tick = 0;

    update_led(tick);

    while (keep_running) {
        if (reload_config) {
            load_configuration(&config);
            reload_config = 0;
        }

        int animating = (config.mode != MODE_STATIC) && !current_touch;
        int timeout = animating ? 100 : -1;

        if (touch_wait(timeout) != 0) {
            fprintf(stderr, "error: lost connection to device, exiting\n");
            break;
        }

        update_led(tick);
        if (current_touch)
            tick = 0;
        else
            tick++;
    }

    printf("\nexiting, turning off LED\n");
    set_led(0, 0, 0);
    close(touch_fd);
    unlink(pid_file);

    return 0;
}

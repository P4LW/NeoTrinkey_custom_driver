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
#define PID_FILE "/run/trinkey/trinkey.pid"

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
volatile int keep_running   = 1;
volatile int reload_config  = 0;
AppConfig config;

static int touch_fd = -1;
static int current_touch = 0;

// Last color actually written to the LED; used to avoid redundant sysfs
// writes when the computed color hasn't changed since the last call.
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

    printf("DEBUG: writing color %d %d %d on led\n", r, g, b);
}

// Wrapper around set_led() that skips the write (and the sysfs round-trip
// it costs) when the requested color is identical to the last one written.
static void set_led_cached(int r, int g, int b)
{
    if (r == last_r && g == last_g && b == last_b)
        return;

    set_led(r, g, b);
    last_r = r;
    last_g = g;
    last_b = b;
}

// Opens the touch attribute and reads it once to establish a baseline.
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

/* Waits up to timeout_ms for the driver to notify a touch state change
 * (via sysfs_notify() on the kernel side, triggered by the HID interrupt
 * report). Updates current_touch if a change arrived.
 *
 * A negative timeout blocks indefinitely - used when the current LED
 * color isn't animating, so there's nothing to wake up for on a timer.
 *
 * Returns 0 on success (timeout or updated state), -1 if the device is
 * gone and the caller should stop.
 */
static int touch_wait(int timeout_ms)
{
    struct pollfd pfd = { .fd = touch_fd, .events = POLLPRI | POLLERR };

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR)
            return 0; // interrupted by SIGINT/SIGHUP, nothing to do here
            return -1;
    }
    if (ret == 0)
        return 0; // timeout, no change: keep driving the LED animation tick

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
    // Defaults used both as fallback on missing file and as initial values before parsing
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

/* The driver binds as a hid_driver, so its sysfs attributes live under /sys/bus/hid/devices/<hid_id>/.
 * Iterate to look for the entry that actually exposes trinkey_led and trinkey_touch.
 */
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

/* Retries discovery for a couple of seconds, to give the driver
 * time to probe the device and create the sysfs files.
 */
static int setup_driver(void)
{
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

// Computes the color that should currently be shown and writes it via set_led_cached()
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
            // 0.5 Hz blink: on for 5 ticks, off for 5 ticks.
            if ((tick % 10) < 5)
                set_led_cached(config.color_idle[0],
                               config.color_idle[1],
                               config.color_idle[2]);
                else
                    set_led_cached(0, 0, 0);
        break;

        case MODE_BREATH:
            // Sine-based fade over a 3-second (30-tick) cycle.
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

int main(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGHUP, sig_handler);

    load_configuration(&config);

    if (setup_driver() != 0) {
        fprintf(stderr, "error: driver setup failed\n");
        return 1;
    }

    if (touch_open() != 0) {
        fprintf(stderr, "error: cannot open %s\n", touch_file);
        return 1;
    }

    FILE *pid_f = fopen(PID_FILE, "w");
    if (pid_f) {
        fprintf(pid_f, "%d\n", getpid());
        fclose(pid_f);
    }

    printf("Trinkey connected.\nLed active.\n");

    // tick drives blink and breath timing at ~10 Hz.
    unsigned int tick = 0;

    // Draw the very first frame before blocking in touch_wait()
    update_led(tick);

    while (keep_running) {
        if (reload_config) {
            load_configuration(&config);
            reload_config = 0;
        }

        /* Block indefinitely on static mode; a touch press/release still wakes us up
         * immediately via the interrupt-driven sysfs notification.
         */
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
    unlink(PID_FILE);

    return 0;
}

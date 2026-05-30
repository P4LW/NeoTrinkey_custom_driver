#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#define CONFIG_FILE "/etc/trinkey/config"
#define PID_FILE    "/tmp/trinkey.pid"

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


/* SIGNAL HANDLING                                                     */

static void sig_handler(int signo)
{
    if (signo == SIGINT)
        keep_running = 0;
    else if (signo == SIGHUP)
        reload_config = 1;
}


/* SYSFS I/O                                                          */

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

static int get_touch(void)
{
    FILE *f = fopen(touch_file, "r");
    if (!f) {
        keep_running = 0;
        return 0;
    }
    int val = 0;
    if (fscanf(f, "%d", &val) != 1)
        val = 0;
    fclose(f);
    return val;
}


/* CONFIGURATION                                                       */

static int load_configuration(AppConfig *cfg)
{
    /* Defaults used both as fallback on missing file and as initial values
     * before parsing, so unknown keys leave a sane state.
     */
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



/* DEVICE DISCOVERY AND DRIVER SETUP                                   */


/* Returns a pointer to a static buffer; valid until the next call. */
static char *find_trinkey_bus(void)
{
    static char bus_name[64];

    DIR *d = opendir("/sys/bus/usb/devices/");
    if (!d)
        return NULL;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        char path[512];
        snprintf(path, sizeof(path),
                 "/sys/bus/usb/devices/%s/idVendor", dir->d_name);

        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        char vendor[10];
        if (fgets(vendor, sizeof(vendor), f) && strstr(vendor, "239a")) {
            strncpy(bus_name, dir->d_name, sizeof(bus_name) - 1);
            bus_name[sizeof(bus_name) - 1] = '\0';
            fclose(f);
            closedir(d);
            return bus_name;
        }
        fclose(f);
    }

    closedir(d);
    return NULL;
}

static int setup_driver(const char *bus)
{
    char interface[128];
    snprintf(interface, sizeof(interface), "%s:1.0", bus);

    /* Unbind cdc_acm if it has claimed the interface. */
    char path[512];
    snprintf(path, sizeof(path),
             "/sys/bus/usb/drivers/cdc_acm/%s", interface);
    if (access(path, F_OK) == 0) {
        FILE *f = fopen("/sys/bus/usb/drivers/cdc_acm/unbind", "w");
        if (f) {
            fprintf(f, "%s", interface);
            fclose(f);
        } else {
            perror("warning: could not unbind cdc_acm");
        }
    }

    /* Bind our custom driver if not already bound. */
    snprintf(path, sizeof(path),
             "/sys/bus/usb/drivers/adafruit_trinkey_custom/%s", interface);
    if (access(path, F_OK) != 0) {
        FILE *f = fopen("/sys/bus/usb/drivers/adafruit_trinkey_custom/bind", "w");
        if (!f) {
            perror("error: could not bind adafruit_trinkey_custom");
            return -1;
        }
        fprintf(f, "%s", interface);
        fclose(f);
    }

    snprintf(led_file,   sizeof(led_file),
             "/sys/bus/usb/devices/%s/trinkey_led",   interface);
    snprintf(touch_file, sizeof(touch_file),
             "/sys/bus/usb/devices/%s/trinkey_touch", interface);

    /* Give the driver a moment to create sysfs entries. */
    usleep(100000);

    if (access(led_file, F_OK) != 0 || access(touch_file, F_OK) != 0)
        return -1;

    return 0;
}



/* MAIN LOOP                                                           */

int main(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGHUP, sig_handler);

    load_configuration(&config);

    char *bus = find_trinkey_bus();
    if (!bus) {
        fprintf(stderr, "error: device not found\n");
        return 1;
    }

    if (setup_driver(bus) != 0) {
        fprintf(stderr, "error: driver setup failed\n");
        return 1;
    }

    FILE *pid_f = fopen(PID_FILE, "w");
    if (pid_f) {
        fprintf(pid_f, "%d\n", getpid());
        fclose(pid_f);
    } else {
        perror("unable to create PID file (try sudo)");
    }

    printf("monitoring started (send SIGHUP to reload config)\n");

    /* tick drives blink and breath timing at 10 Hz.
     * unsigned avoids signed overflow after long uptimes.
     */
    unsigned int tick = 0;

    while (keep_running) {
        if (reload_config) {
            load_configuration(&config);
            reload_config = 0;
        }

        if (get_touch()) {
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
                    /* 0.5 Hz blink: on for 5 ticks, off for 5 ticks. */
                    if ((tick % 10) < 5)
                        set_led(config.color_idle[0],
                                config.color_idle[1],
                                config.color_idle[2]);
                    else
                        set_led(0, 0, 0);
                break;

                case MODE_BREATH:
                    /* Sine-based fade over a 3-second (30-tick) cycle. */
                    {
                        double radians  = (tick % 30) * (2.0 * M_PI / 30.0);
                        double intensity = (sin(radians - M_PI / 2.0) + 1.0) / 2.0;
                        set_led((int)(config.color_idle[0] * intensity),
                                (int)(config.color_idle[1] * intensity),
                                (int)(config.color_idle[2] * intensity));
                    }
                    break;
            }
        }

        tick++;
        usleep(100000); /* 10 Hz */
    }

    printf("\nexiting, turning off LED\n");
    set_led(0, 0, 0);
    unlink(PID_FILE);
    return 0;
}

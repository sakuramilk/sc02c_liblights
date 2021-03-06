/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2011 sakuramilk <c.sakuramilk@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights"
#define LOG_NDEBUG 0

#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct light_state_t g_notification;
static int g_backlight = 255;
static int g_buttons = 0;

/* GENERIC_BLN */
#define BLN_LIGHT_ON    (1)
#define BLN_LIGHT_OFF   (2)
#define BLN_NOTIFY_ON   (1)
#define BLN_NOTIFY_OFF  (0)

/* CM7 LED NOTIFICATIONS BACKLIGHT */
#define CM7_ENABLE_BL   (1)
#define CM7_DISABLE_BL  (2)

char const*const LCD_FILE
        = "/sys/class/backlight/pwm-backlight/brightness";

char const*const KEYBOARD_FILE
        = "/sys/class/leds/keyboard-backlight/brightness";

char const*const BUTTON_FILE
        = "/sys/class/misc/melfas_touchkey/brightness";

char const*const NOTIFICATION_FILE
        = "/sys/class/misc/backlightnotification/notification_led";

char const*const CM7_NOTIFICATION_FILE
        = "/sys/class/misc/notification/led";

/**
 * device methods
 */

void init_globals(void)
{
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);
}

static int write_int(char const *path, int value)
{
    int fd;
    static int already_warned = 0;

    LOGV("write_int : path %s, value %d", path, value);
    fd = open(path, O_RDWR);
    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            LOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int is_lit(struct light_state_t const *state)
{
    return state->color & 0xffffffff;
}

static int rgb_to_brightness(struct light_state_t const *state)
{
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16) & 0x00ff))
            + (150*((color>>8) & 0x00ff)) + (29*(color & 0x00ff))) >> 8;
}

static int set_light_backlight(struct light_device_t *dev, struct light_state_t const *state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    pthread_mutex_lock(&g_lock);
    g_backlight = brightness;
    err = write_int(LCD_FILE, brightness);
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int set_light_buttons(struct light_device_t *dev, struct light_state_t const *state)
{
    int err = 0;
    int on = is_lit(state);
    pthread_mutex_lock(&g_lock);
    g_buttons = on;
    /* for BLN 1(on) or 2(off) */
    err = write_int(BUTTON_FILE, on ?
        BLN_LIGHT_ON :
        BLN_LIGHT_OFF);
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int set_light_notifications(struct light_device_t *dev, struct light_state_t const *state)
{
    pthread_mutex_lock(&g_lock);

    g_notification = *state;
    LOGE("set_light_notifications color=0x%08x", state->color);

    int err = write_int( NOTIFICATION_FILE, is_lit(state) ?
                    BLN_NOTIFY_ON :
                    BLN_NOTIFY_OFF);

    int brightness = rgb_to_brightness(state);
        
    if (brightness+state->color == 0 || brightness > 100 ) {
        if (state->color & 0x00ffffff) {
            LOGV("[LED Notify] set_light_notifications - ENABLE_BL\n");
            err = write_int (CM7_NOTIFICATION_FILE, CM7_ENABLE_BL);
        } else {
            LOGV("[LED Notify] set_light_notifications - DISABLE_BL\n");
            err = write_int (CM7_NOTIFICATION_FILE, CM7_DISABLE_BL);
        }
    }

    pthread_mutex_unlock(&g_lock);

    return 0;
}

static int close_lights(struct light_device_t *dev)
{
    LOGV("close_light is called");
    if (dev)
        free(dev);

    return 0;
}


/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    LOGE("CMD name =>%s\n", name);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name)) {
        set_light = set_light_backlight;
    }
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name)) {
        set_light = set_light_buttons;
    }
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name)) {
        set_light = set_light_notifications;
    }
    else {
        return -EINVAL;
    }

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

/*
 * The lights Module
 */
const struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "SC-02C lights Module",
    .author = "sakuramilk",
    .methods = &lights_module_methods,
};

/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/stp_indicators.h>

#include <zmk/activity.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/battery.h>
#include <zmk/hid_indicators.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_indicators)

#error "A zmk,indicators chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_indicators)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

struct zmk_stp_ble {
    uint8_t prof;
    bool open;
    bool connected;
};

static struct zmk_led_hsb color0; // BLE Led
static struct zmk_led_hsb color1; // Caps

static struct zmk_stp_ble ble_status;
static bool caps;
static bool usb;
static bool battery;
static bool events_en;

static bool on;

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];

static const struct device *ext_power;

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r, g, b;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

static void zmk_stp_indicators_batt(struct k_work *work) {
    // Get state of charge
    uint8_t soc = zmk_battery_state_of_charge();
    LOG_DBG("State of charge: %d", soc);
    struct led_rgb rgb;
    if (soc > 80) {
        rgb.r = 0;
        rgb.g = 255;
        rgb.b = 0;
    } else if (soc > 50 && soc < 80) {
        rgb.r = 255;
        rgb.g = 255;
        rgb.b = 0;
    } else if (soc > 20 && soc < 51) {
        rgb.r = 255;
        rgb.g = 140;
        rgb.b = 0;
    } else {
        rgb.r = 255;
        rgb.g = 0;
        rgb.b = 0;
    }
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

static void zmk_stp_indicators_blink_work(struct k_work *work) {
    LOG_DBG("Blink work triggered");
    // If LED on turn off and vice cersa
    if (color0.b)
        color0.b = 0;
    else
        color0.b = CONFIG_ZMK_STP_INDICATORS_BRT_MAX;
    // Convert HSB to RGB and update LEDs
    pixels[0] = hsb_to_rgb(color0);
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(blink_work, zmk_stp_indicators_blink_work);

static void zmk_stp_indicators_blink_handler(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &blink_work);
}

// Define timers for blinking and led timeout
K_TIMER_DEFINE(fast_blink_timer, zmk_stp_indicators_blink_handler, NULL);
K_TIMER_DEFINE(slow_blink_timer, zmk_stp_indicators_blink_handler, NULL);
K_TIMER_DEFINE(connected_timeout_timer, zmk_stp_indicators_blink_handler, NULL);

static void zmk_stp_indicators_bluetooth(struct k_work *work) {
    // Set LED to blue if profile one, set sat to 0 if profile 0 (white)
    LOG_DBG("BLE PROFILE: %d", ble_status.prof);

    if (ble_status.prof) {
        color0.h = 240;
        color0.s = 100;
    } else
        color0.s = 0;
    // If in USB HID mode
    if (usb) {
        LOG_DBG("USB MODE");
        // Stop all timers
        k_timer_stop(&slow_blink_timer);
        k_timer_stop(&fast_blink_timer);
        k_timer_stop(&connected_timeout_timer);
        // Set LED to green
        color0.h = 120;
        color0.s = 100;
        color0.b = CONFIG_ZMK_STP_INDICATORS_BRT_MAX;
    } else if (ble_status.open) {
        LOG_DBG("BLE PROF OPEN");
        // If profile is open (unpaired) start fast blink timer and ensure LED turns on
        color0.b = CONFIG_ZMK_STP_INDICATORS_BRT_MAX;
        k_timer_stop(&slow_blink_timer);
        k_timer_stop(&connected_timeout_timer);
        k_timer_start(&fast_blink_timer, K_NO_WAIT, K_MSEC(200));
    } else if (!ble_status.connected) {
        LOG_DBG("BLE PROF NOT CONN");
        // If profile paired but not connected start slow blink timer and ensure LED on
        color0.b = CONFIG_ZMK_STP_INDICATORS_BRT_MAX;
        k_timer_stop(&fast_blink_timer);
        k_timer_stop(&connected_timeout_timer);
        k_timer_start(&slow_blink_timer, K_NO_WAIT, K_MSEC(750));
    } else {
        LOG_DBG("BLE PROF CONN");
        // If connected start the 3 second timeout to turn LED off
        color0.b = CONFIG_ZMK_STP_INDICATORS_BRT_MAX;
        k_timer_stop(&slow_blink_timer);
        k_timer_stop(&fast_blink_timer);
        k_timer_start(&connected_timeout_timer, K_SECONDS(3), K_NO_WAIT);
    }
    // Convert HSB to RGB and update the LEDs

    pixels[0] = hsb_to_rgb(color0);
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

static void zmk_stp_indicators_caps(struct k_work *work) {
    if (usb) {
        color1.h = 120;
        color1.s = 100;
    } else if (ble_status.prof) {
        color1.h = 240;
        color1.s = 100;
    } else
        color1.s = 0;
    // Set LED on if capslock pressed
    if (caps)
        color1.b = CONFIG_ZMK_STP_INDICATORS_BRT_MAX;
    else
        color1.b = 0;
    // Convert HSB to RGB and update the LEDs
    pixels[1] = hsb_to_rgb(color1);
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

// Define work to update LEDs
K_WORK_DEFINE(battery_ind_work, zmk_stp_indicators_batt);
K_WORK_DEFINE(bluetooth_ind_work, zmk_stp_indicators_bluetooth);
K_WORK_DEFINE(caps_ind_work, zmk_stp_indicators_caps);

int zmk_stp_indicators_enable_batt() {
    // Stop blinking timers
    k_timer_stop(&slow_blink_timer);
    k_timer_stop(&fast_blink_timer);
    k_timer_stop(&connected_timeout_timer);
    // Set battery flag to prevent other things overriding
    battery = true;
    // Submit battery work to queue
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &battery_ind_work);
    return 0;
}
int zmk_stp_indicators_disable_batt() {
    // Unset battery flag to allow other events to override
    battery = false;
    // Submit works to update both LEDs
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &bluetooth_ind_work);
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &caps_ind_work);
    return 0;
}

static int zmk_stp_indicators_resample(void) {
    int rc = ext_power_enable(ext_power);
    if (rc != 0) {
        LOG_ERR("Unable to enable EXT_POWER: %d", rc);
    }

    ble_status = (struct zmk_stp_ble){
        prof : zmk_ble_active_profile_index(),
        open : zmk_ble_active_profile_is_open(),
        connected : zmk_ble_active_profile_is_connected()
    };
    caps = (zmk_hid_indicators_get_current_profile() & ZMK_LED_CAPSLOCK_BIT);
    usb = zmk_usb_is_powered();

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &bluetooth_ind_work);
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &caps_ind_work);
}

static void zmk_stp_indicators_resample_work(struct k_work *work) {
    LOG_DBG("Resample work triggered");
    zmk_stp_indicators_resample();
}

K_WORK_DELAYABLE_DEFINE(resample_work, zmk_stp_indicators_resample_work);

static int zmk_stp_indicators_init(void) {

    LOG_DBG("Initialising STP indicators");

    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

    ext_power = device_get_binding("EXT_POWER");
    if (ext_power == NULL) {
        LOG_ERR("Unable to retrieve ext_power device: EXT_POWER");
    }

    int rc = ext_power_enable(ext_power);
    if (rc != 0) {
        LOG_ERR("Unable to enable EXT_POWER: %d", rc);
    }

    color0 = (struct zmk_led_hsb){
        h : 240,
        s : 100,
        b : CONFIG_ZMK_STP_INDICATORS_BRT_MAX,
    };

    color1 = (struct zmk_led_hsb){
        h : 240,
        s : 0,
        b : CONFIG_ZMK_STP_INDICATORS_BRT_MAX,
    };

    ble_status = (struct zmk_stp_ble){
        prof : zmk_ble_active_profile_index(),
        open : zmk_ble_active_profile_is_open(),
        connected : zmk_ble_active_profile_is_connected()
    };
    caps = (zmk_hid_indicators_get_current_profile() & ZMK_LED_CAPSLOCK_BIT);
    usb = false;
    battery = false;

    on = true;
    // Enable events

    k_work_schedule(&resample_work, K_MSEC(500));

    if (!events_en)
        events_en = true;

    return 0;
}

int zmk_stp_indicators_on() {
    if (!led_strip)
        return -ENODEV;

    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &bluetooth_ind_work);
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &caps_ind_work);

    return 0;
}

static void zmk_stp_indicators_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_off_work, zmk_stp_indicators_off_handler);

int zmk_stp_indicators_off() {
    if (!led_strip)
        return -ENODEV;

    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);
    on = false;

    return 0;
}

static int stp_indicators_auto_state(bool *prev_state, bool new_state) {
    if (on == new_state) {
        return 0;
    }
    if (new_state) {
        on = *prev_state;
        *prev_state = false;
        return zmk_stp_indicators_on();
    } else {
        on = false;
        *prev_state = true;
        return zmk_stp_indicators_off();
    }
}

static int stp_indicators_event_listener(const zmk_event_t *eh) {
    // If going idle or waking up
    if (as_zmk_activity_state_changed(eh) && events_en) {
        static bool prev_state = false;
        return stp_indicators_auto_state(&prev_state,
                                         zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
    // If USB state changed
    if (as_zmk_usb_conn_state_changed(eh) && events_en) {

        // Get new USB state, HID state and set local flags
        usb = zmk_usb_is_powered();
        LOG_DBG("USB EVENT: %d", usb);

        caps = (zmk_hid_indicators_get_current_profile() & ZMK_LED_CAPSLOCK_BIT);
        //  Update LEDs
        //
        if (!battery) {
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &bluetooth_ind_work);
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &caps_ind_work);
        }
        return 0;
    }

    // If BLE state changed
    if (as_zmk_ble_active_profile_changed(eh) && events_en) {
        LOG_DBG("BLE CHANGE LOGGED");
        // Get BLE information, Caps state and set local flags
        struct zmk_ble_active_profile_changed *ble_state = as_zmk_ble_active_profile_changed(eh);
        ble_status.connected = ble_state->connected;
        ble_status.open = ble_state->open;
        ble_status.prof = ble_state->index;
        caps = (zmk_hid_indicators_get_current_profile() & ZMK_LED_CAPSLOCK_BIT);
        // Update LEDs
        if (!battery) {
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &bluetooth_ind_work);
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &caps_ind_work);
        }
        return 0;
    }

    if (as_zmk_hid_indicators_changed(eh) && events_en) {
        // Get new HID state, set local flags
        caps = (zmk_hid_indicators_get_current_profile() & ZMK_LED_CAPSLOCK_BIT);
        LOG_DBG("INDICATOR CHANGED: %d", caps);
        if (!battery) {
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &caps_ind_work);
            // k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &bluetooth_ind_work);
        }
        return 0;
    }

    return -ENOTSUP;
}

ZMK_LISTENER(stp_indicators, stp_indicators_event_listener);

ZMK_SUBSCRIPTION(stp_indicators, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(stp_indicators, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(stp_indicators, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(stp_indicators, zmk_hid_indicators_changed);

SYS_INIT(zmk_stp_indicators_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

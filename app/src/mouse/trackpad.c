#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <zmk/mouse/trackpad.h>
#include <zmk/mouse/hid.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/sensor_event.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include "drivers/sensor/gen4.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

const struct device *trackpad = DEVICE_DT_GET(DT_INST(0, cirque_gen4));

static zmk_trackpad_finger_contacts_t present_contacts = 0;
static zmk_trackpad_finger_contacts_t contacts_to_send = 0;
static zmk_trackpad_finger_contacts_t received_contacts = 0;

static uint8_t btns;
static uint16_t scantime;

static bool trackpad_enabled;

static bool mousemode;
static bool surface_mode;
static bool button_mode;

static int16_t xDelta, yDelta, scrollDelta;

static struct zmk_ptp_finger fingers[CONFIG_ZMK_TRACKPAD_FINGERS];
static const struct zmk_ptp_finger empty_finger = {0};

static bool mouse_modes[ZMK_ENDPOINT_COUNT] = {0};
static bool surface_modes[ZMK_ENDPOINT_COUNT] = {0};
static bool button_modes[ZMK_ENDPOINT_COUNT] = {0};

static void handle_trackpad_ptp(const struct device *dev, const struct sensor_trigger *trig) {
    int ret = sensor_sample_fetch(dev);
    if (ret < 0) {
        LOG_ERR("fetch: %d", ret);
        return;
    }
    // LOG_DBG("Trackpad handler trigd %d", 0);

    struct sensor_value contacts, confidence_tip, id, x, y, buttons, scan_time;
    sensor_channel_get(dev, SENSOR_CHAN_CONTACTS, &contacts);
    sensor_channel_get(dev, SENSOR_CHAN_BUTTONS, &buttons);
    sensor_channel_get(dev, SENSOR_CHAN_SCAN_TIME, &scan_time);

    present_contacts = contacts.val1 ? contacts.val1 : present_contacts;
    // Buttons and scan time
    btns = button_mode ? buttons.val1 : 0;
    scantime = scan_time.val1;
    // released Fingers
    sensor_channel_get(dev, SENSOR_CHAN_X, &x);
    sensor_channel_get(dev, SENSOR_CHAN_Y, &y);
    sensor_channel_get(dev, SENSOR_CHAN_CONFIDENCE_TIP, &confidence_tip);
    sensor_channel_get(dev, SENSOR_CHAN_FINGER, &id);
    // If finger has changed
    LOG_DBG("Confidence tip: %d", confidence_tip.val1);
    fingers[id.val1].touch_valid = confidence_tip.val1 & 0x01;
    fingers[id.val1].tip_switch = (confidence_tip.val1 & 0x02) >> 1;
    fingers[id.val1].contact_id = id.val1;
    fingers[id.val1].x =
        x.val1 < CONFIG_ZMK_TRACKPAD_LOGICAL_X ? x.val1 : CONFIG_ZMK_TRACKPAD_LOGICAL_X;
    fingers[id.val1].y =
        y.val1 < CONFIG_ZMK_TRACKPAD_LOGICAL_Y ? y.val1 : CONFIG_ZMK_TRACKPAD_LOGICAL_Y;
    contacts_to_send |= BIT(id.val1);
    received_contacts++;

    // LOG_DBG("total contacts: %d, received contacts: %d", present_contacts, received_contacts);

    if ((present_contacts == received_contacts) && surface_mode) {
        LOG_DBG("total contacts: %d, received contacts: %d, bitmap contacts %d", present_contacts,
                received_contacts, contacts_to_send);
#if CONFIG_ZMK_TRACKPAD_FINGERS == 5
        zmk_hid_ptp_set((contacts_to_send & BIT(0)) ? fingers[0] : empty_finger,
                        (contacts_to_send & BIT(1)) ? fingers[1] : empty_finger,
                        (contacts_to_send & BIT(2)) ? fingers[2] : empty_finger,
                        (contacts_to_send & BIT(3)) ? fingers[3] : empty_finger,
                        (contacts_to_send & BIT(4)) ? fingers[4] : empty_finger, present_contacts,
                        scantime, button_mode ? btns : 0);
#elif CONFIG_ZMK_TRACKPAD_FINGERS == 4
        zmk_hid_ptp_set((contacts_to_send & BIT(0)) ? fingers[0] : empty_finger,
                        (contacts_to_send & BIT(1)) ? fingers[1] : empty_finger,
                        (contacts_to_send & BIT(2)) ? fingers[2] : empty_finger,
                        (contacts_to_send & BIT(3)) ? fingers[3] : empty_finger, empty_finger,
                        present_contacts, scantime, button_mode ? btns : 0);
#else
        zmk_hid_ptp_set((contacts_to_send & BIT(0)) ? fingers[0] : empty_finger,
                        (contacts_to_send & BIT(1)) ? fingers[1] : empty_finger,
                        (contacts_to_send & BIT(2)) ? fingers[2] : empty_finger, empty_finger,
                        empty_finger, present_contacts, scantime, button_mode ? btns : 0);
#endif
        zmk_endpoints_send_ptp_report();
        contacts_to_send = 0;
        received_contacts = 0;
    } else if (!surface_mode) {
        zmk_hid_ptp_set(empty_finger, empty_finger, empty_finger, empty_finger, empty_finger, 0,
                        scantime, button_mode ? btns : 0);
        zmk_endpoints_send_ptp_report();
        contacts_to_send = 0;
        received_contacts = 0;
    }

    raise_zmk_sensor_event(
        (struct zmk_sensor_event){.sensor_index = 0,
                                  .channel_data_size = 1,
                                  .channel_data = {(struct zmk_sensor_channel_data){
                                      .value = buttons, .channel = SENSOR_CHAN_BUTTONS}},
                                  .timestamp = k_uptime_get()});
}

static void handle_mouse_mode(const struct device *dev, const struct sensor_trigger *trig) {
    LOG_DBG("Trackpad handler trigd in mouse mode %d", 0);
    int ret = sensor_sample_fetch(dev);
    if (ret < 0) {
        LOG_ERR("fetch: %d", ret);
        return;
    }

    struct sensor_value x, y, buttons, wheel;
    sensor_channel_get(dev, SENSOR_CHAN_XDELTA, &x);
    sensor_channel_get(dev, SENSOR_CHAN_YDELTA, &y);
    sensor_channel_get(dev, SENSOR_CHAN_BUTTONS, &buttons);
    sensor_channel_get(dev, SENSOR_CHAN_WHEEL, &wheel);

    btns = buttons.val1;
    xDelta = x.val1;
    yDelta = y.val1;
#if IS_ENABLED(CONFIG_ZMK_TRACKPAD_REVERSE_SCROLL)
    scrollDelta = -wheel.val1;
#else
    scrollDelta = wheel.val1;
#endif
    zmk_hid_mouse_set(btns, xDelta, yDelta, scrollDelta);
    zmk_endpoints_send_mouse_report();

    raise_zmk_sensor_event(
        (struct zmk_sensor_event){.sensor_index = 0,
                                  .channel_data_size = 1,
                                  .channel_data = {(struct zmk_sensor_channel_data){
                                      .value = buttons, .channel = SENSOR_CHAN_BUTTONS}},
                                  .timestamp = k_uptime_get()});
}

static void zmk_trackpad_set_mouse_mode(bool mouse_mode) {
    struct sensor_trigger trigger = {
        .type = SENSOR_TRIG_DATA_READY,
        .chan = SENSOR_CHAN_ALL,
    };
    struct sensor_value attr;
    attr.val1 = mouse_mode;
    LOG_DBG("Setting attr %d", attr.val1);
    mousemode = mouse_mode;
    sensor_attr_set(trackpad, SENSOR_CHAN_ALL, SENSOR_ATTR_CONFIGURATION, &attr);
    if (mouse_mode) {
        zmk_hid_ptp_set(empty_finger, empty_finger, empty_finger, empty_finger, empty_finger, 0,
                        scantime + 1, 0);
        zmk_endpoints_send_ptp_report();
        if (sensor_trigger_set(trackpad, &trigger, handle_mouse_mode) < 0) {
            LOG_ERR("can't set trigger mouse mode");
        };
    } else {
        zmk_hid_mouse_clear();
        zmk_endpoints_send_mouse_report();
        if (sensor_trigger_set(trackpad, &trigger, handle_trackpad_ptp) < 0) {
            LOG_ERR("can't set trigger");
        };
    }
}

void zmk_trackpad_set_enabled(bool enabled) {
    if (trackpad_enabled == enabled)
        return;
    struct sensor_trigger trigger = {
        .type = SENSOR_TRIG_DATA_READY,
        .chan = SENSOR_CHAN_ALL,
    };
    trackpad_enabled = enabled;
    if (trackpad_enabled) {
        // Activate everything
        if (mousemode) {
            if (sensor_trigger_set(trackpad, &trigger, handle_mouse_mode) < 0) {
                LOG_ERR("can't set trigger mouse mode");
            };
        } else {
            if (sensor_trigger_set(trackpad, &trigger, handle_trackpad_ptp) < 0) {
                LOG_ERR("can't set trigger");
            };
        }
    } else {
        // Clear reports, stop trigger
        if (mousemode) {
            zmk_hid_mouse_clear();
            zmk_endpoints_send_mouse_report();
        } else {
            zmk_hid_ptp_set(empty_finger, empty_finger, empty_finger, empty_finger, empty_finger, 0,
                            scantime + 1, 0);
            zmk_endpoints_send_ptp_report();
        }
        if (sensor_trigger_set(trackpad, &trigger, NULL) < 0) {
            LOG_ERR("can't unset trigger");
        };
    }
}

bool zmk_trackpad_get_enabled() { return trackpad_enabled; }

static void process_mode_report(struct k_work *_work) {
    bool state = mouse_modes[zmk_endpoint_instance_to_index(zmk_endpoints_selected())];
    LOG_DBG("Current state %d, new state %d", mousemode, state);
    if (mousemode != state) {
        LOG_DBG("Setting mouse mode to %d for endpoint %d", state,
                zmk_endpoint_instance_to_index(zmk_endpoints_selected()));
        zmk_trackpad_set_mouse_mode(state);
        zmk_mouse_hid_ptp_set_feature_mode(state ? 0 : 3);
    }
}

static K_WORK_DEFINE(mode_changed_work, process_mode_report);

static void process_selective_report(struct k_work *_work) {
    surface_mode = surface_modes[zmk_endpoint_instance_to_index(zmk_endpoints_selected())];
    button_mode = button_modes[zmk_endpoint_instance_to_index(zmk_endpoints_selected())];
    zmk_mouse_hid_ptp_set_feature_selective_report(surface_mode, button_mode);
}

static K_WORK_DEFINE(selective_changed_work, process_selective_report);

static int trackpad_init(void) {

    for (int i = 0; i < ZMK_ENDPOINT_COUNT; i++) {
        mouse_modes[i] = true;
        surface_modes[i] = true;
        button_modes[i] = true;
    }
    k_work_submit(&mode_changed_work);
    k_work_submit(&selective_changed_work);
    trackpad_enabled = true;
    return 0;
}

static int trackpad_event_listener(const zmk_event_t *eh) {
    // Reset to mouse mode on usb disconnection
    if (as_zmk_usb_conn_state_changed(eh)) {
        struct zmk_usb_conn_state_changed *usb_state = as_zmk_usb_conn_state_changed(eh);
        if (usb_state->conn_state == ZMK_USB_CONN_NONE) {
            struct zmk_endpoint_instance endpoint = {
                .transport = ZMK_TRANSPORT_USB,
            };
            mouse_modes[zmk_endpoint_instance_to_index(endpoint)] = true;
            surface_modes[zmk_endpoint_instance_to_index(endpoint)] = true;
            button_modes[zmk_endpoint_instance_to_index(endpoint)] = true;
        }
    }
    // reset to mouse mode on BLE profile disconnection or unpairing
    if (as_zmk_ble_active_profile_changed(eh)) {
        struct zmk_ble_active_profile_changed *ble_state = as_zmk_ble_active_profile_changed(eh);
        if (ble_state->open || !ble_state->connected) {
            struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_BLE,
                                                     .ble = {.profile_index = ble_state->index}};
            mouse_modes[zmk_endpoint_instance_to_index(endpoint)] = true;
            surface_modes[zmk_endpoint_instance_to_index(endpoint)] = true;
            button_modes[zmk_endpoint_instance_to_index(endpoint)] = true;
        }
    }
    k_work_submit(&mode_changed_work);
    k_work_submit(&selective_changed_work);
    LOG_DBG("Mode change evt triggered");
    return 0;
}

ZMK_LISTENER(trackpad, trackpad_event_listener);
ZMK_SUBSCRIPTION(trackpad, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(trackpad, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(trackpad, zmk_ble_active_profile_changed);

void zmk_trackpad_set_mode_report(uint8_t *report, struct zmk_endpoint_instance endpoint) {
    int profile = zmk_endpoint_instance_to_index(endpoint);
    LOG_DBG("Received report %d on endpoint %d", *report, profile);
    mouse_modes[profile] = *report ? false : true;
    k_work_submit(&mode_changed_work);
}

void zmk_trackpad_set_selective_report(bool surface_switch, bool button_switch,
                                       struct zmk_endpoint_instance endpoint) {
    surface_modes[zmk_endpoint_instance_to_index(endpoint)] = surface_switch;
    button_modes[zmk_endpoint_instance_to_index(endpoint)] = button_switch;
    LOG_DBG("Surface: %d, Button %d", surface_mode, button_mode);
    k_work_submit(&selective_changed_work);
}

SYS_INIT(trackpad_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
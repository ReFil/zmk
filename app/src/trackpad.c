#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/sensor_event.h>
#include "drivers/sensor/gen4.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

const struct device *trackpad = DEVICE_DT_GET(DT_INST(0, cirque_gen4));

static uint8_t btns;

static int8_t xDelta, yDelta, scrollDelta;

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

static void zmk_trackpad_set_mouse_mode(void) {
    struct sensor_trigger trigger = {
        .type = SENSOR_TRIG_DATA_READY,
        .chan = SENSOR_CHAN_ALL,
    };
    struct sensor_value attr;
    attr.val1 = true;
    LOG_DBG("Setting attr %d", attr.val1);
    sensor_attr_set(trackpad, SENSOR_CHAN_ALL, SENSOR_ATTR_CONFIGURATION, &attr);
    if (sensor_trigger_set(trackpad, &trigger, handle_mouse_mode) < 0) {
        LOG_ERR("can't set trigger mouse mode");
    };
}

static int trackpad_init(void) {

    zmk_trackpad_set_mouse_mode();
    return 0;
}

SYS_INIT(trackpad_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
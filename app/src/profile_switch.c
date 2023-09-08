#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include <zmk/ble.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_CHOSEN(zmk_profileswitch),
             "CONFIG_ZMK_PROFILESWITCH is enabled but no zmk,profileswitch chosen node found");

struct gpio_callback a_gpio_cb;
const struct gpio_dt_spec switchgpio = GPIO_DT_SPEC_GET(DT_CHOSEN(zmk_profileswitch), switch_gpios);

static void zmk_profile_switch_read() {
    uint8_t val = gpio_pin_get_dt(&switchgpio);
    LOG_DBG("Setting BLE profile to %d", val);
    zmk_ble_prof_select(val);
    if (gpio_pin_interrupt_configure_dt(&switchgpio, GPIO_INT_EDGE_BOTH)) {
        LOG_WRN("Unable to set A pin GPIO interrupt");
    }
}

static void zmk_profile_switch_work_cb(struct k_work *work) { zmk_profile_switch_read(); }

K_WORK_DEFINE(profileswitch_work, zmk_profile_switch_work_cb);

static void zmk_profile_switch_callback(const struct device *dev, struct gpio_callback *cb,
                                        uint32_t pins) {

    if (gpio_pin_interrupt_configure_dt(&switchgpio, GPIO_INT_DISABLE)) {
        LOG_WRN("Unable to set A pin GPIO interrupt");
    }
    LOG_DBG("interrupt triggered");
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &profileswitch_work);
}

static int zmk_profile_switch_init(void) {

    if (!device_is_ready(switchgpio.port)) {
        LOG_ERR("A GPIO device is not ready");
        return -EINVAL;
    }
    if (gpio_pin_configure_dt(&switchgpio, GPIO_INPUT)) {
        LOG_DBG("Failed to configure A pin");
        return -EIO;
    }
    gpio_init_callback(&a_gpio_cb, zmk_profile_switch_callback, BIT(switchgpio.pin));

    if (gpio_add_callback(switchgpio.port, &a_gpio_cb) < 0) {
        LOG_DBG("Failed to set A callback!");
        return -EIO;
    }
    if (gpio_pin_interrupt_configure_dt(&switchgpio, GPIO_INT_EDGE_BOTH)) {
        LOG_WRN("Unable to set A pin GPIO interrupt");
    }
    LOG_DBG("Setting profile now");
    zmk_profile_switch_read();
    return 0;
}

SYS_INIT(zmk_profile_switch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
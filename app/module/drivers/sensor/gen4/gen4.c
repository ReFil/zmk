#define DT_DRV_COMPAT cirque_gen4

#include <zephyr/init.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <drivers/sensor/gen4.h>
#include "gen4.h"

LOG_MODULE_REGISTER(gen4, CONFIG_SENSOR_LOG_LEVEL);

static int gen4_normal_read(const struct device *dev, uint8_t *buf, const uint8_t len) {
    const struct gen4_config *config = dev->config;
    return i2c_read_dt(&config->bus, buf, len);
}

static int gen4_mouse_mode(const struct device *dev) {
    const struct gen4_config *cfg = dev->config;
    // enable mouse mode
    uint8_t request2[10] = {0x05, 0x00, 0x34, 0x03, 0x06, 0x00, 0x04, 0x00, 0x04, 0x00};
    int ret = i2c_write_dt(&cfg->bus, request2, 10);
    if (ret < 0) {
        LOG_ERR("ext read status: %d", ret);
        return ret;
    }
    return 0;
}

static int gen4_abs_mode(const struct device *dev) {
    const struct gen4_config *cfg = dev->config;
    // enable mouse mode
    uint8_t request2[10] = {0x05, 0x00, 0x34, 0x03, 0x06, 0x00, 0x04, 0x00, 0x04, 0x03};
    LOG_DBG("Entering abs mode");
    int ret = i2c_write_dt(&cfg->bus, request2, 10);
    if (ret < 0) {
        LOG_ERR("ext read status: %d", ret);
        return ret;
    }
    return 0;
}

static int gen4_i2c_init(const struct device *dev) {
    const struct gen4_config *cfg = dev->config;
    uint8_t request[2] = {0x20, 0x00};
    uint8_t buffer[30];
    int ret = i2c_write_read_dt(&cfg->bus, request, 2, buffer, 30);
    if (ret < 0) {
        LOG_ERR("ext read status: %d", ret);
        return ret;
    }
    LOG_DBG("received value %x", buffer[2]);

    return gen4_mouse_mode(dev);
}

static int gen4_attr_set(const struct device *dev, enum sensor_channel chan,
                         enum sensor_attribute attr, const struct sensor_value *val) {
    struct gen4_data *data = dev->data;
    data->mousemode = val->val1;
    switch (val->val1) {
    case 0:
        LOG_DBG("Entering abs mode");
        return gen4_abs_mode(dev);
        break;
    case 1:
        LOG_DBG("Entering mouse mode");
        return gen4_mouse_mode(dev);
        break;

    default:
        break;
    }
    return -EIO;
}

static int gen4_channel_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    const struct gen4_data *data = dev->data;
    switch ((enum sensor_channel_gen4)chan) {
    case SENSOR_CHAN_CONTACTS:
        val->val1 = data->contacts;
        break;
    case SENSOR_CHAN_X:
        val->val1 = data->finger.x;
        break;
    case SENSOR_CHAN_Y:
        val->val1 = data->finger.y;
        break;
    case SENSOR_CHAN_CONFIDENCE_TIP:
        val->val1 = data->finger.confidence_tip;
        break;
    case SENSOR_CHAN_FINGER:
        val->val1 = data->finger_id;
        break;
    case SENSOR_CHAN_SCAN_TIME:
        val->val1 = data->scan_time;
        break;
    case SENSOR_CHAN_BUTTONS:
        val->val1 = data->btns;
        break;
    case SENSOR_CHAN_XDELTA:
        val->val1 = data->mouse.xDelta;
        break;
    case SENSOR_CHAN_YDELTA:
        val->val1 = data->mouse.yDelta;
        break;
    case SENSOR_CHAN_WHEEL:
        val->val1 = data->mouse.scrollDelta;
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static int gen4_sample_fetch(const struct device *dev, enum sensor_channel) {
    uint8_t packet[53];
    int ret;
    ret = gen4_normal_read(dev, packet, 52);
    if (ret < 0) {
        LOG_ERR("read status: %d, retrying", ret);
        ret = gen4_normal_read(dev, packet, 52);
        if (ret != 0)
            return ret;
    }

    struct gen4_data *data = dev->data;

    if (data->mousemode) {
        if (!(packet[REPORT_ID_SHIFT] == MOUSE_REPORT_ID)) {
            return -EAGAIN;
        }
        data->btns = packet[3];
        data->mouse.xDelta = packet[4];
        data->mouse.yDelta = packet[5];
        data->mouse.scrollDelta = packet[6];
    } else {
        if (!(packet[REPORT_ID_SHIFT] == PTP_REPORT_ID)) {
            return -EAGAIN;
        }

        uint16_t report_length =
            packet[LENGTH_LOWBYTE_SHIFT] | (packet[LENGTH_HIGHBYTE_SHIFT] << 8);

        if (report_length == 12) {
            data->scan_time = (uint16_t)packet[8] | (uint16_t)(packet[9] << 8);
            data->contacts = packet[10];
            data->btns = packet[11];
        } else {
            data->scan_time = (uint16_t)packet[9] | (uint16_t)(packet[10] << 8);
            data->contacts = packet[11];
            data->btns = packet[12];
        }

        data->finger_id = (packet[3] & 0xFC) >> 2;
        // LOG_DBG("FINGER ID: %d", data->finger_id);
        //   Finger data
        data->finger.confidence_tip = (packet[3] & 0x03);
        data->finger.x = (uint16_t)packet[4] | (uint16_t)(packet[5] << 8);
        data->finger.y = (uint16_t)packet[6] | (uint16_t)(packet[7] << 8);

        // LOG_DBG("Finger palm/detected: %d", data->finger.confidence_tip);
        // LOG_DBG("Finger x: %d", data->finger.x);
        // LOG_DBG("Finger y: %d", data->finger.y);
    }
    return 0;
}

#ifdef CONFIG_GEN4_TRIGGER
static void set_int(const struct device *dev, const bool en) {
    const struct gen4_config *config = dev->config;
    int ret =
        gpio_pin_interrupt_configure_dt(&config->dr, en ? GPIO_INT_LEVEL_ACTIVE : GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_ERR("can't set interrupt");
    }
}

static int gen4_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
                            sensor_trigger_handler_t handler) {
    struct gen4_data *data = dev->data;

    set_int(dev, false);
    if (trig->type != SENSOR_TRIG_DATA_READY) {
        return -ENOTSUP;
    }
    data->data_ready_trigger = trig;
    data->data_ready_handler = handler;
    set_int(dev, true);
    return 0;
}

static void gen4_int_cb(const struct device *dev) {
    struct gen4_data *data = dev->data;

    // LOG_DBG("Gen4 interrupt trigd: %d", 0);
    data->data_ready_handler(dev, data->data_ready_trigger);
    LOG_DBG("Setting int on %d", 0);
    set_int(dev, true);
}

#ifdef CONFIG_GEN4_TRIGGER_OWN_THREAD
static void gen4_thread(void *arg) {
    const struct device *dev = arg;
    struct gen4_data *data = dev->data;
    set_int(dev, false);
    while (1) {
        k_sem_take(&data->gpio_sem, K_FOREVER);
        gen4_int_cb(dev);
    }
}
#elif defined(CONFIG_GEN4_TRIGGER_GLOBAL_THREAD)
static void gen4_work_cb(struct k_work *work) {
    struct gen4_data *data = CONTAINER_OF(work, struct gen4_data, work);
    gen4_int_cb(data->dev);
}
#endif

static void gen4_gpio_cb(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    struct gen4_data *data = CONTAINER_OF(cb, struct gen4_data, gpio_cb);
    set_int(data->dev, false);
    LOG_DBG("Interrupt trigd");
#if defined(CONFIG_GEN4_TRIGGER_OWN_THREAD)
    k_sem_give(&data->gpio_sem);
#elif defined(CONFIG_GEN4_TRIGGER_GLOBAL_THREAD)
    k_work_submit(&data->work);
#endif
}
#endif

static int gen4_init(const struct device *dev) {
    struct gen4_data *data = dev->data;
    const struct gen4_config *config = dev->config;

    gen4_i2c_init(dev);
    gen4_sample_fetch(dev, 0);
#ifdef CONFIG_GEN4_TRIGGER
    data->dev = dev;
    gpio_pin_configure_dt(&config->dr, GPIO_INPUT);
    gpio_init_callback(&data->gpio_cb, gen4_gpio_cb, BIT(config->dr.pin));
    int ret = gpio_add_callback(config->dr.port, &data->gpio_cb);
    if (ret < 0) {
        LOG_ERR("Failed to set DR callback: %d", ret);
        return -EIO;
    }

#if defined(CONFIG_GEN4_TRIGGER_OWN_THREAD)
    k_sem_init(&data->gpio_sem, 0, UINT_MAX);

    k_thread_create(&data->thread, data->thread_stack, CONFIG_GEN4_THREAD_STACK_SIZE,
                    (k_thread_entry_t)gen4_thread, (void *)dev, 0, NULL,
                    K_PRIO_COOP(CONFIG_GEN4_THREAD_PRIORITY), 0, K_NO_WAIT);
#elif defined(CONFIG_GEN4_TRIGGER_GLOBAL_THREAD)
    k_work_init(&data->work, gen4_work_cb);
#endif
    set_int(dev, true);
#endif
    return 0;
}

static const struct sensor_driver_api gen4_driver_api = {
#if CONFIG_GEN4_TRIGGER
    .trigger_set = gen4_trigger_set,
#endif
    .attr_set = gen4_attr_set,
    .sample_fetch = gen4_sample_fetch,
    .channel_get = gen4_channel_get,
};

#define GEN4_INST(n)                                                                               \
    static struct gen4_data gen4_data_##n;                                                         \
    static const struct gen4_config gen4_config_##n = {                                            \
        .bus = I2C_DT_SPEC_INST_GET(n),                                                            \
        COND_CODE_1(CONFIG_GEN4_TRIGGER, (.dr = GPIO_DT_SPEC_GET(DT_DRV_INST(0), dr_gpios), ),     \
                    ())};                                                                          \
    DEVICE_DT_INST_DEFINE(n, gen4_init, NULL, &gen4_data_##n, &gen4_config_##n, POST_KERNEL,       \
                          CONFIG_SENSOR_INIT_PRIORITY, &gen4_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GEN4_INST)
/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/settings/settings.h>
#include <zephyr/init.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>

#include <zmk/ble.h>
#include <zmk/endpoints_types.h>
#include <zmk/mouse/hog.h>
#include <zmk/mouse/hid.h>

enum {
    HIDS_REMOTE_WAKE = BIT(0),
    HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

struct hids_info {
    uint16_t version; /* version number of base USB HID Specification */
    uint8_t code;     /* country HID Device hardware is localized for. */
    uint8_t flags;
} __packed;

struct hids_report {
    uint8_t id;   /* report id */
    uint8_t type; /* report type */
} __packed;

static struct hids_info info = {
    .version = 0x1101,
    .code = 0x00,
    .flags = HIDS_NORMALLY_CONNECTABLE | HIDS_REMOTE_WAKE,
};

enum {
    HIDS_INPUT = 0x01,
    HIDS_OUTPUT = 0x02,
    HIDS_FEATURE = 0x03,
};

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD)

#include <zmk/mouse/trackpad.h>

// Windows PTP Collection

static struct hids_report ptp_input = {
    .id = ZMK_MOUSE_HID_REPORT_ID_DIGITIZER,
    .type = HIDS_INPUT,
};

static struct hids_report ptp_caps = {
    .id = ZMK_MOUSE_HID_REPORT_ID_FEATURE_PTP_CAPABILITIES,
    .type = HIDS_FEATURE,
};

static struct hids_report ptp_hqa = {
    .id = ZMK_MOUSE_HID_REPORT_ID_FEATURE_PTPHQA,
    .type = HIDS_FEATURE,
};

// Configuration Collection

static struct hids_report ptp_selective_reporting = {
    .id = ZMK_MOUSE_HID_REPORT_ID_FEATURE_PTP_SELECTIVE,
    .type = HIDS_FEATURE,
};

static struct hids_report ptp_mode = {
    .id = ZMK_MOUSE_HID_REPORT_ID_FEATURE_PTP_MODE,
    .type = HIDS_FEATURE,
};

#endif // IS_ENABLED(CONFIG_ZMK_TRACKPAD)

static bool host_requests_notification = false;
static uint8_t ctrl_point;

static ssize_t read_hids_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_info));
}

static ssize_t read_hids_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_report));
}

static ssize_t read_hids_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, zmk_mouse_hid_report_desc,
                             sizeof(zmk_mouse_hid_report_desc));
}

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD)

static ssize_t read_hids_ptp_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                          void *buf, uint16_t len, uint16_t offset) {
    struct zmk_hid_ptp_report_body *report_body = &zmk_mouse_hid_get_ptp_report()->body;
    LOG_DBG("Get PT Input at offset %d with len %d to fetch total size %d", offset, len,
            sizeof(struct zmk_hid_ptp_report_body));
    LOG_HEXDUMP_DBG(report_body, sizeof(struct zmk_hid_ptp_report_body), "PTP report");
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_ptp_report_body));
}

static ssize_t read_hids_ptp_caps_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         void *buf, uint16_t len, uint16_t offset) {
    LOG_DBG("Get CAPS");
    struct zmk_hid_ptp_feature_capabilities_report_body *report_body =
        &zmk_mouse_hid_ptp_get_feature_capabilities_report()->body;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, report_body,
                             sizeof(struct zmk_hid_ptp_feature_capabilities_report_body));
}

static ssize_t read_hids_ptp_hqa_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                        void *buf, uint16_t len, uint16_t offset) {
    struct zmk_hid_ptp_feature_certification_report *report =
        zmk_mouse_hid_ptp_get_feature_certification_report();
    LOG_DBG("Get HQA at offset %d with len %d to fetch total size %d", offset, len,
            sizeof(report->ptphqa_blob));
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &report->ptphqa_blob,
                             sizeof(report->ptphqa_blob));
}

static ssize_t read_hids_ptp_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                                  uint16_t len, uint16_t offset) {
    struct zmk_hid_ptp_feature_mode_report *report = zmk_mouse_hid_ptp_get_feature_mode_report();
    LOG_DBG("Get PTP MODE at offset %d with len %d", offset, len);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &report->mode, sizeof(uint8_t));
}

static ssize_t write_hids_ptp_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        LOG_ERR("Funky offset for mode");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(uint8_t)) {
        LOG_ERR("Wrong size for mode");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_BLE,
                                             .ble = {
                                                 .profile_index = profile,
                                             }};
    uint8_t *report = (uint8_t *)buf;

    LOG_DBG("Mode report set ble %d", *report);
    zmk_trackpad_set_mode_report(report, endpoint);

    return len;
}

static ssize_t read_hids_ptp_sel_reporting(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                           void *buf, uint16_t len, uint16_t offset) {
    struct zmk_hid_ptp_feature_selective_report *report =
        zmk_mouse_hid_ptp_get_feature_selective_report();
    LOG_DBG("Get PTP sel mode at offset %d with len %d", offset, len);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &report->body,
                             sizeof(struct zmk_hid_ptp_feature_selective_report_body));
}

static ssize_t write_hids_ptp_sel_reporting(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            const void *buf, uint16_t len, uint16_t offset,
                                            uint8_t flags) {
    if (offset != 0) {
        LOG_ERR("Funky offset for sel reporting");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(struct zmk_hid_ptp_feature_selective_report_body)) {
        LOG_ERR("Wrong size for sel reporting");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile < 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    struct zmk_hid_ptp_feature_selective_report_body *report =
        (struct zmk_hid_ptp_feature_selective_report_body *)buf;
    LOG_DBG("Selective: surface: %d, button: %d", report->surface_switch, report->button_switch);
    struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_BLE,
                                             .ble = {
                                                 .profile_index = profile,
                                             }};
    zmk_trackpad_set_selective_report(report->surface_switch, report->button_switch, endpoint);

    return len;
}

#endif // IS_ENABLED(CONFIG_ZMK_TRACKPAD)

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("Input CC changed for %d", attr->handle);
    host_requests_notification = (value == BT_GATT_CCC_NOTIFY) ? 1 : 0;
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    uint8_t *value = attr->user_data;

    if (offset + len > sizeof(ctrl_point)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(value + offset, buf, len);

    return len;
}

/* HID Service Declaration */
BT_GATT_SERVICE_DEFINE(
    mouse_hog_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    //    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
    //                           BT_GATT_PERM_WRITE, NULL, write_proto_mode, &proto_mode),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
                           read_hids_report_map, NULL, NULL),

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD)
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_hids_ptp_input_report, NULL, NULL),
    BT_GATT_CCC(input_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &ptp_input),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_hids_ptp_caps_report, NULL, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &ptp_caps),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_hids_ptp_hqa_report, NULL, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &ptp_hqa),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_hids_ptp_mode, write_hids_ptp_mode, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &ptp_mode),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           read_hids_ptp_sel_reporting, write_hids_ptp_sel_reporting, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_hids_report_ref,
                       NULL, &ptp_selective_reporting),

#endif // IS_ENABLED(CONFIG_ZMK_TRACKPAD)

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_hids_info,
                           NULL, &info),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl_point, &ctrl_point));

static struct bt_conn *destination_connection(void) {
    struct bt_conn *conn;
    bt_addr_le_t *addr = zmk_ble_active_profile_addr();

    if (!bt_addr_le_cmp(addr, BT_ADDR_LE_ANY)) {
        LOG_WRN("Not sending, no active address for current profile");
        return NULL;
    } else if ((conn = bt_conn_lookup_addr_le(BT_ID_DEFAULT, addr)) == NULL) {
        LOG_WRN("Not sending, not connected to active profile");
        return NULL;
    }

    return conn;
}

K_THREAD_STACK_DEFINE(mouse_hog_q_stack, CONFIG_ZMK_BLE_THREAD_STACK_SIZE);

static struct k_work_q mouse_hog_work_q;

#if IS_ENABLED(CONFIG_ZMK_TRACKPAD)

struct zmk_hid_ptp_report_body hog_report;

void send_ptp_report_callback(struct k_work *work) {
    struct bt_conn *conn = destination_connection();
    if (conn == NULL) {
        return;
    }

    struct bt_gatt_notify_params notify_params = {
        .attr = &mouse_hog_svc.attrs[3],
        .data = &hog_report,
        .len = sizeof(hog_report),
    };

    int err = bt_gatt_notify_cb(conn, &notify_params);
    if (err == -EPERM) {
        bt_conn_set_security(conn, BT_SECURITY_L2);
    } else if (err) {
        LOG_DBG("Error notifying %d", err);
    }

    bt_conn_unref(conn);
};

K_WORK_DEFINE(hog_ptp_work, send_ptp_report_callback);

int zmk_mouse_hog_send_ptp_report(struct zmk_hid_ptp_report_body *report) {
    hog_report = *report;

    k_work_submit_to_queue(&mouse_hog_work_q, &hog_ptp_work);

    return 0;
};
#endif // IS_ENABLED(CONFIG_ZMK_TRACKPAD)

static int zmk_mouse_hog_init(void) {
    static const struct k_work_queue_config queue_config = {.name =
                                                                "Mouse HID Over GATT Send Work"};
    k_work_queue_start(&mouse_hog_work_q, mouse_hog_q_stack,
                       K_THREAD_STACK_SIZEOF(mouse_hog_q_stack), CONFIG_ZMK_BLE_THREAD_PRIORITY,
                       &queue_config);

    return 0;
}

SYS_INIT(zmk_mouse_hog_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);

/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/endpoints_types.h>
#include <zmk/hid.h>
#include <zmk/hid_indicators_types.h>

#define ZMK_LED_NUMLOCK_BIT BIT(0)
#define ZMK_LED_CAPSLOCK_BIT BIT(1)
#define ZMK_LED_SCROLLLOCK_BIT BIT(2)
#define ZMK_LED_COMPOSE_BIT BIT(3)
#define ZMK_LED_KANA_BIT BIT(4)

zmk_hid_indicators_t zmk_hid_indicators_get_current_profile(void);
zmk_hid_indicators_t zmk_hid_indicators_get_profile(struct zmk_endpoint_instance endpoint);
void zmk_hid_indicators_set_profile(zmk_hid_indicators_t indicators,
                                    struct zmk_endpoint_instance endpoint);

void zmk_hid_indicators_process_report(struct zmk_hid_led_report_body *report,
                                       struct zmk_endpoint_instance endpoint);

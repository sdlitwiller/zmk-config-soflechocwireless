/*
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define HUB_LED_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(HUB_LED_NODE, okay) && DT_NODE_HAS_PROP(HUB_LED_NODE, gpios)
#define HUB_LED_AVAILABLE 1
static const struct gpio_dt_spec hub_led = GPIO_DT_SPEC_GET(HUB_LED_NODE, gpios);
#else
#define HUB_LED_AVAILABLE 0
#endif

static uint8_t hub_battery_pct = 100;
static uint8_t peripheral_battery_pct[CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS];
static bool low_battery_alarm_active = false;
static bool led_on = false;

static void set_hub_led(bool on) {
#if HUB_LED_AVAILABLE
    int rc = gpio_pin_set_dt(&hub_led, on ? 1 : 0);
    if (rc < 0) {
        LOG_ERR("Failed to set hub LED state: %d", rc);
    }
#else
    ARG_UNUSED(on);
#endif
}

static void low_battery_blink_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!low_battery_alarm_active) {
        return;
    }

    led_on = !led_on;
    set_hub_led(led_on);
}

static K_WORK_DEFINE(low_battery_blink_work, low_battery_blink_work_handler);

static void low_battery_blink_timer_handler(struct k_timer *timer) {
    ARG_UNUSED(timer);
    k_work_submit(&low_battery_blink_work);
}

K_TIMER_DEFINE(low_battery_blink_timer, low_battery_blink_timer_handler, NULL);

static bool is_any_device_low_battery(void) {
    if (hub_battery_pct <= CONFIG_ZMK_HUB_LOW_BATTERY_THRESHOLD) {
        return true;
    }

    for (size_t i = 0; i < ARRAY_SIZE(peripheral_battery_pct); i++) {
        uint8_t level = peripheral_battery_pct[i];

        if (level != 0 && level <= CONFIG_ZMK_HUB_LOW_BATTERY_THRESHOLD) {
            return true;
        }
    }

    return false;
}

static void sync_low_battery_alarm_state(void) {
    bool should_alarm = is_any_device_low_battery();

    if (should_alarm == low_battery_alarm_active) {
        return;
    }

    low_battery_alarm_active = should_alarm;

    if (low_battery_alarm_active) {
        led_on = false;
        k_timer_start(&low_battery_blink_timer, K_NO_WAIT,
                      K_MSEC(CONFIG_ZMK_HUB_LOW_BATTERY_BLINK_MS));
    } else {
        k_timer_stop(&low_battery_blink_timer);
        led_on = false;
        set_hub_led(false);
    }
}

static int hub_low_battery_led_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *hub_batt_ev = as_zmk_battery_state_changed(eh);
    if (hub_batt_ev != NULL) {
        hub_battery_pct = hub_batt_ev->state_of_charge;
        sync_low_battery_alarm_state();
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_peripheral_battery_state_changed *peripheral_batt_ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (peripheral_batt_ev != NULL) {
        if (peripheral_batt_ev->source >= ARRAY_SIZE(peripheral_battery_pct)) {
            LOG_WRN("Ignoring battery update for out-of-range source: %d", peripheral_batt_ev->source);
            return ZMK_EV_EVENT_BUBBLE;
        }

        peripheral_battery_pct[peripheral_batt_ev->source] = peripheral_batt_ev->state_of_charge;
        sync_low_battery_alarm_state();
        return ZMK_EV_EVENT_BUBBLE;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(hub_low_battery_led, hub_low_battery_led_listener);
ZMK_SUBSCRIPTION(hub_low_battery_led, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(hub_low_battery_led, zmk_peripheral_battery_state_changed);

static int hub_low_battery_led_init(void) {
    for (size_t i = 0; i < ARRAY_SIZE(peripheral_battery_pct); i++) {
        peripheral_battery_pct[i] = 100;
    }

#if HUB_LED_AVAILABLE
    if (!gpio_is_ready_dt(&hub_led)) {
        LOG_WRN("Hub led0 GPIO is not ready; low battery LED alert disabled");
        return 0;
    }

    int rc = gpio_pin_configure_dt(&hub_led, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) {
        LOG_ERR("Failed to configure hub led0 GPIO: %d", rc);
        return rc;
    }
#else
    LOG_WRN("No led0 alias with gpios found; low battery LED alert disabled");
#endif

    return 0;
}

SYS_INIT(hub_low_battery_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

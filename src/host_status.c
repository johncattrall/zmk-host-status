/*
 * zmk-host-status: expose keyboard status to host apps over BLE GATT.
 *
 * Service 6e1a0001-7c5b-4f2e-9a3d-2b8c4d1e5f60
 *   ...0002 status  (read, notify)  15-byte packed struct, see below
 *   ...0003 names   (read)          layer names, '\n'-separated, index order
 *   ...0004 build   (read)          firmware build id (git short SHA)
 *
 * Status payload v2 (little endian). v1 was the first 12 bytes; later
 * versions only append, so v1 readers keep working:
 *   u8  version            = 2
 *   u8  flags              bit0: all expected split peripherals connected
 *   u32 active             bit i = layer at index i active
 *   u32 locked             active layers that stay on with no key held
 *                          (toggles, NumWord-style auto layers, temp layers)
 *   u8  periph_connected
 *   u8  periph_expected    0 on non-split boards
 *   -- v2 --
 *   u8  mods               held explicit modifiers, HID bit order (LCtrl,
 *                          LShift, LAlt, LGui, RCtrl, RShift, RAlt, RGui);
 *                          always 0 with CONFIG_ZMK_HOST_STATUS_MODIFIERS=n
 *   u8  flags2             bit0: output is USB (else BLE)
 *                          bit1: active BLE profile is connected
 *   u8  ble_profile        active BLE profile index
 *
 * Notifies only on change, coalesced over 30 ms. SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/keymap.h>
#include <zmk/ble.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>

#include <zephyr/bluetooth/conn.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#define HS_PERIPH_EXPECTED ZMK_SPLIT_BLE_PERIPHERAL_COUNT
#else
#define HS_PERIPH_EXPECTED 0
#endif

LOG_MODULE_REGISTER(zmk_host_status, CONFIG_ZMK_LOG_LEVEL);

#define HS_VERSION 2
#ifndef CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN
#define CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN 20
#endif
#define HS_MAX_POSITIONS 128

#define HS_UUID(n) BT_UUID_128_ENCODE(0x6e1a0000 + (n), 0x7c5b, 0x4f2e, 0x9a3d, 0x2b8c4d1e5f60)
static struct bt_uuid_128 hs_svc_uuid = BT_UUID_INIT_128(HS_UUID(1));
static struct bt_uuid_128 hs_status_uuid = BT_UUID_INIT_128(HS_UUID(2));
static struct bt_uuid_128 hs_names_uuid = BT_UUID_INIT_128(HS_UUID(3));
static struct bt_uuid_128 hs_build_uuid = BT_UUID_INIT_128(HS_UUID(4));

struct hs_payload {
    uint8_t version;
    uint8_t flags;
    uint32_t active;
    uint32_t locked;
    uint8_t periph_connected;
    uint8_t periph_expected;
    /* v2 */
    uint8_t mods;
    uint8_t flags2;
    uint8_t ble_profile;
} __packed;

static uint32_t locked_mask;
/* Split halves linked to this central. Counted from BLE connections in
 * which this device is the BLE central: ZMK's central half is the BLE
 * central only toward its peripheral halves (hosts connect to it as
 * peripheral). zmk_split_peripheral_status_changed is NOT usable here: in
 * ZMK v0.3 it is raised on the peripheral half only. */
static atomic_t periph_connected;
static uint32_t held_positions[HS_MAX_POSITIONS / 32];
static struct hs_payload last_sent;
static bool notify_enabled;

static uint32_t active_mask(void) {
    uint32_t mask = 0;
    for (zmk_keymap_layer_index_t i = 0; i < ZMK_KEYMAP_LAYERS_LEN && i < 32; i++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(i);
        if (id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(id)) {
            mask |= BIT(i);
        }
    }
    return mask;
}

static bool any_key_held(void) {
    for (size_t i = 0; i < ARRAY_SIZE(held_positions); i++) {
        if (held_positions[i]) {
            return true;
        }
    }
    return false;
}

static void build_payload(struct hs_payload *p) {
    uint32_t active = active_mask();
    /* Locked = non-default layers still on when no key is held. While keys
     * are held, a lock can only be lost (layer turned off), never gained. */
    if (any_key_held()) {
        locked_mask &= active;
    } else {
        locked_mask = active & ~BIT(0);
    }
    p->version = HS_VERSION;
    uint8_t linked = (uint8_t)MIN(atomic_get(&periph_connected), HS_PERIPH_EXPECTED);
    p->flags = (HS_PERIPH_EXPECTED > 0 && linked >= HS_PERIPH_EXPECTED) ? BIT(0) : 0;
    p->active = sys_cpu_to_le32(active);
    p->locked = sys_cpu_to_le32(locked_mask);
    p->periph_connected = linked;
    p->periph_expected = HS_PERIPH_EXPECTED;
#if IS_ENABLED(CONFIG_ZMK_HOST_STATUS_MODIFIERS)
    p->mods = zmk_hid_get_explicit_mods();
#else
    p->mods = 0;
#endif
    p->flags2 = 0;
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB) {
        p->flags2 |= BIT(0);
    }
    if (zmk_ble_active_profile_is_connected()) {
        p->flags2 |= BIT(1);
    }
    p->ble_profile = (uint8_t)zmk_ble_active_profile_index();
}

/* ---- GATT ---- */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    struct hs_payload p;
    build_payload(&p);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &p, sizeof(p));
}

static ssize_t read_names(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t len, uint16_t offset) {
    char names[ZMK_KEYMAP_LAYERS_LEN * (CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN + 1) + 1];
    size_t pos = 0;
    for (zmk_keymap_layer_index_t i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(i);
        const char *name = (id == ZMK_KEYMAP_LAYER_ID_INVAL) ? NULL : zmk_keymap_layer_name(id);
        int n = (name && name[0]) ? snprintf(names + pos, sizeof(names) - pos, "%s%s",
                                             i ? "\n" : "", name)
                                  : snprintf(names + pos, sizeof(names) - pos, "%sLayer %d",
                                             i ? "\n" : "", i);
        if (n < 0 || (size_t)n >= sizeof(names) - pos) {
            break;
        }
        pos += n;
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, names, pos);
}

static ssize_t read_build(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t len, uint16_t offset) {
    static const char build[] = HOST_STATUS_BUILD_ID;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, build, strlen(build));
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(host_status_svc, BT_GATT_PRIMARY_SERVICE(&hs_svc_uuid),
                       BT_GATT_CHARACTERISTIC(&hs_status_uuid.uuid,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ_ENCRYPT, read_status, NULL, NULL),
                       BT_GATT_CCC(status_ccc_changed,
                                   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
                       BT_GATT_CHARACTERISTIC(&hs_names_uuid.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ_ENCRYPT, read_names, NULL, NULL),
                       BT_GATT_CHARACTERISTIC(&hs_build_uuid.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ_ENCRYPT, read_build, NULL, NULL), );

/* Attribute index of the status characteristic value within the service. */
#define HS_STATUS_ATTR (&host_status_svc.attrs[2])

static void notify_work_cb(struct k_work *work) {
    struct hs_payload p;
    build_payload(&p);
    if (memcmp(&p, &last_sent, sizeof(p)) == 0) {
        return;
    }
    last_sent = p;
    if (notify_enabled) {
        int err = bt_gatt_notify(NULL, HS_STATUS_ATTR, &p, sizeof(p));
        if (err && err != -ENOTCONN) {
            LOG_DBG("host status notify failed (%d)", err);
        }
    }
}

static K_WORK_DELAYABLE_DEFINE(notify_work, notify_work_cb);

static void schedule_notify(void) { k_work_reschedule(&notify_work, K_MSEC(30)); }

/* ---- events ---- */
static int host_status_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        if (pos->position < HS_MAX_POSITIONS) {
            uint32_t bit = BIT(pos->position % 32);
            uint32_t *word = &held_positions[pos->position / 32];
            bool was_held = any_key_held();
            if (pos->state) {
                *word |= bit;
            } else {
                *word &= ~bit;
            }
            /* Only all-keys-released can change the lock set; skip work on
             * ordinary typing. */
            if (was_held && !any_key_held()) {
                schedule_notify();
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_layer_state_changed(eh) || as_zmk_ble_active_profile_changed(eh) ||
        as_zmk_endpoint_changed(eh)) {
        schedule_notify();
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_HOST_STATUS_MODIFIERS)
    /* zmk_modifiers_state_changed is declared but never raised in ZMK v0.3,
     * so watch modifier key usages instead and read the HID state in the
     * (deferred) work, after the HID listener has applied the change.
     * Ordinary letters are filtered out: typing costs no extra work. */
    const struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);
    if (kc && kc->usage_page == HID_USAGE_KEY && kc->keycode >= 0xE0 && kc->keycode <= 0xE7) {
        schedule_notify();
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(host_status, host_status_listener);
ZMK_SUBSCRIPTION(host_status, zmk_position_state_changed);
ZMK_SUBSCRIPTION(host_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(host_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(host_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_HOST_STATUS_MODIFIERS)
ZMK_SUBSCRIPTION(host_status, zmk_keycode_state_changed);
#endif

/* ---- connection tracking ---- */
static bool is_split_link(struct bt_conn *conn) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    struct bt_conn_info info;
    return bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL;
#else
    return false;
#endif
}

static void hs_connected(struct bt_conn *conn, uint8_t err) {
    if (err == 0 && is_split_link(conn)) {
        atomic_inc(&periph_connected);
    }
    /* Host connections change the profile-connected flag too. */
    schedule_notify();
}

static void hs_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (is_split_link(conn) && atomic_get(&periph_connected) > 0) {
        atomic_dec(&periph_connected);
    }
    schedule_notify();
}

BT_CONN_CB_DEFINE(host_status_conn_cb) = {
    .connected = hs_connected,
    .disconnected = hs_disconnected,
};

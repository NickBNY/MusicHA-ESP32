/**
 * Settings > General > Home Assistant card.
 *
 * Lets the panel appear in Home Assistant as an ESPHome device. Off by
 * default. "Debug information" adds memory, task stack, reboot and sync
 * entities to that device. The LVGL callbacks run on mainAppTask, which is
 * where the NVS writes in haSetEnabled()/haSetDebug() are safe.
 */

#include "ui_common.h"
#include "config.h"
#include "ui_settings_card.h"
#include "ui_fonts.h"
#include "amber.h"
#include "esphome_api.h"

static lv_obj_t* sw_ha_enable;
static lv_obj_t* sw_ha_debug;
static lv_obj_t* lbl_ha_status;
static lv_obj_t* lbl_ha_address;

static void setSwitch(lv_obj_t* sw, bool on) {
    if (!sw) return;
    // add/remove_state do not raise VALUE_CHANGED, so this cannot loop back
    // into the handlers below.
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

void homeAssistantCardRefresh() {
    if (!sw_ha_enable) return;
    const bool enabled = haIsEnabled();
    setSwitch(sw_ha_enable, enabled);
    setSwitch(sw_ha_debug, haIsDebug());
    if (enabled) lv_obj_remove_state(sw_ha_debug, LV_STATE_DISABLED);
    else         lv_obj_add_state(sw_ha_debug, LV_STATE_DISABLED);

    char buf[64];
    haStatus(buf, sizeof(buf));
    lv_label_set_text(lbl_ha_status, buf);
    haAddress(buf, sizeof(buf));
    lv_label_set_text(lbl_ha_address, buf);
}

void addHomeAssistantCard(lv_obj_t* content) {
    lv_obj_t* card = addCard(content, "Home Assistant");

    lv_obj_t* slot = addSettingRow(card, "Enable",
                                   "Shows this panel in Home Assistant as an ESPHome device "
                                   "with now playing, volume, Wi-Fi signal and uptime. "
                                   "The connection is unencrypted and read-only, so use it "
                                   "only on a trusted home network.",
                                   true);
    sw_ha_enable = addSwitch(slot, false);
    lv_obj_add_event_cb(sw_ha_enable, [](lv_event_t* e) {
        lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
        const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
        haSetEnabled(on);
        homeAssistantCardRefresh();
    }, LV_EVENT_VALUE_CHANGED, NULL);

    slot = addSettingRow(card, "Debug information",
                         "Adds memory, task stack, restart history and speaker sync "
                         "readings to the device in Home Assistant.",
                         true);
    sw_ha_debug = addSwitch(slot, false);
    lv_obj_add_state(sw_ha_debug, LV_STATE_DISABLED);
    lv_obj_add_event_cb(sw_ha_debug, [](lv_event_t* e) {
        lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
        haSetDebug(lv_obj_has_state(sw, LV_STATE_CHECKED));
    }, LV_EVENT_VALUE_CHANGED, NULL);

    slot = addSettingRow(card, "Status", NULL, true);
    lbl_ha_status = addValueLabel(slot, "Off");

    // No description on this row: the address is long and needs the width.
    slot = addSettingRow(card, "Address", NULL, true);
    lbl_ha_address = addValueLabel(slot, "Not connected");
    lv_obj_set_style_text_align(lbl_ha_address, LV_TEXT_ALIGN_RIGHT, 0);
    addDescLabel(card, "If Home Assistant does not find the panel on its own, add the "
                       "ESPHome integration there and enter this address.");
}

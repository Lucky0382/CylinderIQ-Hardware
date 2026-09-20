// CylinderIQ Hub V2 — zigbee_coord.cpp (ESP32-C6)
//
// Zigbee 3.0 Coordinator. Runs alongside Wi-Fi using the ESP32-C6
// built-in radio coexistence (Wi-Fi + IEEE 802.15.4 share the 2.4GHz
// RF front-end via time-division multiplexing).
//
// Architecture:
//   - zigbee_task() runs esp_zb_main_loop_iteration() continuously
//   - HTTP tasks call zigbee_coord_switch_set() which acquires the
//     Zigbee lock, sends a ZCL On/Off command, then releases the lock
//   - Paired switch IEEE addresses are stored in NVS namespace "zb"
//   - On reboot, NVS is read and runtime state is restored
//
// MOES 20A switch Zigbee profile:
//   Profile:  HA (0x0104)
//   Device:   On/Off Switch (0x0002) or Mains Power Outlet (0x0009)
//   Endpoint: 1
//   Cluster:  On/Off (0x0006)
//   Commands: 0x00 = Off, 0x01 = On

#include "zigbee_coord.h"

#include <string.h>
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "esp_coexist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "zb_coord";

// ── NVS keys ──────────────────────────────────────────────────
#define NVS_NAMESPACE   "zb"
static const char *NVS_IEEE_KEY[SWITCH_COUNT]   = {"top_ieee",  "bot_ieee"};
static const char *NVS_SHORT_KEY[SWITCH_COUNT]  = {"top_short", "bot_short"};
static const char *NVS_PAIRED_KEY[SWITCH_COUNT] = {"top_ok",    "bot_ok"};

// ── Runtime state (mutex-protected) ───────────────────────────
static SemaphoreHandle_t s_state_mutex;
static zb_switch_t       s_sw[SWITCH_COUNT];
static zb_pair_state_t   s_pair = {false, SWITCH_TOP, 0};

// Coordinator endpoint number
#define COORD_ENDPOINT  1

// ── NVS helpers ───────────────────────────────────────────────

static void nvs_save_switch(switch_id_t sw)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_IEEE_KEY[sw],   s_sw[sw].ieee,       8);
    nvs_set_u16 (h, NVS_SHORT_KEY[sw],  s_sw[sw].short_addr);
    nvs_set_u8  (h, NVS_PAIRED_KEY[sw], s_sw[sw].paired ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_switch(switch_id_t sw)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t paired = 0;
    nvs_get_u8(h, NVS_PAIRED_KEY[sw], &paired);

    if (paired) {
        size_t len = 8;
        nvs_get_blob(h, NVS_IEEE_KEY[sw],  s_sw[sw].ieee, &len);
        nvs_get_u16 (h, NVS_SHORT_KEY[sw], &s_sw[sw].short_addr);
        s_sw[sw].paired = true;
        ESP_LOGI(TAG, "Switch %s loaded from NVS addr=0x%04X",
                 sw == SWITCH_TOP ? "TOP" : "BOTTOM", s_sw[sw].short_addr);
    }
    nvs_close(h);
}

static void nvs_clear_switch(switch_id_t sw)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_IEEE_KEY[sw]);
    nvs_erase_key(h, NVS_SHORT_KEY[sw]);
    nvs_erase_key(h, NVS_PAIRED_KEY[sw]);
    nvs_commit(h);
    nvs_close(h);
}

// ── Device join handler ────────────────────────────────────────

static void handle_device_joined(uint16_t short_addr, const uint8_t *ieee)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (!s_pair.open) {
        ESP_LOGW(TAG, "Device joined but pair window closed — ignoring 0x%04X", short_addr);
        xSemaphoreGive(s_state_mutex);
        return;
    }

    switch_id_t slot = s_pair.target;
    s_sw[slot].short_addr = short_addr;
    memcpy(s_sw[slot].ieee, ieee, 8);
    s_sw[slot].paired = true;
    s_sw[slot].on     = false;

    s_pair.open = false; // close window after one device joins

    xSemaphoreGive(s_state_mutex);

    nvs_save_switch(slot);

    ESP_LOGI(TAG, "Switch %s paired — addr=0x%04X IEEE=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             slot == SWITCH_TOP ? "TOP" : "BOTTOM",
             short_addr,
             ieee[7], ieee[6], ieee[5], ieee[4],
             ieee[3], ieee[2], ieee[1], ieee[0]);
}

// ── Zigbee signal handler ──────────────────────────────────────
// Called by the Zigbee stack for all network events.

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    esp_zb_bdb_start_top_level_commissioning(mode_mask);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p     = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig_type) {

    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized — forming network");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ext_pan;
            esp_zb_get_extended_pan_id(ext_pan);
            ESP_LOGI(TAG, "Network formed — channel %d  PAN 0x%04X",
                     esp_zb_get_current_channel(),
                     esp_zb_get_pan_id());
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGE(TAG, "Network formation failed: %s — retrying",
                     esp_err_to_name(err_status));
            esp_zb_scheduler_alarm(
                bdb_start_top_level_commissioning_cb,
                ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Network steering complete — coordinator ready");
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *params =
            (esp_zb_zdo_signal_device_annce_params_t *)
                esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "Device announce — short=0x%04X", params->device_short_addr);
        handle_device_joined(params->device_short_addr, params->ieee_addr);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Device left network");
        break;

    default:
        ESP_LOGD(TAG, "Signal %d status=%s", sig_type, esp_err_to_name(err_status));
        break;
    }
}

// ── Zigbee task ───────────────────────────────────────────────

static void zigbee_task(void *arg)
{
    (void)arg;

    // Coordinator config
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role        = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg = {
            .zczr_cfg = {
                .max_children = 10,
            }
        }
    };
    esp_zb_init(&zb_cfg);

    // Create a minimal HA endpoint so the coordinator appears on the network
    esp_zb_ep_list_t      *ep_list      = esp_zb_ep_list_create();
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01, // Mains powered
    };
    esp_zb_cluster_list_add_basic_cluster(
        cluster_list,
        esp_zb_basic_cluster_create(&basic_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint        = COORD_ENDPOINT,
        .app_profile_id  = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id   = ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
    esp_zb_device_register(ep_list);

    // Fix Zigbee coordinator to Channel 25 (2475 MHz) — zero overlap with Wi-Fi Channel 6 (2437 MHz)
    esp_zb_set_primary_network_channel_set(1 << 25);

    ESP_ERROR_CHECK(esp_zb_start(false)); // false = don't erase stored network

    // Main loop — runs forever, processes Zigbee stack events
    esp_zb_stack_main_loop();
    // Should never return, but just in case:
    vTaskDelete(NULL);
}

// ── Public API ────────────────────────────────────────────────

void zigbee_coord_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex);
    memset(s_sw, 0, sizeof(s_sw));

    // Restore paired switches from NVS
    for (int i = 0; i < SWITCH_COUNT; i++) {
        nvs_load_switch((switch_id_t)i);
    }

    // Enable Wi-Fi + IEEE 802.15.4 radio coexistence
    esp_coex_wifi_i154_enable();

    // Initialise Zigbee platform (must be called before esp_zb_init)
    esp_zb_platform_config_t platform_cfg = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        }
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    // Zigbee task — pinned to core 0, stack 4096, priority 5
    xTaskCreatePinnedToCore(zigbee_task, "zigbee", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Zigbee coordinator task started");
}

void zigbee_coord_permit_join(switch_id_t slot, uint8_t duration_s)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_pair.open       = (duration_s > 0);
    s_pair.target     = slot;
    s_pair.remaining_s = duration_s;
    xSemaphoreGive(s_state_mutex);

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_bdb_open_network(duration_s);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "Permit join: slot=%s duration=%ds",
             slot == SWITCH_TOP ? "TOP" : "BOTTOM", duration_s);
}

bool zigbee_coord_switch_set(switch_id_t sw, bool on)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool paired       = s_sw[sw].paired;
    uint16_t short_addr = s_sw[sw].short_addr;
    xSemaphoreGive(s_state_mutex);

    if (!paired) {
        ESP_LOGW(TAG, "Switch %d not paired — cannot send command", sw);
        return false;
    }

    // Build ZCL On/Off command
    esp_zb_zcl_on_off_cmd_t cmd = {};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    cmd.zcl_basic_cmd.dst_endpoint          = 1; // MOES endpoint
    cmd.zcl_basic_cmd.src_endpoint          = COORD_ENDPOINT;
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
                           : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();

    if (err == ESP_OK) {
        // Optimistic state update
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_sw[sw].on = on;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(TAG, "Switch %s → %s", sw == SWITCH_TOP ? "TOP" : "BOTTOM",
                 on ? "ON" : "OFF");
        return true;
    }

    ESP_LOGE(TAG, "ZCL send failed: %s", esp_err_to_name(err));
    return false;
}

zb_switch_t zigbee_coord_switch_get(switch_id_t sw)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    zb_switch_t copy = s_sw[sw];
    xSemaphoreGive(s_state_mutex);
    return copy;
}

zb_pair_state_t zigbee_coord_pair_state(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    zb_pair_state_t copy = s_pair;
    xSemaphoreGive(s_state_mutex);
    return copy;
}

void zigbee_coord_clear(switch_id_t sw)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memset(&s_sw[sw], 0, sizeof(zb_switch_t));
    xSemaphoreGive(s_state_mutex);
    nvs_clear_switch(sw);
    ESP_LOGI(TAG, "Switch %s cleared", sw == SWITCH_TOP ? "TOP" : "BOTTOM");
}

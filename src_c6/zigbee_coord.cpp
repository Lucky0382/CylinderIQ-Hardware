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
#include "esp_zigbee_secur.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"

// Set storage partition name for esp-zigbee dataset persistence
extern "C" void esp_zigbee_set_storage_name(const char *name);
extern "C" void ezb_secur_tcpol_set_allow_rejoins_with_well_known_key(bool allow);

static const char *TAG = "zb_coord";

// ── NVS keys ──────────────────────────────────────────────────
#define NVS_NAMESPACE   "zb"
static const char *NVS_IEEE_KEY[SWITCH_COUNT]   = {"top_ieee",  "bot_ieee"};
static const char *NVS_SHORT_KEY[SWITCH_COUNT]  = {"top_short", "bot_short"};
static const char *NVS_EP_KEY[SWITCH_COUNT]     = {"top_ep",    "bot_ep"};
static const char *NVS_PAIRED_KEY[SWITCH_COUNT] = {"top_ok",    "bot_ok"};

// ── Runtime state (mutex-protected) ───────────────────────────
static SemaphoreHandle_t s_state_mutex;
static zb_switch_t       s_sw[SWITCH_COUNT];
static zb_pair_state_t   s_pair = {false, SWITCH_TOP, 0};
static TimerHandle_t     s_pair_timer = NULL;
static bool              s_coord_ready = false;
static uint16_t          s_pan_id = 0xA276;
static uint8_t           s_channel = 20;

// Coordinator endpoint number
#define COORD_ENDPOINT  1

// ── NVS helpers ───────────────────────────────────────────────

static void nvs_save_switch(switch_id_t sw)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_IEEE_KEY[sw],   s_sw[sw].ieee,       8);
    nvs_set_u16 (h, NVS_SHORT_KEY[sw],  s_sw[sw].short_addr);
    nvs_set_u8  (h, NVS_EP_KEY[sw],     s_sw[sw].endpoint ? s_sw[sw].endpoint : 1);
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
        uint8_t ep = 1;
        nvs_get_u8(h, NVS_EP_KEY[sw], &ep);
        s_sw[sw].endpoint = ep ? ep : 1;
        s_sw[sw].paired = true;
        ESP_LOGI(TAG, "Switch %s loaded from NVS addr=0x%04X ep=%d",
                 sw == SWITCH_TOP ? "TOP" : "BOTTOM", s_sw[sw].short_addr, s_sw[sw].endpoint);
    }
    nvs_close(h);
}

static void nvs_clear_switch(switch_id_t sw)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_IEEE_KEY[sw]);
    nvs_erase_key(h, NVS_SHORT_KEY[sw]);
    nvs_erase_key(h, NVS_EP_KEY[sw]);
    nvs_erase_key(h, NVS_PAIRED_KEY[sw]);
    nvs_commit(h);
    nvs_close(h);
}

// ── Pair window countdown timer callback ───────────────────────
static void pair_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_pair.remaining_s > 0) {
        s_pair.remaining_s--;
    }
    if (s_pair.remaining_s == 0) {
        s_pair.open = false;
        if (s_pair_timer) {
            xTimerStop(s_pair_timer, 0);
        }
        ESP_LOGI(TAG, "Permit join window expired");
    }
    xSemaphoreGive(s_state_mutex);
}

// ── Device join handler ────────────────────────────────────────

static void handle_device_joined(uint16_t short_addr, const uint8_t *ieee)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    // If device is already paired to a slot (by IEEE or short_addr), update address and avoid duplicate slot assignment
    for (int i = 0; i < SWITCH_COUNT; i++) {
        if (s_sw[i].paired) {
            bool same_ieee = (ieee && memcmp(s_sw[i].ieee, ieee, 8) == 0);
            bool same_short = (s_sw[i].short_addr == short_addr && short_addr != 0);
            if (same_ieee || same_short) {
                s_sw[i].short_addr = short_addr;
                if (ieee) memcpy(s_sw[i].ieee, ieee, 8);
                xSemaphoreGive(s_state_mutex);
                nvs_save_switch((switch_id_t)i);
                ESP_LOGI(TAG, "Device 0x%04X already paired as %s (updated)",
                         short_addr, i == 0 ? "TOP" : "BOTTOM");
                return;
            }
        }
    }

    switch_id_t slot;
    if (s_pair.open) {
        slot = s_pair.target;
    } else if (!s_sw[SWITCH_TOP].paired) {
        slot = SWITCH_TOP;
        ESP_LOGI(TAG, "Auto-assigning joining device to unpaired TOP slot");
    } else if (!s_sw[SWITCH_BOTTOM].paired) {
        slot = SWITCH_BOTTOM;
        ESP_LOGI(TAG, "Auto-assigning joining device to unpaired BOTTOM slot");
    } else {
        ESP_LOGW(TAG, "Both slots already paired — ignoring joining device 0x%04X", short_addr);
        xSemaphoreGive(s_state_mutex);
        return;
    }

    s_sw[slot].short_addr = short_addr;
    if (ieee) {
        memcpy(s_sw[slot].ieee, ieee, 8);
    }
    s_sw[slot].paired   = true;
    s_sw[slot].on       = false;
    s_sw[slot].endpoint = 1; // Default to endpoint 1 until user_find_cb confirms

    xSemaphoreGive(s_state_mutex);

    nvs_save_switch(slot);

    if (ieee) {
        ESP_LOGI(TAG, "Switch %s paired — addr=0x%04X IEEE=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                 slot == SWITCH_TOP ? "TOP" : "BOTTOM",
                 short_addr,
                 ieee[7], ieee[6], ieee[5], ieee[4],
                 ieee[3], ieee[2], ieee[1], ieee[0]);
    } else {
        ESP_LOGI(TAG, "Switch %s paired — addr=0x%04X",
                 slot == SWITCH_TOP ? "TOP" : "BOTTOM", short_addr);
    }
}

// ── Device discovery & binding callbacks ───────────────────────

static void bind_cb(esp_zb_zdp_status_t zdo_status, void *user_ctx)
{
    (void)user_ctx;
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Device successfully bound to coordinator On/Off cluster!");
    } else {
        ESP_LOGW(TAG, "Device bind returned status 0x%02X", zdo_status);
    }
}

static void user_find_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, uint8_t endpoint, void *user_ctx)
{
    (void)user_ctx;
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Found On/Off endpoint %d on device 0x%04X — initiating binding", endpoint, addr);
        esp_zb_ieee_addr_t remote_ieee = {};
        esp_zb_ieee_address_by_short(addr, remote_ieee);

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        for (int i = 0; i < SWITCH_COUNT; i++) {
            if (s_sw[i].paired && s_sw[i].short_addr == addr) {
                s_sw[i].endpoint = endpoint;
                if (remote_ieee[0] != 0 || remote_ieee[7] != 0) {
                    memcpy(s_sw[i].ieee, remote_ieee, 8);
                }
                nvs_save_switch((switch_id_t)i);
                break;
            }
        }
        xSemaphoreGive(s_state_mutex);

        esp_zb_zdo_bind_req_param_t bind_req = {};
        memcpy(bind_req.src_address, remote_ieee, sizeof(esp_zb_ieee_addr_t));
        bind_req.src_endp      = endpoint;
        bind_req.cluster_id    = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
        bind_req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
        esp_zb_get_long_address(bind_req.dst_address_u.addr_long);
        bind_req.dst_endp      = COORD_ENDPOINT;
        bind_req.req_dst_addr  = addr;
        esp_zb_zdo_device_bind_req(&bind_req, bind_cb, NULL);
    } else {
        ESP_LOGW(TAG, "Find On/Off endpoint on 0x%04X returned status 0x%02X", addr, zdo_status);
    }
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
        ESP_LOGI(TAG, "Zigbee stack initialized — starting BDB initialization");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode",
                     esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Forming Zigbee network on Channel 20");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                uint8_t cur_ch = esp_zb_get_current_channel();
                uint16_t pan   = esp_zb_get_pan_id();
                ESP_LOGI(TAG, "Coordinator restored from NVS (channel %d  PAN 0x%04X)", cur_ch, pan);
                if (cur_ch != 20) {
                    ESP_LOGW(TAG, "Restored channel %d != target Channel 20. Clearing old network to reform on Channel 20...", cur_ch);
                    nvs_clear_switch(SWITCH_TOP);
                    nvs_clear_switch(SWITCH_BOTTOM);
                    esp_zb_bdb_reset_via_local_action();
                } else {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_coord_ready = true;
                    s_pan_id      = pan;
                    s_channel     = cur_ch;
                    xSemaphoreGive(s_state_mutex);
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                }
            }
        } else {
            ESP_LOGE(TAG, "BDB initialization failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_coord_ready = true;
            s_pan_id      = esp_zb_get_pan_id();
            s_channel     = esp_zb_get_current_channel();
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "Network formed successfully — channel %d  PAN 0x%04X",
                     s_channel, s_pan_id);
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGE(TAG, "Network formation failed: %s — retrying in 1s",
                     esp_err_to_name(err_status));
            esp_zb_scheduler_alarm(
                bdb_start_top_level_commissioning_cb,
                ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_coord_ready = true;
            s_pan_id      = esp_zb_get_pan_id();
            s_channel     = esp_zb_get_current_channel();
            xSemaphoreGive(s_state_mutex);
            ESP_LOGI(TAG, "Network steering complete — coordinator ready");
        }
        break;

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK) {
            uint8_t *p_dur = (uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (!p_dur) break;
            uint8_t dur = *p_dur;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            if (dur > 0) {
                if (!s_sw[SWITCH_TOP].paired) {
                    s_pair.open        = true;
                    s_pair.target      = SWITCH_TOP;
                    s_pair.remaining_s = dur;
                    if (s_pair_timer) xTimerReset(s_pair_timer, 0);
                    ESP_LOGI(TAG, "Zigbee network (PAN 0x%04X) is OPEN for %d seconds — READY TO PAIR TOP SWITCH",
                             s_pan_id, dur);
                } else if (!s_sw[SWITCH_BOTTOM].paired) {
                    s_pair.open        = true;
                    s_pair.target      = SWITCH_BOTTOM;
                    s_pair.remaining_s = dur;
                    if (s_pair_timer) xTimerReset(s_pair_timer, 0);
                    ESP_LOGI(TAG, "Zigbee network (PAN 0x%04X) is OPEN for %d seconds — READY TO PAIR BOTTOM SWITCH",
                             s_pan_id, dur);
                } else {
                    ESP_LOGI(TAG, "Zigbee network (PAN 0x%04X) is OPEN for %d seconds (both switches paired)",
                             s_pan_id, dur);
                }
            } else {
                s_pair.open        = false;
                s_pair.remaining_s = 0;
                if (s_pair_timer) xTimerStop(s_pair_timer, 0);
                ESP_LOGI(TAG, "Zigbee network (PAN 0x%04X) is CLOSED", s_pan_id);
            }
            xSemaphoreGive(s_state_mutex);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        if (err_status != ESP_OK) {
            ESP_LOGW(TAG, "Device announce status error: %s", esp_err_to_name(err_status));
            break;
        }
        esp_zb_zdo_signal_device_annce_params_t *params =
            (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        if (!params) {
            ESP_LOGW(TAG, "Device announce: null params");
            break;
        }
        ESP_LOGI(TAG, "Device announce — short=0x%04X IEEE=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                 params->device_short_addr,
                 params->ieee_addr[7], params->ieee_addr[6], params->ieee_addr[5], params->ieee_addr[4],
                 params->ieee_addr[3], params->ieee_addr[2], params->ieee_addr[1], params->ieee_addr[0]);
        handle_device_joined(params->device_short_addr, params->ieee_addr);

        // 1. Immediately bind standard endpoint 1 On/Off cluster to coordinator
        esp_zb_zdo_bind_req_param_t bind_req = {};
        memcpy(bind_req.src_address, params->ieee_addr, sizeof(esp_zb_ieee_addr_t));
        bind_req.src_endp      = 1;
        bind_req.cluster_id    = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
        bind_req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
        esp_zb_get_long_address(bind_req.dst_address_u.addr_long);
        bind_req.dst_endp      = COORD_ENDPOINT;
        bind_req.req_dst_addr  = params->device_short_addr;
        esp_zb_zdo_device_bind_req(&bind_req, bind_cb, NULL);

        // 2. Also query endpoints via Match_Desc_req in case the switch uses an endpoint other than 1
        esp_zb_zdo_match_desc_req_param_t cmd_req = {};
        cmd_req.dst_nwk_addr     = params->device_short_addr;
        cmd_req.addr_of_interest = params->device_short_addr;
        esp_zb_zdo_find_on_off_light(&cmd_req, user_find_cb, NULL);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        if (err_status != ESP_OK) {
            ESP_LOGW(TAG, "Device update status error: %s", esp_err_to_name(err_status));
            break;
        }
        esp_zb_zdo_signal_device_update_params_t *params =
            (esp_zb_zdo_signal_device_update_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        if (!params) {
            ESP_LOGW(TAG, "Device update: null params");
            break;
        }
        ESP_LOGI(TAG, "Device update (MAC join in progress) — short=0x%04X status=%d",
                 params->short_addr, params->status);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        if (err_status != ESP_OK) {
            ESP_LOGW(TAG, "Device authorized status error: %s", esp_err_to_name(err_status));
            break;
        }
        esp_zb_zdo_signal_device_authorized_params_t *params =
            (esp_zb_zdo_signal_device_authorized_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        if (!params) {
            ESP_LOGW(TAG, "Device authorized: null params");
            break;
        }
        ESP_LOGI(TAG, "Device authorized (Trust Center authenticated) — short=0x%04X auth_status=%d",
                 params->short_addr, params->authorization_status);
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "Device left network");
        break;

    default:
        ESP_LOGI(TAG, "Zigbee Signal 0x%02X (%d) status=%s", sig_type, sig_type, esp_err_to_name(err_status));
        break;
    }
}

// ── Zigbee task ───────────────────────────────────────────────

static void zigbee_task(void *arg)
{
    (void)arg;

    // Explicitly configure Zigbee stack datasets subsystem to use the dedicated 512KB "zigbee" partition
    esp_zigbee_set_storage_name("zigbee");

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

    // v2.x SDK removed the implicit default global link key that earlier versions set --
    // it must now be configured explicitly, or the Trust Center has nothing to use when
    // deriving/transporting a key for a newly joining device, which crashes
    // (Guru Meditation Load access fault) inside aps_secur_key_pair_get_key on first real join.
    static const uint8_t s_tc_link_key[16] = {
        0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C,
        0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39
    }; // "ZigBeeAlliance09"
    esp_zb_secur_TC_standard_preconfigure_key_set(s_tc_link_key);

    // Disable TCLK exchange requirement for commercial Tuya / MOES devices
    // Allows devices to authenticate using standard preconfigured global link key (ZigBeeAlliance09)
    esp_zb_secur_link_key_exchange_required_set(false);
    ezb_secur_tcpol_set_allow_rejoins_with_well_known_key(true);

    // Create standard HA On/Off switch endpoint
    // Registers Basic (Server), Identify (Server & Client), and On/Off (Client) clusters
    // so joining devices (MOES switch) find matching clusters and complete commissioning!
    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_switch_ep_create(COORD_ENDPOINT, &switch_cfg);
    esp_zb_device_register(ep_list);

    // Fix Zigbee coordinator to Channel 20 (2450 MHz)
    // Full +20 dBm TX power, universal Tuya compatibility, 26 MHz RF isolation from Wi-Fi Ch 1 (2412 MHz)
    esp_zb_set_primary_network_channel_set(1 << 20);
    esp_zb_set_tx_power(20);

    ESP_ERROR_CHECK(esp_zb_start(false)); // false = don't erase stored network unless channel mismatch

    // Main loop — runs forever, processes Zigbee stack events
    esp_zb_stack_main_loop();
    // Should never return, but just in case:
    vTaskDelete(NULL);
}

// ── Public API ────────────────────────────────────────────────

void zigbee_coord_init(void)
{
    // Ensure dataset subsystem routes to dedicated partition
    esp_zigbee_set_storage_name("zigbee");

    s_state_mutex = xSemaphoreCreateMutex();
    configASSERT(s_state_mutex);
    memset(s_sw, 0, sizeof(s_sw));

    // Restore paired switches from NVS
    for (int i = 0; i < SWITCH_COUNT; i++) {
        nvs_load_switch((switch_id_t)i);
    }

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

    // Zigbee task — pinned to core 0, stack 10240, priority 6
    xTaskCreatePinnedToCore(zigbee_task, "zigbee", 10240, NULL, 6, NULL, 0);

    // 1-second periodic software timer for pairing window countdown
    s_pair_timer = xTimerCreate("pair_tmr", pdMS_TO_TICKS(1000), pdTRUE, NULL, pair_timer_cb);

    ESP_LOGI(TAG, "Zigbee coordinator task started");
}

void zigbee_coord_permit_join(switch_id_t slot, uint8_t duration_s)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_pair.open       = (duration_s > 0);
    s_pair.target     = slot;
    s_pair.remaining_s = duration_s;
    xSemaphoreGive(s_state_mutex);

    if (duration_s > 0 && s_pair_timer) {
        xTimerReset(s_pair_timer, 0);
    } else if (s_pair_timer) {
        xTimerStop(s_pair_timer, 0);
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    if (duration_s > 0) {
        esp_err_t bdb_err = esp_zb_bdb_open_network(duration_s);
        ESP_LOGI(TAG, "Permit join open: slot=%s duration=%ds (bdb_open: %s)",
                 slot == SWITCH_TOP ? "TOP" : "BOTTOM", duration_s, esp_err_to_name(bdb_err));
    } else {
        esp_err_t bdb_err = esp_zb_bdb_close_network();
        ESP_LOGI(TAG, "Permit join closed: slot=%s (bdb_close: %s)",
                 slot == SWITCH_TOP ? "TOP" : "BOTTOM", esp_err_to_name(bdb_err));
    }
    esp_zb_lock_release();
}

bool zigbee_coord_switch_set(switch_id_t sw, bool on)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool paired         = s_sw[sw].paired;
    uint16_t short_addr = s_sw[sw].short_addr;
    uint8_t ep          = s_sw[sw].endpoint ? s_sw[sw].endpoint : 1;
    xSemaphoreGive(s_state_mutex);

    if (!paired) {
        ESP_LOGW(TAG, "Switch %d not paired — cannot send command", sw);
        return false;
    }

    // Build ZCL On/Off command
    esp_zb_zcl_on_off_cmd_t cmd = {};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    cmd.zcl_basic_cmd.dst_endpoint          = ep;
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

void zigbee_coord_get_network_info(uint16_t *pan_id, uint8_t *channel, bool *online)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (pan_id)  *pan_id  = s_pan_id;
    if (channel) *channel = s_channel;
    if (online)  *online  = s_coord_ready;
    xSemaphoreGive(s_state_mutex);
}

void zigbee_coord_reset_network(void)
{
    ESP_LOGI(TAG, "Resetting Zigbee network and clearing all paired switches...");
    zigbee_coord_clear(SWITCH_TOP);
    zigbee_coord_clear(SWITCH_BOTTOM);
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_bdb_reset_via_local_action();
    esp_zb_lock_release();
}


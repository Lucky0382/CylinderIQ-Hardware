// CylinderIQ Hub V2 — zigbee_crypto.c (ESP32-C6)
//
// Platform cryptographic backend override for esp-zigbee-lib.
// Overrides the weak symbols in libesp-zigbee-core.zczr.*.a.
//
// ROOT CAUSE FIXED:
// In esp-zigbee-lib v2.0.4, the precompiled crypto_platform_psa.c.obj
// contains a struct offset mismatch with ESP-IDF 5.2.x mbedtls 3.x PSA Crypto.
// Specifically, ezb_plat_crypto_aes_setkey_enc and setkey_dec manually wrote
// key usage flags into attributes offset 8. In ESP-IDF 5.2.1, offset 8 is
// mbedtls_svc_key_id_t id, which caused psa_import_key to reject volatile keys
// with PSA_ERROR_INVALID_ARGUMENT (-135). That error returned EZB_ERR_FAIL,
// triggering an __assert_func(0,0,0,0) abort in crypto_aes_ecb_setkey_enc
// whenever a device (e.g. MOES switch TS0001_power) joined the network.
//
// By providing strong symbols here using the official mbedtls PSA setter APIs
// (psa_set_key_usage_flags, psa_set_key_algorithm, psa_set_key_type), the key
// attributes are populated correctly and psa_import_key succeeds 100%.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_ieee802154.h"
#include "esp_ieee802154_types.h"
#include "psa/crypto.h"
#include "mbedtls/aes.h"
#include <ezbee/error.h>
#include <ezbee/platform/crypto.h>

static const char *TAG = "zb_crypto";

void ezb_plat_crypto_init(void)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %ld", (long)status);
        return;
    }
    ESP_LOGI(TAG, "PSA crypto initialized successfully (ESP-IDF 5.2 native)");

    // ── NIST AES-128-ECB self-test (FIPS 197 Appendix B) ──
    // Key:        2b7e151628aed2a6abf7158809cf4f3c
    // Plaintext:  6bc1bee22e409f96e93d7e117393172a
    // Expected:   3ad77bb40d7a3660a89ecaf32466ef97
    static const uint8_t test_key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    static const uint8_t test_pt[16] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
        0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a
    };
    static const uint8_t test_expected[16] = {
        0x3a,0xd7,0x7b,0xb4,0x0d,0x7a,0x36,0x60,
        0xa8,0x9e,0xca,0xf3,0x24,0x66,0xef,0x97
    };

    // Import test key
    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
    mbedtls_svc_key_id_t test_kid = 0;
    status = psa_import_key(&attr, test_key, 16, &test_kid);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "SELF-TEST: key import failed: %ld", (long)status);
        return;
    }

    // Encrypt using multi-part API (same as our override)
    uint8_t test_ct[16] = {0};
    psa_cipher_operation_t op = psa_cipher_operation_init();
    status = psa_cipher_encrypt_setup(&op, test_kid, PSA_ALG_ECB_NO_PADDING);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "SELF-TEST: encrypt_setup failed: %ld", (long)status);
        psa_destroy_key(test_kid);
        return;
    }
    size_t out_len = 0;
    status = psa_cipher_update(&op, test_pt, 16, test_ct, 16, &out_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "SELF-TEST: cipher_update failed: %ld", (long)status);
        psa_cipher_abort(&op);
        psa_destroy_key(test_kid);
        return;
    }
    size_t fin_len = 0;
    psa_cipher_finish(&op, test_ct + out_len, 16 - out_len, &fin_len);
    psa_destroy_key(test_kid);

    // Verify output
    bool pass = (out_len + fin_len == 16) && (memcmp(test_ct, test_expected, 16) == 0);
    ESP_LOGI(TAG, "SELF-TEST AES-128-ECB: %s (out=%d+%d)",
             pass ? "PASS" : "*** FAIL ***", (int)out_len, (int)fin_len);
    if (!pass) {
        ESP_LOGE(TAG, "  Expected: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                 test_expected[0],test_expected[1],test_expected[2],test_expected[3],
                 test_expected[4],test_expected[5],test_expected[6],test_expected[7],
                 test_expected[8],test_expected[9],test_expected[10],test_expected[11],
                 test_expected[12],test_expected[13],test_expected[14],test_expected[15]);
        ESP_LOGE(TAG, "  Got:      %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                 test_ct[0],test_ct[1],test_ct[2],test_ct[3],
                 test_ct[4],test_ct[5],test_ct[6],test_ct[7],
                 test_ct[8],test_ct[9],test_ct[10],test_ct[11],
                 test_ct[12],test_ct[13],test_ct[14],test_ct[15]);
    }
}

ezb_err_t ezb_plat_crypto_aes_init(ezb_crypto_context_t *context)
{
    if (!context || !context->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_svc_key_id_t)) {
        ESP_LOGE(TAG, "aes_init: ctx_size %d too small (need %d)",
                 (int)context->ctx_size, (int)sizeof(mbedtls_svc_key_id_t));
        return EZB_ERR_FAIL;
    }
    *(mbedtls_svc_key_id_t *)context->ctx = 0;
    ESP_LOGD(TAG, "aes_init OK (ctx_size=%d)", (int)context->ctx_size);
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_setkey_enc(ezb_crypto_context_t *context, const ezb_crypto_key_t *key)
{
    if (!context || !context->ctx || !key || !key->key) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_svc_key_id_t)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_svc_key_id_t *p_key_id = (mbedtls_svc_key_id_t *)context->ctx;
    if (*p_key_id != 0) {
        psa_destroy_key(*p_key_id);
        *p_key_id = 0;
    }

    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key->key_len * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);

    psa_status_t status = psa_import_key(&attributes, key->key, key->key_len, p_key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key (enc) failed: %ld (key_len=%d)", (long)status, (int)key->key_len);
        return EZB_ERR_FAIL;
    }
    ESP_LOGI(TAG, "AES enc key imported OK (key_id=%lu, key_len=%d)", (unsigned long)*p_key_id, (int)key->key_len);
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_setkey_dec(ezb_crypto_context_t *context, const ezb_crypto_key_t *key)
{
    if (!context || !context->ctx || !key || !key->key) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_svc_key_id_t)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_svc_key_id_t *p_key_id = (mbedtls_svc_key_id_t *)context->ctx;
    if (*p_key_id != 0) {
        psa_destroy_key(*p_key_id);
        *p_key_id = 0;
    }

    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, key->key_len * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);

    psa_status_t status = psa_import_key(&attributes, key->key, key->key_len, p_key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key (dec) failed: %ld (key_len=%d)", (long)status, (int)key->key_len);
        return EZB_ERR_FAIL;
    }
    ESP_LOGI(TAG, "AES dec key imported OK (key_id=%lu, key_len=%d)", (unsigned long)*p_key_id, (int)key->key_len);
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_encrypt(ezb_crypto_context_t *context, const uint8_t *input, uint8_t *output)
{
    if (!context || !context->ctx || !input || !output) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_svc_key_id_t)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_svc_key_id_t key_id = *(mbedtls_svc_key_id_t *)context->ctx;

    // Use multi-part cipher API to avoid any single-part buffer layout issues
    psa_cipher_operation_t op = psa_cipher_operation_init();
    psa_status_t status = psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_ECB_NO_PADDING);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_cipher_encrypt_setup failed: %ld (key_id=%lu)", (long)status, (unsigned long)key_id);
        psa_cipher_abort(&op);
        return EZB_ERR_FAIL;
    }

    size_t out_len = 0;
    status = psa_cipher_update(&op, input, 16, output, 16, &out_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_cipher_update (enc) failed: %ld", (long)status);
        psa_cipher_abort(&op);
        return EZB_ERR_FAIL;
    }

    size_t finish_len = 0;
    status = psa_cipher_finish(&op, output + out_len, 16 - out_len, &finish_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_cipher_finish (enc) failed: %ld", (long)status);
        psa_cipher_abort(&op);
        return EZB_ERR_FAIL;
    }

    ESP_LOGI(TAG, "AES encrypt OK (key_id=%lu, out=%d+%d bytes)", (unsigned long)key_id, (int)out_len, (int)finish_len);
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_decrypt(ezb_crypto_context_t *context, const uint8_t *input, uint8_t *output)
{
    if (!context || !context->ctx || !input || !output) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_svc_key_id_t)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_svc_key_id_t key_id = *(mbedtls_svc_key_id_t *)context->ctx;

    // Use multi-part cipher API
    psa_cipher_operation_t op = psa_cipher_operation_init();
    psa_status_t status = psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_ECB_NO_PADDING);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_cipher_decrypt_setup failed: %ld (key_id=%lu)", (long)status, (unsigned long)key_id);
        psa_cipher_abort(&op);
        return EZB_ERR_FAIL;
    }

    size_t out_len = 0;
    status = psa_cipher_update(&op, input, 16, output, 16, &out_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_cipher_update (dec) failed: %ld", (long)status);
        psa_cipher_abort(&op);
        return EZB_ERR_FAIL;
    }

    size_t finish_len = 0;
    status = psa_cipher_finish(&op, output + out_len, 16 - out_len, &finish_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_cipher_finish (dec) failed: %ld", (long)status);
        psa_cipher_abort(&op);
        return EZB_ERR_FAIL;
    }

    ESP_LOGI(TAG, "AES decrypt OK (key_id=%lu, out=%d+%d bytes)", (unsigned long)key_id, (int)out_len, (int)finish_len);
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_free(ezb_crypto_context_t *context)
{
    if (!context || !context->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_svc_key_id_t)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_svc_key_id_t *p_key_id = (mbedtls_svc_key_id_t *)context->ctx;
    if (*p_key_id != 0) {
        psa_destroy_key(*p_key_id);
        *p_key_id = 0;
    }
    return EZB_ERR_NONE;
}

void ezb_plat_crypto_random_init(void)
{
    ezb_plat_crypto_init();
}

void ezb_plat_crypto_random_deinit(void)
{
}

ezb_err_t ezb_plat_crypto_random_get(uint8_t *output, uint16_t output_length)
{
    if (!output) {
        return EZB_ERR_INV_ARG;
    }
    psa_status_t status = psa_generate_random(output, output_length);
    if (status != PSA_SUCCESS) {
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_sha256_init(ezb_crypto_context_t *ctx)
{
    if (!ctx || !ctx->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (ctx->ctx_size < sizeof(psa_hash_operation_t)) {
        return EZB_ERR_FAIL;
    }
    memset(ctx->ctx, 0, sizeof(psa_hash_operation_t));
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_sha256_start(ezb_crypto_context_t *ctx)
{
    if (!ctx || !ctx->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (ctx->ctx_size < sizeof(psa_hash_operation_t)) {
        return EZB_ERR_FAIL;
    }
    psa_hash_operation_t *op = (psa_hash_operation_t *)ctx->ctx;
    *op = psa_hash_operation_init();
    psa_status_t status = psa_hash_setup(op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_sha256_update(ezb_crypto_context_t *ctx, const uint8_t *data, uint16_t data_len)
{
    if (!ctx || !ctx->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (ctx->ctx_size < sizeof(psa_hash_operation_t)) {
        return EZB_ERR_FAIL;
    }
    psa_hash_operation_t *op = (psa_hash_operation_t *)ctx->ctx;
    psa_status_t status = psa_hash_update(op, data, data_len);
    if (status != PSA_SUCCESS) {
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_sha256_finish(ezb_crypto_context_t *ctx, uint8_t *output, uint16_t output_len)
{
    if (!ctx || !ctx->ctx || !output) {
        return EZB_ERR_INV_ARG;
    }
    if (ctx->ctx_size < sizeof(psa_hash_operation_t)) {
        return EZB_ERR_FAIL;
    }
    psa_hash_operation_t *op = (psa_hash_operation_t *)ctx->ctx;
    size_t out_len = 0;
    psa_status_t status = psa_hash_finish(op, output, output_len, &out_len);
    if (status != PSA_SUCCESS) {
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_sha256_free(ezb_crypto_context_t *ctx)
{
    if (!ctx || !ctx->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (ctx->ctx_size < sizeof(psa_hash_operation_t)) {
        return EZB_ERR_FAIL;
    }
    psa_hash_operation_t *op = (psa_hash_operation_t *)ctx->ctx;
    psa_hash_abort(op);
    return EZB_ERR_NONE;
}

// ── Linker Wraps for esp-zigbee-lib fixes ──────────────────────

// 1. Fix MbedTLS 3.x struct offset mismatch in precompiled aes_ccm.c.obj
// In precompiled esp-zigbee-lib, crypto_psa_import_aes_key wrote:
//   attr.id = usage flags (offset 8)
//   attr.policy.usage = algorithm (offset 12)
// In ESP-IDF 5.2.x MbedTLS 3.x, volatile keys cannot have a non-zero key_id,
// which caused psa_import_key to reject CCM* keys with PSA_ERROR_INVALID_ARGUMENT (-135).
psa_status_t __real_psa_import_key(const psa_key_attributes_t *attributes,
                                  const uint8_t *data,
                                  size_t data_length,
                                  mbedtls_svc_key_id_t *key);

psa_status_t __wrap_psa_import_key(const psa_key_attributes_t *attributes,
                                  const uint8_t *data,
                                  size_t data_length,
                                  mbedtls_svc_key_id_t *key)
{
    if (attributes &&
        PSA_KEY_LIFETIME_IS_VOLATILE(psa_get_key_lifetime(attributes)) &&
        MBEDTLS_SVC_KEY_ID_GET_KEY_ID(psa_get_key_id(attributes)) != 0)
    {
        // Legacy esp-zigbee binary struct offset mismatch detected!
        const uint32_t *raw = (const uint32_t *)attributes;
        uint32_t legacy_usage = raw[2]; // offset 8 (usage flags)
        uint32_t legacy_alg   = raw[3]; // offset 12 (algorithm)

        psa_key_attributes_t fixed_attr = psa_key_attributes_init();
        psa_set_key_type(&fixed_attr, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&fixed_attr, data_length * 8);
        psa_set_key_usage_flags(&fixed_attr, legacy_usage | PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&fixed_attr, legacy_alg);

        psa_status_t status = __real_psa_import_key(&fixed_attr, data, data_length, key);
        ESP_LOGI(TAG, "psa_import_key [FIXED LEGACY MISMATCH]: usage=0x%08lx alg=0x%08lx -> kid=%lu, ret=%ld",
                 (unsigned long)legacy_usage, (unsigned long)legacy_alg,
                 (unsigned long)*key, (long)status);
        return status;
    }

    return __real_psa_import_key(attributes, data, data_length, key);
}

// 2. Fix missing NWK address registration in Trust Center update device indication
// When status == 1 (UNSECURE_JOIN), zdo_app_tc.c.obj skips nwk_address_update().
// This caused aps_send_cmd() / aps_relay_cmd() to fail address resolution and
// silently drop the Transport Key packet without sending it.
extern ezb_err_t nwk_address_update(const uint8_t *ieee_addr, uint16_t short_addr, uint16_t *ref_out);
extern void __real_apsme_update_device_indication(void *param);

void __wrap_apsme_update_device_indication(void *param)
{
    if (param) {
        const uint8_t *ieee = (const uint8_t *)param + 8;
        uint16_t short_addr = *(const uint16_t *)((const uint8_t *)param + 16);
        uint8_t status = *((const uint8_t *)param + 18);

        ESP_LOGI("zb_tc", "apsme_update_device_indication: IEEE=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X short=0x%04X status=%u",
                 ieee[7], ieee[6], ieee[5], ieee[4],
                 ieee[3], ieee[2], ieee[1], ieee[0],
                 short_addr, status);

        // Pre-populate address mapping so aps_send_cmd / aps_relay_cmd finds the destination address
        uint16_t ref = 0;
        ezb_err_t err = nwk_address_update(ieee, short_addr, &ref);
        ESP_LOGI("zb_tc", "Pre-populating address mapping: nwk_address_update -> ret=%d ref=%u", err, ref);
    }
    __real_apsme_update_device_indication(param);
}

// 3. Low-level IEEE 802.15.4 Radio Transmit & Receive Diagnostics
extern esp_err_t __real_esp_ieee802154_transmit(const uint8_t *frame, bool cca);
esp_err_t __wrap_esp_ieee802154_transmit(const uint8_t *frame, bool cca)
{
    if (frame) {
        uint8_t len = frame[0];
        uint16_t fcf = len >= 2 ? (frame[1] | (frame[2] << 8)) : 0;
        uint8_t seq = len >= 3 ? frame[3] : 0;
        uint8_t type = fcf & 0x07;
        bool ack_req = (fcf >> 5) & 1;
        uint8_t dst_mode = (fcf >> 10) & 3;

        esp_rom_printf("[RADIO TX] len=%u fcf=0x%04x (type=%u ack=%d dst_mode=%d) seq=%u\n",
                       len, fcf, type, ack_req, dst_mode, seq);
    }
    return __real_esp_ieee802154_transmit(frame, cca);
}

extern void __real_esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_frame_info);
void __wrap_esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_frame_info)
{
    esp_rom_printf("[RADIO TX DONE] ack=%s seq=%u\n",
                   ack ? "YES" : "NO",
                   frame ? frame[3] : 0);
    __real_esp_ieee802154_transmit_done(frame, ack, ack_frame_info);
}

extern void __real_esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error);
void __wrap_esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    esp_rom_printf("[RADIO TX FAILED] error=%d seq=%u\n",
                   (int)error,
                   frame ? frame[3] : 0);
    __real_esp_ieee802154_transmit_failed(frame, error);
}

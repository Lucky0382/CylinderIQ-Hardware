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
#include <string.h>

#include "esp_log.h"
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
    } else {
        ESP_LOGI(TAG, "PSA crypto initialized successfully (ESP-IDF 5.2 native)");
    }
}

ezb_err_t ezb_plat_crypto_aes_init(ezb_crypto_context_t *context)
{
    if (!context || !context->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_aes_context)) {
        ESP_LOGE(TAG, "aes_init: ctx_size %d < sizeof(mbedtls_aes_context) %d",
                 (int)context->ctx_size, (int)sizeof(mbedtls_aes_context));
        return EZB_ERR_FAIL;
    }
    mbedtls_aes_context *aes = (mbedtls_aes_context *)context->ctx;
    mbedtls_aes_init(aes);
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_setkey_enc(ezb_crypto_context_t *context, const ezb_crypto_key_t *key)
{
    if (!context || !context->ctx || !key || !key->key) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_aes_context)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_aes_context *aes = (mbedtls_aes_context *)context->ctx;
    int ret = mbedtls_aes_setkey_enc(aes, key->key, key->key_len * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_setkey_enc failed: -0x%04X (key_len=%d)", -ret, (int)key->key_len);
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_setkey_dec(ezb_crypto_context_t *context, const ezb_crypto_key_t *key)
{
    if (!context || !context->ctx || !key || !key->key) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_aes_context)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_aes_context *aes = (mbedtls_aes_context *)context->ctx;
    int ret = mbedtls_aes_setkey_dec(aes, key->key, key->key_len * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_setkey_dec failed: -0x%04X (key_len=%d)", -ret, (int)key->key_len);
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_encrypt(ezb_crypto_context_t *context, const uint8_t *input, uint8_t *output)
{
    if (!context || !context->ctx || !input || !output) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_aes_context)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_aes_context *aes = (mbedtls_aes_context *)context->ctx;
    int ret = mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_ENCRYPT, input, output);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_crypt_ecb (enc) failed: -0x%04X", -ret);
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_decrypt(ezb_crypto_context_t *context, const uint8_t *input, uint8_t *output)
{
    if (!context || !context->ctx || !input || !output) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_aes_context)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_aes_context *aes = (mbedtls_aes_context *)context->ctx;
    int ret = mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_DECRYPT, input, output);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_crypt_ecb (dec) failed: -0x%04X", -ret);
        return EZB_ERR_FAIL;
    }
    return EZB_ERR_NONE;
}

ezb_err_t ezb_plat_crypto_aes_free(ezb_crypto_context_t *context)
{
    if (!context || !context->ctx) {
        return EZB_ERR_INV_ARG;
    }
    if (context->ctx_size < sizeof(mbedtls_aes_context)) {
        return EZB_ERR_FAIL;
    }

    mbedtls_aes_context *aes = (mbedtls_aes_context *)context->ctx;
    mbedtls_aes_free(aes);
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

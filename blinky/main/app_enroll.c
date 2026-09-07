#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#include "psa/crypto.h"
#include "mbedtls/pk.h"        /* mbedtls_pk_wrap_psa: bind a PSA key into a pk context */
#include "mbedtls/x509_csr.h"  /* mbedtls_x509write_csr_* (CSR writer is present in mbedTLS 4) */
#include "mbedtls/error.h"

#include "app_enroll.h"

#define TAG "app_enroll"

#define APP_ENROLL_NVS_NS        "enroll"
#define APP_ENROLL_KEY_CA        "ca_crt"
#define APP_ENROLL_KEY_FACT_CRT  "fact_crt"
#define APP_ENROLL_KEY_FACT_KEY  "fact_key"
#define APP_ENROLL_KEY_DEV_CRT   "dev_crt"
#define APP_ENROLL_KEY_URL       "server_url"
/* Persistent PSA key id for the on-device EC key (stored in NVS-backed ITS). */
#define APP_ENROLL_PSA_DEV_KEY_ID 0x12000001

static nvs_handle_t s_nvs;
static bool s_nvs_ok = false;
static bool s_provisioned = false;   /* factory identity loaded from NVS */
static char s_device_id[32];
static char *s_ca_crt = NULL;
static char *s_fact_crt = NULL;
static char *s_fact_key = NULL;
static char s_server_url[256];

/* ------------------------- NVS helpers ---------------------------------- */

static esp_err_t nvs_str_alloc(const char *key, char **out)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(s_nvs, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(s_nvs, key, buf, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[len] = '\0';
    *out = buf;
    return ESP_OK;
}

static bool nvs_has(const char *key)
{
    size_t len = 0;
    return nvs_get_str(s_nvs, key, NULL, &len) == ESP_OK;
}

static esp_err_t nvs_store_str(const char *key, const char *value)
{
    return nvs_set_str(s_nvs, key, value);
}

/* ------------------------- public API ----------------------------------- */

esp_err_t AppEnroll_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    /* If another component already initialized NVS, the init above returns an
     * error; nvs_open() below still works. */

    err = nvs_open(APP_ENROLL_NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s) failed: %s", APP_ENROLL_NVS_NS, esp_err_to_name(err));
        return err;
    }
    s_nvs_ok = true;

    /* device_id from the base MAC (stable + unique), e.g. device-345f45c4f894 */
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id),
                 "device-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "device-unknown");
    }

    /* Factory identity (provisioned at manufacturing, not in the image). */
    if (nvs_str_alloc(APP_ENROLL_KEY_CA, &s_ca_crt) == ESP_OK &&
        nvs_str_alloc(APP_ENROLL_KEY_FACT_CRT, &s_fact_crt) == ESP_OK &&
        nvs_str_alloc(APP_ENROLL_KEY_FACT_KEY, &s_fact_key) == ESP_OK) {
        s_provisioned = true;
        ESP_LOGI(TAG, "factory identity loaded from NVS (CA %d B, cert %d B, key %d B)",
                 (int)strlen(s_ca_crt), (int)strlen(s_fact_crt), (int)strlen(s_fact_key));
    } else {
        s_provisioned = false;
        ESP_LOGW(TAG, "factory identity NOT provisioned in NVS - device cannot enroll "
                      "(run scripts/factory-nvs.sh at manufacturing)");
    }

    /* Enroll server URL (e.g. https://<host>:9443/enroll), provisioned in NVS. */
    size_t url_len = sizeof(s_server_url);
    if (nvs_get_str(s_nvs, APP_ENROLL_KEY_URL, s_server_url, &url_len) != ESP_OK) {
        s_server_url[0] = '\0';
        ESP_LOGW(TAG, "enroll server URL not provisioned in NVS (key '%s')",
                 APP_ENROLL_KEY_URL);
    } else {
        ESP_LOGI(TAG, "enroll server URL: %s", s_server_url);
    }
    return ESP_OK;
}

bool AppEnroll_is_enrolled(void)
{
    if (!s_nvs_ok) {
        return false;
    }
    psa_crypto_init(); /* idempotent; needed before psa_get_key_attributes() */
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t st = psa_get_key_attributes(APP_ENROLL_PSA_DEV_KEY_ID, &attrs);
    psa_reset_key_attributes(&attrs);
    return st == PSA_SUCCESS && nvs_has(APP_ENROLL_KEY_DEV_CRT);
}

const char *AppEnroll_device_id(void)
{
    return s_device_id;
}

esp_err_t AppEnroll_get_device_cert(char **pem_out, int *len_out)
{
    if (pem_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pem_out = NULL;
    if (len_out) {
        *len_out = 0;
    }
    char *pem = NULL;
    esp_err_t err = nvs_str_alloc(APP_ENROLL_KEY_DEV_CRT, &pem);
    if (err != ESP_OK) {
        return err;
    }
    *pem_out = pem;
    if (len_out) {
        *len_out = (int)strlen(pem);
    }
    return ESP_OK;
}

/* ------------- PSA device key + PKCS#10 CSR (mbedTLS 4 / IDF 6) ---------- */

static void log_mbedtls_error(int ret, const char *what)
{
    char buf[128];
    mbedtls_strerror(ret, buf, sizeof(buf));
    ESP_LOGE(TAG, "%s failed: -0x%04X (%s)", what, (unsigned)-ret, buf);
}

/* Ensure the persistent PSA device key exists and bind it into *pk (caller
 * must mbedtls_pk_init() it first). PSA stores the key in NVS-backed ITS, so
 * it survives reboot and stays matched to the cert EJBCA issued for it. */
static esp_err_t ensure_device_key(mbedtls_pk_context *pk)
{
    psa_status_t st;
    mbedtls_svc_key_id_t key_id;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;

    psa_crypto_init(); /* idempotent */

    /* Already generated? Open the existing persistent key. */
    if (psa_get_key_attributes(APP_ENROLL_PSA_DEV_KEY_ID, &attrs) == PSA_SUCCESS) {
        bool is_pair = PSA_KEY_TYPE_IS_ECC_KEY_PAIR(psa_get_key_type(&attrs));
        psa_reset_key_attributes(&attrs);
        if (is_pair) {
            ESP_LOGI(TAG, "using existing PSA device key");
            return mbedtls_pk_wrap_psa(pk, APP_ENROLL_PSA_DEV_KEY_ID) == 0 ? ESP_OK : ESP_FAIL;
        }
    } else {
        psa_reset_key_attributes(&attrs);
    }

    /* Generate a fresh persistent EC P-256 device key. */
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_lifetime(&attrs, PSA_KEY_LIFETIME_PERSISTENT);
    psa_set_key_id(&attrs, APP_ENROLL_PSA_DEV_KEY_ID);

    st = psa_generate_key(&attrs, &key_id);
    psa_reset_key_attributes(&attrs);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key failed: %d", (int) st);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "generated persistent PSA EC P-256 device key");
    return mbedtls_pk_wrap_psa(pk, key_id) == 0 ? ESP_OK : ESP_FAIL;
}

/* Build a PKCS#10 CSR in PEM with subject CN=device_id. mbedTLS 4.0's writer
 * signs via PSA (no RNG callback argument any more). */
static esp_err_t build_csr(mbedtls_pk_context *pk, char *out, size_t outlen)
{
    int ret;
    char subject[64];
    mbedtls_x509write_csr csr;

    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, pk);
    snprintf(subject, sizeof(subject), "CN=%s", s_device_id);
    ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
    if (ret == 0) {
        ret = mbedtls_x509write_csr_pem(&csr, (unsigned char *)out, outlen);
    }
    mbedtls_x509write_csr_free(&csr);

    if (ret != 0) {
        log_mbedtls_error(ret, "CSR build");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* -------------------- mTLS POST to enroll ------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (r->buf == NULL) {
        r->cap = 4096;
        r->buf = malloc(r->cap);
        if (r->buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        r->len = 0;
        r->buf[0] = '\0';
    }
    if (r->len + evt->data_len + 1 > r->cap) {
        size_t ncap = r->cap * 2;
        char *nb = realloc(r->buf, ncap);
        if (nb == NULL) {
            return ESP_ERR_NO_MEM;
        }
        r->buf = nb;
        r->cap = ncap;
    }
    memcpy(r->buf + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

/* Minimal JSON body builder (avoids a cJSON dependency): only standard JSON
 * string escaping is needed for the two fields. */
static void json_escape_into(char *dst, size_t *len, const char *s)
{
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\\': dst[(*len)++] = '\\'; dst[(*len)++] = '\\'; break;
        case '"':  dst[(*len)++] = '\\'; dst[(*len)++] = '"';  break;
        case '\n': dst[(*len)++] = '\\'; dst[(*len)++] = 'n';  break;
        case '\r': dst[(*len)++] = '\\'; dst[(*len)++] = 'r';  break;
        case '\t': dst[(*len)++] = '\\'; dst[(*len)++] = 't';  break;
        default:   dst[(*len)++] = *p;   break;
        }
    }
}

static char *build_enroll_body(const char *csr)
{
    size_t cap = strlen(csr) * 2 + strlen(s_device_id) * 2 + 64;
    char *body = malloc(cap);
    if (body == NULL) {
        return NULL;
    }
    size_t len = 0;
#define ENROLL_RAW(str) do { const char *_s = (str); size_t _l = strlen(_s); \
        memcpy(body + len, _s, _l); len += _l; } while (0)
    ENROLL_RAW("{\"device_id\":\"");
    json_escape_into(body, &len, s_device_id);
    ENROLL_RAW("\",\"csr\":\"");
    json_escape_into(body, &len, csr);
    ENROLL_RAW("\"}");
#undef ENROLL_RAW
    body[len] = '\0';
    return body;
}

/* POST {device_id, csr} over mTLS; on 200 return the PEM device cert. */
static esp_err_t enroll_post(const char *csr_pem, char **dev_cert_out)
{
    *dev_cert_out = NULL;

    char *body = build_enroll_body(csr_pem);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    http_resp_t resp = {0};
    esp_http_client_config_t cfg = {
        .url = s_server_url,
        .method = HTTP_METHOD_POST,
        .cert_pem = s_ca_crt,
        .client_cert_pem = s_fact_crt,
        .client_key_pem = s_fact_key,
        .skip_cert_common_name_check = true,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .event_handler = http_event_handler,
        .user_data = &resp,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(body);
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enroll POST transport failed: %s", esp_err_to_name(err));
        if (resp.buf) {
            free(resp.buf);
        }
        return ESP_FAIL;
    }

    if (status == 200) {
        if (resp.buf == NULL || resp.len == 0) {
            ESP_LOGE(TAG, "enroll returned 200 with empty body");
            if (resp.buf) {
                free(resp.buf);
            }
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "enroll HTTP 200 (%d bytes)", (int)resp.len);
        *dev_cert_out = resp.buf; /* NUL-terminated PEM */
        return ESP_OK;
    }

    if (resp.buf) {
        ESP_LOGW(TAG, "enroll HTTP %d: %.*s", status, (int)resp.len, resp.buf);
    } else {
        ESP_LOGW(TAG, "enroll HTTP %d", status);
    }
    if (resp.buf) {
        free(resp.buf);
    }
    if (status >= 400 && status < 500) {
        return ESP_ERR_INVALID_RESPONSE; /* permanent: bad CSR/id or rejected */
    }
    return ESP_FAIL; /* 5xx / 3xx etc: retry later */
}

esp_err_t AppEnroll_ensure(void)
{
    if (!s_nvs_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_provisioned) {
        ESP_LOGW(TAG, "cannot enroll: factory identity not provisioned");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_server_url[0] == '\0') {
        ESP_LOGW(TAG, "cannot enroll: server URL not provisioned in NVS (key '%s')",
                 APP_ENROLL_KEY_URL);
        return ESP_ERR_NOT_FOUND;
    }
    if (AppEnroll_is_enrolled()) {
        ESP_LOGI(TAG, "already enrolled (CN=%s)", s_device_id);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "enrolling device %s ...", s_device_id);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    esp_err_t err = ensure_device_key(&pk);
    if (err != ESP_OK) {
        mbedtls_pk_free(&pk);
        return err;
    }

    static char csr[4096]; /* keep big buffer off the small task stack */
    err = build_csr(&pk, csr, sizeof(csr));
    mbedtls_pk_free(&pk);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "CSR ready (%d bytes)", (int)strlen(csr));

    char *dev_cert = NULL;
    err = enroll_post(csr, &dev_cert);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_store_str(APP_ENROLL_KEY_DEV_CRT, dev_cert);
    free(dev_cert);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not persist device cert: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "enrolled OK: CN=%s", s_device_id);
    return ESP_OK;
}

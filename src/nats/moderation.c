#include "moderation.h"
#include "crypto/fly_crypto.h"
#include <cjson/cJSON.h>
#include <cwist/core/mem/alloc.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

#define MODERATION_PROTOCOL_VERSION 1
#define MODERATION_DECISION_MAX_BYTES 65536
#define MODERATION_CONTENT_MAX_BYTES (1024 * 1024)

static const char *json_string(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static bool json_integer(cJSON *obj, const char *key, long long *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 253402300799.0) return false;
    *out = (long long)item->valuedouble;
    return (double)*out == item->valuedouble;
}

static bool valid_correlation(const char *id) {
    if (!id || strlen(id) != FLY_MODERATION_CORRELATION_LEN) return false;
    for (size_t i = 0; i < FLY_MODERATION_CORRELATION_LEN; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

static bool valid_decision_id(const char *id) {
    size_t len = id ? strlen(id) : 0;
    if (len == 0 || len > 64) return false;
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '-' || c == '_')) return false;
    }
    return true;
}

static bool valid_envelope(cJSON *obj, const char *type, size_t fields) {
    if (!obj || !cJSON_IsObject(obj) || cJSON_GetArraySize(obj) != (int)fields) return false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(obj, "version");
    const char *kind = json_string(obj, "type");
    const char *algorithm = json_string(obj, "sig_alg");
    return cJSON_IsNumber(version) && version->valueint == MODERATION_PROTOCOL_VERSION &&
           kind && strcmp(kind, type) == 0 && algorithm && strcmp(algorithm, "ML-DSA-65") == 0 &&
           json_string(obj, "sig_b64") && json_string(obj, "pubkey_b64");
}

bool fly_moderation_correlation_id(char out[FLY_MODERATION_CORRELATION_LEN + 1]) {
    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1) return false;
    for (size_t i = 0; i < sizeof(random); i++) snprintf(out + i * 2, 3, "%02x", random[i]);
    return true;
}

char *fly_moderation_build_request(const char *correlation_id, const char *target_type,
                                   const char *content, const char *reason,
                                   long long issued_at, long long expires_at) {
    if (!valid_correlation(correlation_id) || !content || strlen(content) > MODERATION_CONTENT_MAX_BYTES ||
        !reason || strlen(reason) > 128 || expires_at <= issued_at ||
        (strcmp(target_type, MODERATION_POST) != 0 && strcmp(target_type, MODERATION_COMMENT) != 0)) return NULL;
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;
    cJSON_AddNumberToObject(body, "version", MODERATION_PROTOCOL_VERSION);
    cJSON_AddStringToObject(body, "type", "moderation.review");
    cJSON_AddStringToObject(body, "correlation_id", correlation_id);
    cJSON_AddStringToObject(body, "target_type", target_type);
    cJSON_AddStringToObject(body, "content", content);
    cJSON_AddStringToObject(body, "reason", reason);
    cJSON_AddNumberToObject(body, "issued_at", (double)issued_at);
    cJSON_AddNumberToObject(body, "expires_at", (double)expires_at);
    char *canonical = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!canonical) return NULL;

    char *signature = NULL;
    char *pubkey = NULL;
    if (!fly_crypto_sign_moderation_request((const uint8_t *)canonical, strlen(canonical), &signature) ||
        !fly_crypto_pubkey_export(&pubkey)) {
        cwist_free(canonical);
        if (signature) cwist_free(signature);
        return NULL;
    }
    cJSON *envelope = cJSON_Parse(canonical);
    cwist_free(canonical);
    if (!envelope) {
        cwist_free(signature);
        cwist_free(pubkey);
        return NULL;
    }
    cJSON_AddStringToObject(envelope, "sig_alg", "ML-DSA-65");
    cJSON_AddStringToObject(envelope, "sig_b64", signature);
    cJSON_AddStringToObject(envelope, "pubkey_b64", pubkey);
    char *json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    cwist_free(signature);
    cwist_free(pubkey);
    return json;
}

moderation_external_result_t fly_moderation_process_decision(cwist_db *db, const char *data, size_t len,
                                                            const char *reviewer_pubkey_b64, long long now) {
    if (!db || !data || len == 0 || len > MODERATION_DECISION_MAX_BYTES || !reviewer_pubkey_b64) {
        return MODERATION_EXTERNAL_REJECTED;
    }
    cJSON *obj = cJSON_ParseWithLength(data, len);
    if (!valid_envelope(obj, "moderation.decision", 10)) {
        if (obj) cJSON_Delete(obj);
        return MODERATION_EXTERNAL_REJECTED;
    }
    const char *correlation_id = json_string(obj, "correlation_id");
    const char *decision_id = json_string(obj, "decision_id");
    const char *action = json_string(obj, "action");
    const char *reason = json_string(obj, "reason");
    const char *signature = json_string(obj, "sig_b64");
    const char *pubkey = json_string(obj, "pubkey_b64");
    long long issued_at = 0;
    if (!valid_correlation(correlation_id) || !valid_decision_id(decision_id) || !reason || strlen(reason) > 256 ||
        !json_integer(obj, "issued_at", &issued_at) || strcmp(pubkey, reviewer_pubkey_b64) != 0 ||
        (strcmp(action, "allow") != 0 && strcmp(action, "pending") != 0 && strcmp(action, "reject") != 0)) {
        cJSON_Delete(obj);
        return MODERATION_EXTERNAL_REJECTED;
    }

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        cJSON_Delete(obj);
        return MODERATION_EXTERNAL_ERROR;
    }
    cJSON_AddNumberToObject(body, "version", MODERATION_PROTOCOL_VERSION);
    cJSON_AddStringToObject(body, "type", "moderation.decision");
    cJSON_AddStringToObject(body, "correlation_id", correlation_id);
    cJSON_AddStringToObject(body, "decision_id", decision_id);
    cJSON_AddStringToObject(body, "action", action);
    cJSON_AddNumberToObject(body, "issued_at", (double)issued_at);
    cJSON_AddStringToObject(body, "reason", reason);
    char *canonical = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    bool verified = canonical && fly_crypto_verify_moderation_decision(pubkey, (const uint8_t *)canonical,
                                                                       strlen(canonical), signature);
    if (canonical) cwist_free(canonical);
    if (!verified) {
        cJSON_Delete(obj);
        return MODERATION_EXTERNAL_REJECTED;
    }

    moderation_action_t decision = strcmp(action, "allow") == 0 ? MODERATION_ALLOW
                                 : strcmp(action, "reject") == 0 ? MODERATION_REJECT : MODERATION_PENDING;
    moderation_external_result_t result = db_moderation_external_apply(db, correlation_id, decision_id, decision,
                                                                       issued_at, reason, now);
    cJSON_Delete(obj);
    return result;
}

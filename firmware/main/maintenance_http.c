#include "maintenance_http.h"
#include "device_maintenance.h"
#include "device_identity.h"
#include "device_name.h"
#include "radio_activity.h"
#include "peer_labels.h"
#include "netp2pnet.h"
#include "netcom.h"
#include "app_control.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "lwip/inet.h"
#include "lwip/sockets.h"

static esp_err_t reply(httpd_req_t *req, cJSON *out, int status)
{
    if (status == 400) { httpd_resp_set_status(req, "400 Bad Request"); }
    else if (status == 409) { httpd_resp_set_status(req, "409 Conflict"); }
    else if (status == 503) { httpd_resp_set_status(req, "503 Service Unavailable"); }
    char *text = cJSON_PrintUnformatted(out); cJSON_Delete(out);
    if (!text) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN); free(text); return err;
}
static esp_err_t error(httpd_req_t *req, const char *message, int status)
{
    cJSON *out = cJSON_CreateObject(); cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "err", message);
    return reply(req, out, status);
}
static bool identity_matches(httpd_req_t *req)
{
    char id[24];
    if (httpd_req_get_hdr_value_str(req, "X-ORL-Device", id, sizeof(id)) == ESP_OK && !strcmp(id, device_identity_id())) { return true; }
    httpd_resp_set_hdr(req, "Connection", "close");
    (void)error(req, "设备身份已变化，请重新连接后再操作", 409); return false;
}
static bool uint32_field(const cJSON *body, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 || v->valuedouble > UINT32_MAX ||
        floor(v->valuedouble) != v->valuedouble) { return false; }
    *out = (uint32_t)v->valuedouble; return true;
}
static cJSON *maintenance_snapshot(void)
{
    char json[1500]; device_maintenance_json(json, sizeof(json));
    cJSON *out = cJSON_Parse(json);
    if (out) {
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "device_id", device_identity_id());
        cJSON_AddStringToObject(out, "boot_id", radio_activity_boot_id());
    }
    return out;
}
esp_err_t maintenance_http(httpd_req_t *req)
{
    if (req->method == HTTP_GET) { return reply(req, maintenance_snapshot(), 200); }
    if (!identity_matches(req)) { return ESP_FAIL; }
    cJSON *body = app_read_json(req); if (!body) { return ESP_FAIL; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "set")) {
        const cJSON *clean = cJSON_GetObjectItemCaseSensitive(body, "cleanup_enabled");
        const cJSON *restart = cJSON_GetObjectItemCaseSensitive(body, "restart_enabled");
        uint32_t revision, days, minute;
        if (cJSON_IsBool(clean) && cJSON_IsBool(restart) && uint32_field(body, "revision", &revision) &&
            uint32_field(body, "restart_days", &days) && days <= MAINTENANCE_WEEKDAYS &&
            uint32_field(body, "restart_minute", &minute) && minute < MAINTENANCE_MINUTES_PER_DAY &&
            (!cJSON_IsTrue(restart) || days)) {
            const maintenance_config_t cfg = {
                .version = 2, .restart_days = (uint8_t)days, .restart_minute = (uint16_t)minute,
                .cleanup_enabled = cJSON_IsTrue(clean), .restart_enabled = cJSON_IsTrue(restart),
            };
            err = device_maintenance_set(&cfg, revision);
        }
    } else if (cJSON_IsString(op) && !strcmp(op->valuestring, "cleanup")) {
        err = device_maintenance_cleanup();
    } else if (cJSON_IsString(op) && (!strcmp(op->valuestring, "reboot") ||
                                      !strcmp(op->valuestring, "factory_reset"))) {
        const cJSON *boot = cJSON_GetObjectItemCaseSensitive(body, "boot_id");
        const bool factory = !strcmp(op->valuestring, "factory_reset");
        err = ESP_ERR_INVALID_STATE;
        if (cJSON_IsString(boot) && !strcmp(boot->valuestring, radio_activity_boot_id())) {
            err = factory ? device_maintenance_factory_reset() : device_maintenance_reboot();
        }
        if (factory && err != ESP_OK) {
            cJSON_Delete(body);
            return error(req, err == ESP_ERR_INVALID_STATE ? "设备状态已变更或正在写入，请重新读取后重试" :
                err == ESP_ERR_NOT_SUPPORTED ? "当前 Flash 布局不支持板载彻底恢复，请使用 USB 刷机工具" :
                "恢复请求未确认，请核对设备是否仍在线", err == ESP_ERR_INVALID_STATE ? 409 : 503);
        }
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return error(req, err == ESP_ERR_INVALID_ARG ? "设置无效：启用时需至少选一天，并填写有效的时分、开关和版本" :
            err == ESP_ERR_INVALID_STATE ? "状态已变更或设备忙；回收资源需要连续空闲60秒，请重新读取后重试" : "保存或执行失败，未确认成功，请重试",
            err == ESP_ERR_INVALID_ARG ? 400 : err == ESP_ERR_INVALID_STATE ? 409 : 503);
    }
    return reply(req, maintenance_snapshot(), 200);
}
esp_err_t broker_http(httpd_req_t *req)
{
    if (!identity_matches(req)) { return ESP_FAIL; }
    cJSON *body = app_read_json(req);
    if (!body) { return ESP_FAIL; }
    const cJSON *address = cJSON_GetObjectItemCaseSensitive(body, "address");
    const esp_err_t err = cJSON_IsString(address) ? netcom_set_broker(address->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return err == ESP_ERR_INVALID_ARG ? error(req, "请输入有效的域名或 IP，端口范围为 1–65535", 400) :
            error(req, "中继服务器未保存，请重试", 503);
    }
    char configured[NET_ENDPOINT_SIZE];
    netcom_broker_address(configured, sizeof(configured));
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddBoolToObject(out, "scheduled", netcom_broker_pending());
    cJSON_AddStringToObject(out, "address", configured);
    return reply(req, out, 200);
}
esp_err_t peer_labels_http(httpd_req_t *req)
{
    if (!identity_matches(req)) { return ESP_FAIL; }
    cJSON *body = app_read_json(req); if (!body) { return ESP_FAIL; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(body, "name"); uint32_t revision;
    esp_err_t err = ESP_ERR_INVALID_ARG; bool added = false;
    if (cJSON_IsString(op) && cJSON_IsString(name) && device_name_validate(name->valuestring) && uint32_field(body, "revision", &revision)) {
        if (!strcmp(op->valuestring, "rename")) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "id");
            if (cJSON_IsString(id)) { err = netp2pnet_peer_rename(id->valuestring, name->valuestring, revision); }
        } else if (!strcmp(op->valuestring, "add")) {
            const cJSON *address = cJSON_GetObjectItemCaseSensitive(body, "address");
            const cJSON *restore = cJSON_GetObjectItemCaseSensitive(body, "restore");
            net_endpoint_t endpoint;
            if (cJSON_IsString(address) && cJSON_IsBool(restore) &&
                net_endpoint_parse(address->valuestring, 54321, &endpoint)) {
                err = netp2pnet_peer_add_named(name->valuestring, endpoint.host, endpoint.port,
                                              cJSON_IsTrue(restore), revision, &added);
            }
        }
    }
    cJSON_Delete(body);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK); cJSON_AddBoolToObject(out, "peer_added", added);
    cJSON_AddNumberToObject(out, "label_revision", peer_labels_revision());
    if (err != ESP_OK) {
        cJSON_AddStringToObject(out, "err", added ? "对端已添加，但名称未确认保存；请保留草稿并在列表中重试改名" :
            err == ESP_ERR_INVALID_ARG ? "名称最多24个字符/96字节；请输入有效域名或 IP，端口范围为 1–65535" :
            err == ESP_ERR_NOT_FOUND ? "对端不存在或名额已满，请刷新列表" :
            err == ESP_ERR_INVALID_STATE ? "对端、名称版本或拒绝规则已变化，请重新读取；草稿已保留" : "名称保存失败，原有名称保留，请重试");
    }
    return reply(req, out, err == ESP_OK ? 200 : err == ESP_ERR_INVALID_ARG ? 400 :
        err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_FOUND ? 409 : 503);
}

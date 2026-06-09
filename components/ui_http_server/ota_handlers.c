#include "ota_handlers.h"

#if CONFIG_SNAPCLIENT_WEB_OTA

#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "OTA_HTTP";

extern void sc_stop_snapclient(void);

#define OTA_HTTP_CHUNK_SIZE 4096

static void set_cors_headers(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
	                   "GET, POST, DELETE, OPTIONS");
	httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
	httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

static esp_err_t options_handler(httpd_req_t *req) {
	set_cors_headers(req);
	httpd_resp_set_status(req, "204 No Content");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

static void restart_task(void *pv) {
	vTaskDelay(pdMS_TO_TICKS(200));
	esp_restart();
	vTaskDelete(NULL);
}

/*
 * POST /api/ota/upload
 * Receives a raw firmware binary, writes it to the inactive OTA partition,
 * sets it as the next boot partition, and restarts.
 */
static esp_err_t ota_upload_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s content_len=%d", __func__, req->uri, req->content_len);

	set_cors_headers(req);
	httpd_resp_set_type(req, "application/json");

	if (req->content_len == 0) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"No firmware data\"}");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "%s: OTA upload started, %d bytes", __func__, req->content_len);

	sc_stop_snapclient();
	vTaskDelay(pdMS_TO_TICKS(500));

	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
	if (!update_partition) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"No OTA partition available\"}");
		return ESP_OK;
	}

	esp_ota_handle_t ota_handle;
	esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_begin failed: %s", __func__, esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"OTA begin failed\"}");
		return ESP_OK;
	}

	char *buf = (char *)malloc(OTA_HTTP_CHUNK_SIZE);
	if (!buf) {
		esp_ota_abort(ota_handle);
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
		return ESP_OK;
	}

	int remaining = req->content_len;
	int received = 0;
	int last_pct10 = -1;

	while (remaining > 0) {
		int to_read = (remaining < OTA_HTTP_CHUNK_SIZE) ? remaining : OTA_HTTP_CHUNK_SIZE;
		int ret = httpd_req_recv(req, buf, to_read);

		if (ret <= 0) {
			free(buf);
			esp_ota_abort(ota_handle);
			if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
				httpd_resp_set_status(req, "408 Request Timeout");
				httpd_resp_sendstr(req, "{\"error\": \"Upload timed out\"}");
			} else {
				httpd_resp_set_status(req, "500 Internal Server Error");
				httpd_resp_sendstr(req, "{\"error\": \"Receive failed\"}");
			}
			return ESP_OK;
		}

		err = esp_ota_write(ota_handle, buf, ret);
		if (err != ESP_OK) {
			free(buf);
			esp_ota_abort(ota_handle);
			ESP_LOGE(TAG, "%s: esp_ota_write failed: %s", __func__, esp_err_to_name(err));
			httpd_resp_set_status(req, "500 Internal Server Error");
			httpd_resp_sendstr(req, "{\"error\": \"OTA write failed\"}");
			return ESP_OK;
		}

		received += ret;
		remaining -= ret;

		int pct10 = received * 10 / req->content_len;
		if (pct10 != last_pct10) {
			last_pct10 = pct10;
			ESP_LOGI(TAG, "%s: OTA progress: %d%%", __func__, pct10 * 10);
		}
	}

	free(buf);

	err = esp_ota_end(ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_end failed: %s", __func__, esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"OTA validation failed — invalid firmware?\"}");
		return ESP_OK;
	}

	err = esp_ota_set_boot_partition(update_partition);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_set_boot_partition failed: %s", __func__, esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Failed to set boot partition\"}");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "%s: OTA complete (%d bytes), restarting...", __func__, received);

	httpd_resp_set_status(req, "200 OK");
	httpd_resp_set_hdr(req, "Connection", "close");
	httpd_resp_sendstr(req, "{\"success\": true}");

	xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);

	return ESP_OK;
}

/*
 * GET /api/ota/status
 * Returns running firmware description so the frontend can confirm a
 * successful OTA by comparing SHA256 before and after the upload.
 */
static esp_err_t get_ota_status_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

	set_cors_headers(req);
	httpd_resp_set_type(req, "application/json");

	const esp_app_desc_t *desc = esp_app_get_description();

	char sha256[65] = {0};
	for (int i = 0; i < 32; i++) {
		snprintf(sha256 + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
	}

	char json[600];
	int n = snprintf(json, sizeof(json),
		"{\"version\":\"%s\",\"project_name\":\"%s\","
		"\"compile_date\":\"%s\",\"compile_time\":\"%s\","
		"\"idf_version\":\"%s\",\"sha256\":\"%s\"",
		desc->version, desc->project_name,
		desc->date, desc->time,
		desc->idf_ver, sha256);
#if CONFIG_SNAPCLIENT_WEB_OTA_PULL
	n += snprintf(json + n, sizeof(json) - n,
		",\"pull_enabled\":true,\"pull_url\":\"%s\"",
		CONFIG_SNAPCLIENT_OTA_PULL_URL);
#else
	n += snprintf(json + n, sizeof(json) - n, ",\"pull_enabled\":false");
#endif
	snprintf(json + n, sizeof(json) - n, "}");

	httpd_resp_sendstr(req, json);
	return ESP_OK;
}

#if CONFIG_SNAPCLIENT_WEB_OTA_PULL
#include "esp_crt_bundle.h"
#define OTA_PULL_BODY_MAX     600
#define OTA_PULL_URL_MAX      512
#define OTA_MANIFEST_BODY_MAX 4096
#define OTA_APP_DESC_MAGIC    0xABCD5432u
#define OTA_APP_DESC_PROJ_OFF 48

/* Fetches a URL into a heap buffer. Caller must free *out_buf on ESP_OK. */
static esp_err_t fetch_url_to_buf(const char *url, char **out_buf, int *out_len) {
	esp_http_client_config_t cfg = {
		.url                = url,
		.timeout_ms         = 15000,
		.buffer_size        = 512,
		.crt_bundle_attach  = esp_crt_bundle_attach,
	};
	esp_http_client_handle_t client = esp_http_client_init(&cfg);
	if (!client) return ESP_ERR_NO_MEM;

	esp_err_t err = esp_http_client_open(client, 0);
	if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }

	esp_http_client_fetch_headers(client);
	if (esp_http_client_get_status_code(client) != 200) {
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return ESP_FAIL;
	}

	char *buf = malloc(OTA_MANIFEST_BODY_MAX + 1);
	if (!buf) {
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return ESP_ERR_NO_MEM;
	}
	int total = 0;
	while (total < OTA_MANIFEST_BODY_MAX) {
		int rd = esp_http_client_read(client, buf + total, OTA_MANIFEST_BODY_MAX - total);
		if (rd <= 0) break;
		total += rd;
	}
	buf[total] = '\0';
	esp_http_client_close(client);
	esp_http_client_cleanup(client);
	*out_buf = buf;
	*out_len = total;
	return ESP_OK;
}

static esp_err_t validate_ota_first_chunk(const char *buf, int len, bool force) {
	for (int i = 0; i + 256 <= len; i += 4) {
		uint32_t word;
		memcpy(&word, buf + i, 4);
		if (word != OTA_APP_DESC_MAGIC) continue;

		const char *new_proj = buf + i + OTA_APP_DESC_PROJ_OFF;
		const esp_app_desc_t *running = esp_app_get_description();

		if (force) {
			ESP_LOGW(TAG, "OTA pull: force=true, skipping project_name check (image='%.32s')", new_proj);
			return ESP_OK;
		}
		if (strncmp(new_proj, running->project_name, 32) != 0) {
			ESP_LOGE(TAG, "OTA pull: project_name mismatch: image='%.32s' expected='%s'",
			         new_proj, running->project_name);
			return ESP_ERR_INVALID_ARG;
		}
		ESP_LOGI(TAG, "OTA pull: image validated (project='%s')", running->project_name);
		return ESP_OK;
	}
	ESP_LOGE(TAG, "OTA pull: esp_app_desc magic not found in first chunk");
	return ESP_ERR_NOT_FOUND;
}

/*
 * POST /api/ota/manifest
 * Body: {"url":"<manifest_url>"}
 * Fetches and parses the manifest JSON, returns it alongside the running
 * firmware version so the browser can show a diff before flashing.
 */
static esp_err_t ota_manifest_check_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
	set_cors_headers(req);
	httpd_resp_set_type(req, "application/json");

	int body_len = req->content_len;
	if (body_len <= 0 || body_len > OTA_PULL_BODY_MAX) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Missing or oversized request body\"}");
		return ESP_OK;
	}
	char *req_body = malloc(body_len + 1);
	if (!req_body) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
		return ESP_OK;
	}
	int recvd = httpd_req_recv(req, req_body, body_len);
	req_body[recvd > 0 ? recvd : 0] = '\0';

	cJSON *root = cJSON_ParseWithLength(req_body, recvd > 0 ? recvd : 0);
	free(req_body);
	if (!root) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON body\"}");
		return ESP_OK;
	}
	cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
	if (!cJSON_IsString(url_item) || !url_item->valuestring || !url_item->valuestring[0]) {
		cJSON_Delete(root);
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid \\\"url\\\" field\"}");
		return ESP_OK;
	}
	char murl[OTA_PULL_URL_MAX + 1];
	strlcpy(murl, url_item->valuestring, sizeof(murl));
	cJSON_Delete(root);

	char *manifest_body = NULL; int manifest_body_len = 0;
	if (fetch_url_to_buf(murl, &manifest_body, &manifest_body_len) != ESP_OK) {
		httpd_resp_set_status(req, "502 Bad Gateway");
		httpd_resp_sendstr(req, "{\"error\": \"Failed to fetch manifest\"}");
		return ESP_OK;
	}
	cJSON *manifest = cJSON_ParseWithLength(manifest_body, manifest_body_len);
	free(manifest_body);
	if (!manifest) {
		httpd_resp_set_status(req, "502 Bad Gateway");
		httpd_resp_sendstr(req, "{\"error\": \"Manifest is not valid JSON\"}");
		return ESP_OK;
	}

	const esp_app_desc_t *running = esp_app_get_description();
	cJSON *ver = cJSON_GetObjectItemCaseSensitive(manifest, "version");
	bool update_available = !cJSON_IsString(ver)
	    || strcmp(ver->valuestring, running->version) != 0;

	cJSON *resp = cJSON_CreateObject();
	cJSON_AddItemToObject(resp, "manifest", manifest);
	cJSON_AddStringToObject(resp, "current_version", running->version);
	cJSON_AddBoolToObject(resp, "update_available", update_available);

	char *out = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (!out) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Serialization failed\"}");
		return ESP_OK;
	}
	httpd_resp_sendstr(req, out);
	free(out);
	return ESP_OK;
}

/*
 * POST /api/ota/pull
 * Body: {"url":"<manifest_url>", "force":false}
 *
 * 1. Fetches and parses the manifest JSON.
 * 2. If the manifest version matches the running version (and force=false),
 *    returns {"up_to_date":true} without flashing.
 * 3. Downloads the binary from manifest["url"], validates esp_app_desc_t,
 *    streams to the inactive OTA partition, then calls esp_ota_end() which
 *    verifies the SHA-256 appended to every ESP-IDF image.
 */
static esp_err_t ota_pull_handler(httpd_req_t *req) {
	ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
	set_cors_headers(req);
	httpd_resp_set_type(req, "application/json");

	int body_len = req->content_len;
	if (body_len <= 0 || body_len > OTA_PULL_BODY_MAX) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Missing or oversized request body\"}");
		return ESP_OK;
	}
	char *req_body = malloc(body_len + 1);
	if (!req_body) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
		return ESP_OK;
	}
	int recvd = httpd_req_recv(req, req_body, body_len);
	req_body[recvd > 0 ? recvd : 0] = '\0';

	cJSON *root = cJSON_ParseWithLength(req_body, recvd > 0 ? recvd : 0);
	free(req_body);
	if (!root) {
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON body\"}");
		return ESP_OK;
	}
	cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
	if (!cJSON_IsString(url_item) || !url_item->valuestring || !url_item->valuestring[0]) {
		cJSON_Delete(root);
		httpd_resp_set_status(req, "400 Bad Request");
		httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid \\\"url\\\" field\"}");
		return ESP_OK;
	}
	char manifest_url[OTA_PULL_URL_MAX + 1];
	strlcpy(manifest_url, url_item->valuestring, sizeof(manifest_url));
	bool force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "force"));
	cJSON_Delete(root);

	ESP_LOGI(TAG, "%s: fetching manifest: %s", __func__, manifest_url);
	char *manifest_body = NULL; int manifest_len = 0;
	if (fetch_url_to_buf(manifest_url, &manifest_body, &manifest_len) != ESP_OK) {
		httpd_resp_set_status(req, "502 Bad Gateway");
		httpd_resp_sendstr(req, "{\"error\": \"Failed to fetch manifest\"}");
		return ESP_OK;
	}
	cJSON *manifest = cJSON_ParseWithLength(manifest_body, manifest_len);
	free(manifest_body);
	if (!manifest) {
		httpd_resp_set_status(req, "502 Bad Gateway");
		httpd_resp_sendstr(req, "{\"error\": \"Manifest is not valid JSON\"}");
		return ESP_OK;
	}

	cJSON *bin_url_item = cJSON_GetObjectItemCaseSensitive(manifest, "url");
	cJSON *ver_item     = cJSON_GetObjectItemCaseSensitive(manifest, "version");
	cJSON *sha256_item  = cJSON_GetObjectItemCaseSensitive(manifest, "sha256");
	cJSON *notes_item   = cJSON_GetObjectItemCaseSensitive(manifest, "release_notes");

	if (!cJSON_IsString(bin_url_item) || !bin_url_item->valuestring[0]) {
		cJSON_Delete(manifest);
		httpd_resp_set_status(req, "422 Unprocessable Entity");
		httpd_resp_sendstr(req, "{\"error\": \"Manifest missing required \\\"url\\\" field\"}");
		return ESP_OK;
	}

	char binary_url[OTA_PULL_URL_MAX + 1];
	strlcpy(binary_url, bin_url_item->valuestring, sizeof(binary_url));

	if (cJSON_IsString(ver_item))
		ESP_LOGI(TAG, "%s: manifest version=%s", __func__, ver_item->valuestring);
	if (cJSON_IsString(sha256_item) && sha256_item->valuestring)
		ESP_LOGI(TAG, "%s: manifest sha256=%s", __func__, sha256_item->valuestring);
	if (cJSON_IsString(notes_item))
		ESP_LOGI(TAG, "%s: release_notes=%s", __func__, notes_item->valuestring);

	const esp_app_desc_t *running = esp_app_get_description();
	if (!force && cJSON_IsString(ver_item)
	    && strcmp(ver_item->valuestring, running->version) == 0) {
		ESP_LOGI(TAG, "%s: already running version %s", __func__, running->version);
		cJSON_Delete(manifest);
		char msg[96];
		snprintf(msg, sizeof(msg), "{\"up_to_date\":true,\"version\":\"%s\"}", running->version);
		httpd_resp_sendstr(req, msg);
		return ESP_OK;
	}
	cJSON_Delete(manifest);

	ESP_LOGI(TAG, "%s: downloading binary: %s", __func__, binary_url);
	esp_http_client_config_t http_cfg = {
		.url               = binary_url,
		.timeout_ms        = 30000,
		.buffer_size       = OTA_HTTP_CHUNK_SIZE,
		.keep_alive_enable = false,
		.crt_bundle_attach = esp_crt_bundle_attach,
	};
	esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
	if (!client) {
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"HTTP client init failed\"}");
		return ESP_OK;
	}
	esp_err_t err = esp_http_client_open(client, 0);
	if (err != ESP_OK) {
		esp_http_client_cleanup(client);
		httpd_resp_set_status(req, "502 Bad Gateway");
		httpd_resp_sendstr(req, "{\"error\": \"Failed to connect to binary URL\"}");
		return ESP_OK;
	}
	int64_t content_length = esp_http_client_fetch_headers(client);
	int http_status = esp_http_client_get_status_code(client);
	if (http_status != 200) {
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		httpd_resp_set_status(req, "502 Bad Gateway");
		httpd_resp_sendstr(req, "{\"error\": \"Binary URL returned non-200 status\"}");
		return ESP_OK;
	}
	ESP_LOGI(TAG, "%s: binary size: %" PRId64 " bytes", __func__, content_length);

	sc_stop_snapclient();
	vTaskDelay(pdMS_TO_TICKS(500));

	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
	if (!update_partition) {
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"No OTA partition available\"}");
		return ESP_OK;
	}
	esp_ota_handle_t ota_handle;
	err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
	if (err != ESP_OK) {
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"OTA begin failed\"}");
		return ESP_OK;
	}
	char *buf = malloc(OTA_HTTP_CHUNK_SIZE);
	if (!buf) {
		esp_ota_abort(ota_handle);
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed\"}");
		return ESP_OK;
	}

	bool validated = false;
	int total = 0;
	const char *abort_msg = NULL;

	while (true) {
		int rd = esp_http_client_read(client, buf, OTA_HTTP_CHUNK_SIZE);
		if (rd < 0) { abort_msg = "Download read error"; break; }
		if (rd == 0) break;

		if (!validated) {
			err = validate_ota_first_chunk(buf, rd, force);
			if (err == ESP_ERR_INVALID_ARG) { abort_msg = "Firmware project_name mismatch — wrong board?"; break; }
			if (err == ESP_ERR_NOT_FOUND)   { abort_msg = "Not a valid ESP-IDF firmware image"; break; }
			validated = true;
		}
		err = esp_ota_write(ota_handle, buf, rd);
		if (err != ESP_OK) { abort_msg = "OTA write failed"; break; }
		total += rd;
	}

	free(buf);
	esp_http_client_close(client);
	esp_http_client_cleanup(client);

	if (abort_msg || total == 0) {
		esp_ota_abort(ota_handle);
		ESP_LOGE(TAG, "%s: aborting OTA: %s", __func__, abort_msg ? abort_msg : "zero bytes downloaded");
		httpd_resp_set_status(req, "422 Unprocessable Entity");
		char msg[128];
		snprintf(msg, sizeof(msg), "{\"error\": \"%s\"}", abort_msg ? abort_msg : "Downloaded 0 bytes");
		httpd_resp_sendstr(req, msg);
		return ESP_OK;
	}

	err = esp_ota_end(ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_end failed: %s", __func__, esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"OTA integrity check failed — corrupt image?\"}");
		return ESP_OK;
	}
	err = esp_ota_set_boot_partition(update_partition);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "%s: esp_ota_set_boot_partition failed: %s", __func__, esp_err_to_name(err));
		httpd_resp_set_status(req, "500 Internal Server Error");
		httpd_resp_sendstr(req, "{\"error\": \"Failed to set boot partition\"}");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "%s: OTA pull complete (%d bytes), restarting...", __func__, total);
	httpd_resp_set_status(req, "200 OK");
	httpd_resp_set_hdr(req, "Connection", "close");
	httpd_resp_sendstr(req, "{\"success\": true}");
	xTaskCreate(restart_task, "ota_pull_restart", 2048, NULL, 5, NULL);
	return ESP_OK;
}
#endif /* CONFIG_SNAPCLIENT_WEB_OTA_PULL */

void ota_register_handlers(httpd_handle_t server) {
	httpd_uri_t upload = {
		.uri     = "/api/ota/upload",
		.method  = HTTP_POST,
		.handler = ota_upload_handler,
	};
	httpd_register_uri_handler(server, &upload);

	httpd_uri_t upload_options = {
		.uri     = "/api/ota/upload",
		.method  = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &upload_options);

	httpd_uri_t status = {
		.uri     = "/api/ota/status",
		.method  = HTTP_GET,
		.handler = get_ota_status_handler,
	};
	httpd_register_uri_handler(server, &status);

#if CONFIG_SNAPCLIENT_WEB_OTA_PULL
	httpd_uri_t manifest = {
		.uri     = "/api/ota/manifest",
		.method  = HTTP_POST,
		.handler = ota_manifest_check_handler,
	};
	httpd_register_uri_handler(server, &manifest);

	httpd_uri_t manifest_options = {
		.uri     = "/api/ota/manifest",
		.method  = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &manifest_options);

	httpd_uri_t pull = {
		.uri     = "/api/ota/pull",
		.method  = HTTP_POST,
		.handler = ota_pull_handler,
	};
	httpd_register_uri_handler(server, &pull);

	httpd_uri_t pull_options = {
		.uri     = "/api/ota/pull",
		.method  = HTTP_OPTIONS,
		.handler = options_handler,
	};
	httpd_register_uri_handler(server, &pull_options);
#endif /* CONFIG_SNAPCLIENT_WEB_OTA_PULL */
}

#endif /* CONFIG_SNAPCLIENT_WEB_OTA */

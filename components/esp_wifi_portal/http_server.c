#include "http_server.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_wifi.h>

extern const char root_start[] asm("_binary_root_html_start");
extern const char root_end[] asm("_binary_root_html_end");

static const char* TAG = "esp_wifi_portal";

static httpd_handle_t server = NULL;
static EventGroupHandle_t wifi_event_group = NULL;

static bool is_webserver_started = false;

// HTTP GET Handler
static esp_err_t root_get_handler(httpd_req_t* req)
{
    const int root_len = root_end - root_start;

    ESP_LOGI(TAG, "Serve root");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, root_start, root_len);

    return ESP_OK;
}

/* 扫描并返回 JSON */
static esp_err_t wifi_scan_get_handler(httpd_req_t* req)
{
    uint16_t ap_count = 0;
#ifdef USE_CHANNEL_BIT_MAP
    wifi_scan_config_t* scan_config = (wifi_scan_config_t*)calloc(1, sizeof(wifi_scan_config_t));
    if (!scan_config)
    {
        ESP_LOGE(TAG, "Memory Allocation for scan config failed!");
        return ESP_FAIL;
    }
    array_2_channel_bitmap(channel_list, CHANNEL_LIST_SIZE, scan_config);
    esp_wifi_scan_start(scan_config, true);
    free(scan_config);
#else
    esp_wifi_scan_start(NULL, true);
#endif /*USE_CHANNEL_BIT_MAP*/
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get AP count");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (ap_count == 0)
    {
        // 没有扫描到AP，返回空数组
        const char* empty_json = "[]";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, empty_json);
        return ESP_OK;
    }


    wifi_ap_record_t* ap_records = calloc(CONFIG_ESP_WIFI_PORTAL_MAX_SCAN_CONN, sizeof(wifi_ap_record_t));
    if (!ap_records)
    {
        ESP_LOGE(TAG, "Memory allocation for AP records failed!");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 获取AP记录
    uint16_t ap_limit_num = CONFIG_ESP_WIFI_PORTAL_MAX_SCAN_CONN;
    if (esp_wifi_scan_get_ap_records(&ap_limit_num, ap_records) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get AP records");
        free(ap_records);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 记录扫描结果信息
    ESP_LOGI(TAG, "Total APs scanned = %u, actual AP number retrieved = %u", ap_count,
             CONFIG_ESP_WIFI_PORTAL_MAX_SCAN_CONN);

    // 创建JSON数组
    cJSON* root = cJSON_CreateArray();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create JSON root");
        free(ap_records);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 添加SSID到JSON数组
    for (int i = 0; i < CONFIG_ESP_WIFI_PORTAL_MAX_SCAN_CONN; i++)
    {
        ESP_LOGI(TAG, "SSID \t\t%s", ap_records[i].ssid);
        ESP_LOGI(TAG, "RSSI \t\t%d", ap_records[i].rssi);
        ESP_LOGI(TAG, "Channel \t\t%d", ap_records[i].primary);
        if (ap_records[i].ssid[0] != '\0')
        {
            cJSON_AddItemToArray(root, cJSON_CreateString((const char*)ap_records[i].ssid));
        }
    }

    // 生成JSON字符串
    const char* json_str = cJSON_PrintUnformatted(root);
    if (!json_str)
    {
        ESP_LOGE(TAG, "Failed to print JSON");
        cJSON_Delete(root);
        free(ap_records);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 发送JSON响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    // 清理资源
    cJSON_Delete(root);
    free((void*)json_str);
    free(ap_records);

    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t* req)
{
    char buf[256];
    const int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;
    const char* ssid = cJSON_GetObjectItem(root, "ssid")->valuestring;
    const char* password = cJSON_GetObjectItem(root, "password")->valuestring;

    ESP_LOGI(TAG, "SSID: %s, Password: %s", ssid, password);

    // notice: ssid 最长 32 字节，前端要拦截
    wifi_config_t wifi_sta_config = {0};

    strcpy((char*)wifi_sta_config.sta.ssid, (const char*)ssid);
    strcpy((char*)wifi_sta_config.sta.password, (const char*)password);

    ESP_LOGI(TAG, "SSID: %s, Password: %s", wifi_sta_config.sta.ssid, wifi_sta_config.sta.password);

    esp_wifi_disconnect();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

    esp_wifi_connect();

    const char* resp;
    httpd_resp_set_type(req, "application/json");
    const EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdTRUE, pdTRUE,
                                                 pdMS_TO_TICKS(10000));
    if (bits & WIFI_CONNECTED_BIT)
    {
        resp = "{\"success\":true,\"message\":\"\"}";
        ESP_LOGI(TAG, "Connected to the network");
    }
    else
    {
        resp = "{\"success\":false,\"message\":\"Failed to connect to the network\"}";
        ESP_LOGI(TAG, "Failed to connect to the network");
    }

    httpd_resp_send(req, resp, (ssize_t)strlen(resp));
    cJSON_Delete(root);
    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler
};

static const httpd_uri_t scan_uri = {
    .uri = "/scan",
    .method = HTTP_GET,
    .handler = wifi_scan_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t connect_uri = {
    .uri = "/connect",
    .method = HTTP_POST,
    .handler = connect_post_handler,
    .user_ctx = NULL
};

// HTTP Error (404) Handler - Redirects all requests to the root page
esp_err_t http_404_error_handler(httpd_req_t* req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "302 Temporary Redirect");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting to root");
    return ESP_OK;
}

esp_err_t start_webserver(EventGroupHandle_t event_group)
{
    if (is_webserver_started)
    {
        ESP_LOGE(TAG, "Webserver is already started");
        return ESP_FAIL;
    }
    if (event_group == NULL)
    {
        ESP_LOGE(TAG, "Event group is NULL");
        return ESP_FAIL;
    }
    wifi_event_group = event_group;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);

    esp_err_t ret = httpd_start(&server, &config);

    if (ret == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        ret = httpd_register_uri_handler(server, &root);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register root handler, err: %d", ret);
            return ret;
        }
        ret = httpd_register_uri_handler(server, &scan_uri);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register scan handler, err: %d", ret);
            return ret;
        }
        ret = httpd_register_uri_handler(server, &connect_uri);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register connect handler, err: %d", ret);
            return ret;
        }
        ret = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to register 404 handler, err: %d", ret);
            return ret;
        }
    }
    else
    {
        // [patch] 原代码在 httpd_start 失败时也把 is_webserver_started 置 true，
        // 导致之后永远无法重试（配网页彻底打不开且没有提示）。
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    is_webserver_started = true;
    ESP_LOGI(TAG, "Web server started, visit http://192.168.4.1/");
    return ret;
}

esp_err_t stop_webserver(void)
{
    if (!is_webserver_started)
    {
        ESP_LOGE(TAG, "Webserver is not started");
        return ESP_FAIL;
    }
    is_webserver_started = false;
    ESP_ERROR_CHECK(httpd_unregister_uri(server, root.uri));
    ESP_ERROR_CHECK(httpd_unregister_uri(server, scan_uri.uri));
    ESP_ERROR_CHECK(httpd_unregister_uri(server, connect_uri.uri));
    if (server)
    {
        return httpd_stop(server);
    }

    return ESP_FAIL;
}

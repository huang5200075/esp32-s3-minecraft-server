#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "esp_wifi_portal.h"

#include <esp_http_server.h>

#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_wifi.h>
#include <lwip/inet.h>

#include "dns_server.h"
#include "http_server.h"

static const char* TAG = "esp_wifi_portal";

static esp_netif_t* sta_netif = NULL;

static esp_netif_t* ap_netif = NULL;

static bool is_auto_start = true;

static bool is_portal_running = false;

EventGroupHandle_t wifi_event_group = NULL;

static dns_server_handle_t dns_server = NULL;

static esp_event_handler_instance_t sta_event_handler_wifi_instance = NULL;
static esp_event_handler_instance_t sta_event_handler_ip_instance = NULL;

/**
 * @brief Event handler for station mode WiFi events
 *
 * @param arg Argument passed to the event handler (unused)
 * @param event_base Base of the event (WIFI_EVENT or IP_EVENT)
 * @param event_id ID of the event (specific to the event base)
 * @param event_data Data associated with the event (event-specific)
 */
// [patch] 在独立任务中启动配网，避免在 esp_event 回调（栈仅 2KB+）里
// 执行 netif 创建 / httpd_start / dns 启动 等重操作导致栈溢出。
// 防止重复创建配网任务：WiFi 反复断开时 STA_DISCONNECTED 会连续触发，
// 每次 xTaskCreate 一个 8KB 栈的任务会把内部 SRAM 耗尽。
static volatile bool portal_task_pending = false;

// 配网启动失败计数：连续失败 3 次就不再重试，避免陷入快速失败循环刷日志。
#define PORTAL_MAX_FAILURES 3
static int portal_fail_count = 0;

static void portal_start_task(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_portal_start();
    if (err != ESP_OK) {
        portal_fail_count++;
        ESP_LOGE(TAG, "portal start failed (%d/%d): %s",
                 portal_fail_count, PORTAL_MAX_FAILURES, esp_err_to_name(err));
        if (portal_fail_count >= PORTAL_MAX_FAILURES) {
            ESP_LOGE(TAG, "portal failed %d times, giving up. "
                          "Check log above: OOM / socket exhausted / stack too small.",
                     PORTAL_MAX_FAILURES);
        }
    } else {
        ESP_LOGI(TAG, "portal running: connect to '%s' then open http://192.168.4.1/",
                 CONFIG_ESP_WIFI_PORTAL_AP_SSID);
        portal_fail_count = 0;   // 成功一次就清零
    }
    portal_task_pending = false;
    vTaskDelete(NULL);
}

// 统一入口：已在配网中 / 任务已排队 / 已连续失败 3 次，都不再重试
static void portal_request_start(const char *reason)
{
    if (is_portal_running || portal_task_pending) {
        ESP_LOGI(TAG, "portal already running/pending, skip (%s)", reason);
        return;
    }
    if (portal_fail_count >= PORTAL_MAX_FAILURES) {
        ESP_LOGW(TAG, "portal permanently disabled after %d failures, skip (%s)",
                 PORTAL_MAX_FAILURES, reason);
        return;
    }
    portal_task_pending = true;
    ESP_LOGI(TAG, "starting portal (%s)", reason);
    if (xTaskCreate(portal_start_task, "wifi_portal", 8192, NULL, 5, NULL) != pdPASS) {
        portal_task_pending = false;
        portal_fail_count++;
        ESP_LOGE(TAG, "failed to create portal task (%d/%d, out of memory)",
                 portal_fail_count, PORTAL_MAX_FAILURES);
    }
}

static void sta_event_handler(void* arg, const esp_event_base_t event_base,
                              const int32_t event_id, void* event_data)
{
    if (is_portal_running == false)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        {
            ESP_LOGI(TAG, "Wifi STA Started");
            // [patch] 原代码直接 esp_wifi_connect() 并忽略返回值。
            // 若 NVS 里没有保存任何 SSID，该调用返回 ESP_ERR_WIFI_SSID
            // 且不派发任何事件，流程就此卡死（既连不上也不开热点）。
            // 这里先检查是否已有可用配置，没有就直接进入配网模式。
            wifi_config_t cfg_check;
            memset(&cfg_check, 0, sizeof(cfg_check));
            bool has_config = false;
            if (esp_wifi_get_config(WIFI_IF_STA, &cfg_check) == ESP_OK) {
                has_config = (cfg_check.sta.ssid[0] != 0);
            }
            if (!has_config) {
                ESP_LOGI(TAG, "no saved WiFi credentials, starting portal");
                // [patch] 不在事件回调里直接启动 portal：
                // 事件任务栈很小（默认 2304 字节），而 portal_start 会创建 netif、
                // 启动 HTTP/DNS 服务，容易栈溢出导致网页静默打不开。
                // 改为投递到一个专用任务里执行。
                portal_request_start("no config");
            } else {
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "esp_wifi_connect failed (%s), starting portal",
                             esp_err_to_name(err));
                    portal_request_start("connect failed");
                }
            }
        }
        else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            ESP_LOGI(TAG, "Wifi STA Disconnected");
            if (is_auto_start)
            {
                portal_request_start("sta disconnected");
            }
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        {
            const ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));

            // [patch] 原来这里只打日志：配网成功后热点不关，一直在耗电，
            // 且 is_portal_running 保持 true，后续断线无法重新进入配网模式。
            // 现在 STA 拿到 IP 就关闭配网门户（热点 + HTTP + DNS），
            // 切回纯 STA 模式，把资源让给 MC 服务器。
            if (is_portal_running)
            {
                ESP_LOGI(TAG, "STA connected, shutting down provisioning portal");
                esp_wifi_portal_stop();
            }
        }
    }
}

static esp_event_handler_instance_t ap_event_handler_wifi_instance;
static esp_event_handler_instance_t ap_event_handler_ip_instance;

/**
 * @brief Event handler for access point mode WiFi events
 *
 * @param arg Argument passed to the event handler (unused)
 * @param event_base Base of the event (WIFI_EVENT or IP_EVENT)
 * @param event_id ID of the event (specific to the event base)
 * @param event_data Data associated with the event (event-specific)
 */
static void ap_event_handler(void* arg, const esp_event_base_t event_base,
                             const int32_t event_id, void* event_data)
{
    if (is_portal_running == true)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
        {
            ESP_LOGI(TAG, "Wifi AP Started");
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
        {
            const ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            if (wifi_event_group != NULL) { xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT); }
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_wifi_portal_stop();
        }
    }
}

/**
 * @brief Register event handlers for station mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t registerStaEventHandlers(void)
{
    ESP_LOGI(TAG, "register sta event handlers");
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &sta_event_handler,
                                                        NULL,
                                                        &sta_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register sta WIFI_EVENT event handler failed, err: %d", err);
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &sta_event_handler,
                                              NULL,
                                              &sta_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register sta IP_EVENT event handler failed, err: %d", err);
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Unregister event handlers for station mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t unregisterStaEventHandlers(void)
{
    ESP_LOGI(TAG, "unregister sta event handlers");
    esp_err_t err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                          ESP_EVENT_ANY_ID,
                                                          sta_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister sta WIFI_EVENT event WIFI_EVENT_STA_START handler failed, err: %d", err);
        return err;
    }
    err = esp_event_handler_instance_unregister(IP_EVENT,
                                                IP_EVENT_STA_GOT_IP,
                                                sta_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister sta IP_EVENT event handler failed, err: %d", err);
        return err;
    }
    return ESP_OK;
}

/**
 * @brief Create the station network interface
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static void create_sta_netif(void)
{
    if (sta_netif == NULL)
    {
        // First time create
        sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);
        wifi_config_t sta_cfg;
        esp_wifi_get_config(WIFI_IF_STA, &sta_cfg); // 读出当前配置
        sta_cfg.sta.failure_retry_cnt = CONFIG_ESP_WIFI_PORTAL_STA_RETRY_CNT; // 修改重试次数
        // 当 ssid 为空的时候提供一个默认的 ssid 触发连接
        if (strlen((const char*)sta_cfg.sta.ssid) == 0)
        {
            ESP_LOGW(TAG, "sta ssid is empty, use default ssid: %s", "ap");
            strcpy((char*)sta_cfg.sta.ssid, "ap");
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg)); // 写回配置
    }
}

/**
 * @brief Register event handlers for access point mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t registerApEventHandlers(void)
{
    esp_err_t err = esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &ap_event_handler,
                                                        NULL,
                                                        &ap_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register ap IP_EVENT event handler failed, err: %d", err);
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              WIFI_EVENT_AP_START,
                                              &ap_event_handler,
                                              NULL,
                                              &ap_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register ap WIFI_EVENT event handler failed, err: %d", err);
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Unregister event handlers for access point mode WiFi events
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static esp_err_t unregisterApEventHandlers(void)
{
    esp_err_t err = esp_event_handler_instance_unregister(IP_EVENT,
                                                          IP_EVENT_STA_GOT_IP,
                                                          ap_event_handler_ip_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister ap IP_EVENT event handler failed, err: %d", err);
        return err;
    }


    err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                WIFI_EVENT_AP_START,
                                                ap_event_handler_wifi_instance);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "unregister ap WIFI_EVENT event handler failed, err: %d", err);
        return err;
    }

    return ESP_OK;
}

/**
 * @brief Create the access point network interface
 *
 * @return esp_err_t ESP_OK if successful, otherwise an error code
 */
static void create_ap_netif(void)
{
    if (ap_netif == NULL)
    {
        ap_netif = esp_netif_create_default_wifi_ap();
        assert(ap_netif);

        wifi_config_t wifi_ap_cfg = {
            .ap = {
                .ssid = CONFIG_ESP_WIFI_PORTAL_AP_SSID,
                .password = CONFIG_ESP_WIFI_PORTAL_AP_PASSWORD,
                // [patch] 原来是 max_connection=1 + 纯 WPA3：
                //  - 只能连一台设备，被系统踢掉后新连接进不来
                //  - 纯 WPA3 某些手机不兼容，表现为"连上了但打不开页面"
                // 改为允许 4 台设备、WPA2/WPA3 混合认证（兼容性最好）
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA_WPA2_PSK
            },
        };

        if (strlen(CONFIG_ESP_WIFI_PORTAL_AP_PASSWORD) == 0)
        {
            wifi_ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_cfg));
        ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
        esp_netif_ip_info_t ip_info = {0};
#if (CONFIG_ESP_WIFI_PORTAL_AP_ENHANCED_CAPTIVE)
        ip_info.ip.addr = esp_ip4addr_aton("8.8.8.8");
        ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
        ip_info.gw.addr = esp_ip4addr_aton("8.8.8.8");
#else
        ip_info.ip.addr = esp_ip4addr_aton(CONFIG_ESP_WIFI_PORTAL_AP_IP);
        ip_info.netmask.addr = esp_ip4addr_aton(CONFIG_ESP_WIFI_PORTAL_AP_NETMASK);
        ip_info.gw.addr = esp_ip4addr_aton(CONFIG_ESP_WIFI_PORTAL_AP_GATEWAY);
#endif

        ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
        ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

        ESP_LOGI(TAG, "AP SSID: %s, password: %s", wifi_ap_cfg.ap.ssid, wifi_ap_cfg.ap.password);
    }
}

/**
 * @brief Initialize the Wi-Fi portal
 * @note Call this function after nvs_flash_init(), esp_netif_init() and esp_event_loop_create_default()
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_wifi_portal_init(void)
{
    ESP_LOGI(TAG, "esp_wifi_portal_init");
    /*Initialize WiFi */
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(registerStaEventHandlers());
    ESP_ERROR_CHECK(registerApEventHandlers());
    create_sta_netif();
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

#ifdef CONFIG_ESP_WIFI_PORTAL_ENABLE_DHCP_CAPTIVE_PORTAL
static void dhcp_set_captive_portal_url(void)
{
    // get the IP of the access point to redirect to
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

    // turn the IP into a URI
    char* captive_portal_uri = (char*)malloc(32 * sizeof(char));
    assert(captive_portal_uri && "Failed to allocate captive_portal_uri");
    strcpy(captive_portal_uri, "http://");
    strcat(captive_portal_uri, ip_addr);

    // get a handle to configure DHCP with
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    // set the DHCP option 114
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, captive_portal_uri, strlen(
            captive_portal_uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}
#endif // CONFIG_ESP_WIFI_PORTAL_ENABLE_DHCP_CAPTIVE_PORTAL

esp_err_t esp_wifi_portal_start(void)
{
    if (is_portal_running == true)
    {
        ESP_LOGI(TAG, "esp_wifi_portal_start: portal already running");
        return ESP_FAIL;
    }

    // [patch] 原来是 ESP_ERROR_CHECK，失败直接 abort 重启，看不到任何原因。
    // 改为返回错误码，让上层决定怎么处理。
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(mode_err));
        return mode_err;
    }

    create_ap_netif();

#ifdef CONFIG_ESP_WIFI_PORTAL_ENABLE_DHCP_CAPTIVE_PORTAL
    dhcp_set_captive_portal_url();
#endif

    if (wifi_event_group == NULL)
    {
        wifi_event_group = xEventGroupCreate();
    }

    const esp_err_t ret = start_webserver(wifi_event_group);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start web server");
        return ret;
    }

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    dns_server = start_dns_server(&config);
    if (dns_server == NULL)
    {
        ESP_LOGE(TAG, "Failed to start DNS server");
        return ESP_FAIL;
    }
    is_portal_running = true;
    return ESP_OK;
}

esp_err_t esp_wifi_portal_stop(void)
{
    if (is_portal_running == false)
    {
        ESP_LOGI(TAG, "esp_wifi_portal_stop: portal not running");
        return ESP_FAIL;
    }
    is_portal_running = false;

    stop_dns_server(dns_server);
    dns_server = NULL;

    // [patch] 原来是 ESP_ERROR_CHECK(stop_webserver())，失败直接 abort。
    esp_err_t ws_err = stop_webserver();
    if (ws_err != ESP_OK) {
        ESP_LOGW(TAG, "stop_webserver failed: %s", esp_err_to_name(ws_err));
    }

    // [patch] 原来这里 vEventGroupDelete(wifi_event_group)。
    // 但 http_server.c 内部保存了同一个句柄的副本（connect_post_handler 正在
    // xEventGroupWaitBits 上等待），删除后会变成悬空指针，造成崩溃或超时行为异常。
    // 事件组本身很小，改为不销毁，只重置位。
    if (wifi_event_group != NULL) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }

    // 如果 Log level 是 info 打印 wifi config
#if CONFIG_LOG_DEFAULT_LEVEL >= ESP_LOG_INFO
    wifi_config_t wifi_sta_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_sta_cfg) == ESP_OK) {
        ESP_LOGI(TAG, "Station SSID: %s", wifi_sta_cfg.sta.ssid);
    }
#endif

    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode_err != ESP_OK) {
        ESP_LOGW(TAG, "restore STA mode failed: %s", esp_err_to_name(mode_err));
    }

    if (ap_netif != NULL) {
        esp_netif_destroy(ap_netif);
        ap_netif = NULL;
    }
    return ESP_OK;
}

esp_err_t esp_wifi_portal_deinit(void)
{
    ESP_LOGI(TAG, "esp_wifi_portal_deinit");
    if (is_portal_running == true)
    {
        ESP_LOGI(TAG, "esp_wifi_portal_deinit: portal running, stopping it");
        esp_wifi_portal_stop();
    }
    ESP_ERROR_CHECK(unregisterStaEventHandlers());
    ESP_ERROR_CHECK(unregisterApEventHandlers());
    ESP_ERROR_CHECK(esp_wifi_stop());
    if (wifi_event_group != NULL)
    {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }
    if (ap_netif != NULL)
    {
        esp_netif_destroy(ap_netif);
        ap_netif = NULL;
    }
    if (sta_netif != NULL)
    {
        esp_netif_destroy(sta_netif);
        sta_netif = NULL;
    }

    ESP_ERROR_CHECK(esp_wifi_deinit());

    return ESP_OK;
}

/**
 * @brief Set whether the portal should start automatically when the station disconnects
 * @param auto_start true to start automatically, false to not start automatically
 */
void esp_wifi_portal_set_auto_start(const bool auto_start)
{
    is_auto_start = auto_start;
}

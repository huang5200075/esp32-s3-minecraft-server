idf_component_register(SRCS "esp_wifi_portal.c" "dns_server.c" "http_server.c"
        INCLUDE_DIRS "include"
        EMBED_FILES root.html
        PRIV_REQUIRES esp_netif esp_event nvs_flash esp_wifi esp_http_server json)

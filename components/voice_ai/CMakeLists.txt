# voice_ai component — XiaoZhi voice AI integration for Car OS
idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_timer nvs_flash
                 esp_wifi esp_netif esp_websocket_client mqtt json mbedtls
                 esp_xiaozhi mcp-c-sdk esp_http_client
    REQUIRES driver freertos
)

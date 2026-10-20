idf_component_register(
    SRCS "ATEM_tally_light_espidf.cpp" "tally_web_server.cpp" "tally_server_idf.cpp" "improv_idf.cpp" "battery_monitor.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc esp_event esp_http_server esp_netif esp_wifi nvs_flash lwip
)
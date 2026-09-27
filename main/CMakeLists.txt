idf_component_register(SRCS "main.c" "xiaozhi.c" "wake_word.c" "light.c" "mcp.c" "wifi_screen.c"
                    "music.c" "persona.c"
                    INCLUDE_DIRS "."
                    REQUIRES board oled face servo ws2812 sensors robot_audio esp_audio_codec
                    PRIV_REQUIRES esp_wifi esp_http_server esp_http_client esp_netif esp_event
                                   nvs_flash esp_timer driver log freertos json
                                   esp_websocket_client opus mbedtls lwip
                                   espressif__esp-sr)

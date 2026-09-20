idf_component_register(SRCS "csi_presence.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash lwip esp_timer)
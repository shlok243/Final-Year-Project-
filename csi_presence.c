#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>
#include "ping/ping_sock.h"

#define WIFI_SSID      "4th_floor_4g_EXT"
#define WIFI_PASS      "hopesisbest2"

static const char *TAG = "CSI_PRESENCE";
static esp_netif_t *sta_netif = NULL;
static bool csi_started = false;

// Called every time the ESP32 receives a WiFi packet's CSI data
static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        return;
    }

    wifi_csi_info_t d = *info;
    int8_t *csi_data = d.buf;
    int len = d.len;

    double amp_sum = 0;
    int pairs = len / 2;
    for (int i = 0; i < len - 1; i += 2) {
        int8_t im = csi_data[i];
        int8_t re = csi_data[i + 1];
        amp_sum += sqrt((double)(im * im + re * re));
    }
    double amp_avg = pairs > 0 ? amp_sum / pairs : 0;

    printf("CSI,%lld,%d,%.3f\n",
           (long long)esp_timer_get_time(),
           d.rx_ctrl.rssi,
           amp_avg);
}

// Task: continuously send small UDP packets to the router's gateway IP
// to force WiFi traffic, which is what actually triggers CSI captures.
// static void traffic_gen_task(void *pvParameters)
// {
//     esp_netif_ip_info_t ip_info;
//     esp_netif_get_ip_info(sta_netif, &ip_info);

//     struct sockaddr_in dest_addr;
//     dest_addr.sin_family = AF_INET;
//     dest_addr.sin_port = htons(80);          // router admin port, almost always responds
//     dest_addr.sin_addr.s_addr = ip_info.gw.addr;

//     ESP_LOGI(TAG, "TCP SYN traffic gen targeting gateway:80");

//     while (1) {
//         int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
//         if (sock >= 0) {
//             struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms timeout
//             setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
//             connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)); // result ignore karo
//             close(sock);
//         }
//         vTaskDelay(pdMS_TO_TICKS(30));
//     }
// }

static void start_ping_traffic(void)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(sta_netif, &ip_info);

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr.type = IPADDR_TYPE_V4;
    ping_config.target_addr.u_addr.ip4.addr = ip_info.gw.addr;
    ping_config.count = 0;           // infinite
    ping_config.interval_ms = 30;    // ~33 pings/sec
    ping_config.timeout_ms = 200;

    esp_ping_callbacks_t cbs = { 0 };  // no callbacks needed, just want the traffic
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&ping_config, &cbs, &ping) == ESP_OK) {
        esp_ping_start(ping);
        ESP_LOGI(TAG, "Ping-based traffic generator started");
    } else {
        ESP_LOGE(TAG, "Failed to start ping session");
    }
}

static void start_csi_capture(void)
{
    if (csi_started) return;

    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    // xTaskCreate(traffic_gen_task, "traffic_gen", 4096, NULL, 5, NULL);
    start_ping_traffic();

    csi_started = true;
    ESP_LOGI(TAG, "CSI capture + traffic generator started");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, reconnecting...");
        csi_started = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP, starting CSI capture");
        start_csi_capture();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Nothing else to do here — everything happens via events + traffic_gen_task
}
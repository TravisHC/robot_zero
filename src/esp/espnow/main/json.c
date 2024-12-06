#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "cJSON.h"
#include "driver/uart.h"

/* ESPNOW can work in both station and softap mode. It is configured in menuconfig. */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#endif

#define ESPNOW_QUEUE_SIZE           6

#define IS_BROADCAST_ADDR(addr) (memcmp(addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

#define MAX_JSON_SIZE 1024 // 假设最大 JSON 数据长度
#define CHUNK_SIZE 250     // ESP-NOW 数据片段最大长度
#define UART_PORT_NUM UART_NUM_0
#define UART_RXIO 3
#define UART_TXIO 1
#define CONFIG_UART_BAUD_RATE 115200

static const char *TAG = "ESP-NOW";

static xQueueHandle s_example_espnow_queue;

enum {
    EXAMPLE_ESPNOW_DATA_BROADCAST,
    EXAMPLE_ESPNOW_DATA_UNICAST,
    EXAMPLE_ESPNOW_DATA_MAX,
};

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_example_espnow_seq[EXAMPLE_ESPNOW_DATA_MAX] = { 0, 0 };

// 缓存接收的 JSON 数据
static char json_buffer[MAX_JSON_SIZE];
static size_t json_length = 0;



// ESP-NOW 接收回调函数
static void on_data_received(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
    if (mac_addr == NULL || data == NULL || data_len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    // 分片信息解析
    uint8_t is_last_chunk = data[0]; // 第一字节表示是否是最后一个片段
    size_t chunk_size = data_len - 1;

    if (json_length + chunk_size >= MAX_JSON_SIZE) {
        ESP_LOGE(TAG, "Received data exceeds buffer size");
        json_length = 0;
        return;
    }

    // 保存数据片段
    memcpy(json_buffer + json_length, data + 1, chunk_size);
    json_length += chunk_size;

    if (is_last_chunk) {
        json_buffer[json_length] = '\n'; // 添加换行符
        // ESP_LOGI(TAG, "Full JSON received: %s", json_buffer);

        uart_write_bytes(UART_PORT_NUM, json_buffer, json_length);

        // 重置缓冲区
        json_length = 0;
    }
}

// ESP-NOW 发送回调函数
static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    ESP_LOGI(TAG, "Data sent to " MACSTR " status: %s",
             MAC2STR(mac_addr), status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

// 初始化 ESP-NOW
void brodcast_init() {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_received));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));

    // 添加广播设备
    esp_now_peer_info_t peer_info = {};
    memset(&peer_info, 0, sizeof(esp_now_peer_info_t));
    peer_info.channel = 0; // 使用默认频道
    peer_info.encrypt = false;
    memcpy(peer_info.peer_addr, "\xFF\xFF\xFF\xFF\xFF\xFF", 6);

    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
}

// 通过 ESP-NOW 发送 JSON
void esp_now_send_json(const char *json_data) {
    size_t json_len = strlen(json_data);

    for (size_t offset = 0; offset < json_len; offset += CHUNK_SIZE - 1) {
        uint8_t chunk[CHUNK_SIZE];
        size_t chunk_size = (json_len - offset) > (CHUNK_SIZE - 1) ? (CHUNK_SIZE - 1) : (json_len - offset);

        chunk[0] = (offset + chunk_size >= json_len) ? 1 : 0; // 是否是最后一个片段
        memcpy(chunk + 1, json_data + offset, chunk_size);

        ESP_ERROR_CHECK(esp_now_send(s_example_broadcast_mac, chunk, chunk_size + 1));
        // vTaskDelay(pdMS_TO_TICKS(1)); // 短暂延时以确保消息不会被覆盖
    }
}

// UART 初始化
void uart_init() {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, 2 * CHUNK_SIZE, 2 * CHUNK_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    // ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_RXIO, UART_TXIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));


}

uint8_t checkBrace(uint8_t *data, int len) {
    int count = 0;
    for (int i = 0; i < len; i++) {
        if (data[i] == '{') {
            count++;
        } else if (data[i] == '}') {
            count--;
        }
    }
    return count == 0;
}

// 从串口读取 JSON 数据并通过 ESP-NOW 发送
void uart_task(void *param) {
    uint8_t *data = (uint8_t *)malloc(MAX_JSON_SIZE);
    if(data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for recv uart data");
        vTaskDelete(NULL); 
    }
    while (1) {
        int uart_read_wait_ms = 20; // ms
        int len = uart_read_bytes(UART_PORT_NUM, data, MAX_JSON_SIZE - 1, uart_read_wait_ms / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0'; // 添加字符串结束符
            if(checkBrace(data, len) && data[0] == '{') {
                ESP_LOGI(TAG, "UART received JSON [%d bytes]: %s", len, (char *) data);
                // 发送 JSON 数据
                esp_now_send_json((char *)data);
            }else if(strncmp((char *)data, "getMac", 6) == 0){
                uint8_t mac[6];
                // esp_read_mac(mac, ESP_MAC_WIFI_STA);
                esp_efuse_mac_get_default(mac);
                char macStr[19];
                sprintf(macStr, "%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                ESP_LOGI(TAG, "Get self MAC: %s", macStr);
                uart_write_bytes(UART_PORT_NUM, macStr, strlen(macStr));
            }else{
                ESP_LOGI(TAG, "UART received other msg [%d bytes]: %s", len, (char *) data);
                // esp_now_send_json((char *)data);
            }
        }
        // vTaskDelay(pdMS_TO_TICKS(1)); // 避免任务占用过多 CPU
    }
    free(data);
}

/* WiFi should start before using ESPNOW */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

void app_main(void) {
    // esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set(TAG, ESP_LOG_NONE);
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    // 初始化 Wi-Fi
    wifi_init();

    // 初始化 ESP-NOW
    brodcast_init();
    
    // 初始化 UART
    uart_init(); 

    // 创建 UART 任务

    xTaskCreate(uart_task, "uart_task", 4 * 1024,
                NULL, 10, NULL);
}

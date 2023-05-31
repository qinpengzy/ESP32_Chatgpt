#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "nvs_flash.h"

#include <string.h>
#include <esp_http_client.h>
#include <cJSON.h>

#include "esp_system.h"
#include "esp_crt_bundle.h"

extern const uint8_t cacert_pem_start[] asm("_binary_server_root_cert_pem_start");
extern const uint8_t cacert_pem_end[] asm("_binary_server_root_cert_pem_end");

static const char *TAG = "WIFI";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s] password:[%s]", CONFIG_WIFI_SSID, "******");
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

void chat_with_gpt(const char* message) {
    int content_length = 0; 

    esp_http_client_config_t config = {
        .method = HTTP_METHOD_POST,
        .url = "https://api.openai.com/v1/chat/completions",
        .transport_type = HTTP_TRANSPORT_OVER_SSL,  // 使用 TLS 进行传输
        .port =  443,
        .host = "api.openai.com",
        .cert_pem = (const char *)cacert_pem_start,  // 设置服务器证书 PEM 格式字符串
        .cert_len = cacert_pem_end - cacert_pem_start,  // 设置服务器证书 PEM 字符串长度
        .timeout_ms = 5000,      // 增加超时时间
        .buffer_size = 4096,     // 增大接收缓冲区大小
        .buffer_size_tx = 4096   // 增大发送缓冲区大小 
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth_header_value[128];
    snprintf(auth_header_value, sizeof(auth_header_value), "Bearer %s", CONFIG_OPENAI_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_header_value);

    // 构建 JSON 请求数据
    cJSON *root = cJSON_CreateObject();
    cJSON *model = cJSON_CreateString("gpt-3.5-turbo");
    cJSON *role = cJSON_CreateString("user");
    cJSON *content = cJSON_CreateString(message);
    cJSON *messages = cJSON_CreateArray();
    cJSON *messageObj = cJSON_CreateObject();
    cJSON_AddItemToObject(messageObj, "role", role);
    cJSON_AddItemToObject(messageObj, "content", content);
    cJSON_AddItemToArray(messages, messageObj);
    cJSON_AddItemToObject(root, "model", model);
    cJSON_AddItemToObject(root, "messages", messages);
    char *request_data = cJSON_PrintUnformatted(root);

    // 发送 HTTP 请求
    esp_err_t err = esp_http_client_open(client, strlen(request_data));
    if (err != ESP_OK) { 
         ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err)); 
    } else { 
        int wlen = esp_http_client_write(client, request_data, strlen(request_data)); 
        if (wlen < 0) { 
            ESP_LOGE(TAG, "Write failed"); 
        } 
        content_length = esp_http_client_fetch_headers(client); 
        if (content_length < 0) { 
            ESP_LOGE(TAG, "HTTP client fetch headers failed"); 
        } else { 
            char *output_buffer = (char *)malloc((content_length + 1) * sizeof(char));
            int data_read = esp_http_client_read_response(client, output_buffer, content_length); 
            if (data_read >= 0) { 
                ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d", 
                esp_http_client_get_status_code(client), 
                esp_http_client_get_content_length(client)); 
                output_buffer[data_read] = '\0';
                ESP_LOGI(TAG, "Response: %s", output_buffer); 

                // 解析 JSON 响应
                cJSON *root = cJSON_Parse(output_buffer);
                cJSON *choices = cJSON_GetObjectItem(root, "choices");
                cJSON *messageObj = cJSON_GetArrayItem(choices, 0);
                cJSON *message = cJSON_GetObjectItem(messageObj, "message");
                cJSON *content = cJSON_GetObjectItem(message, "content");
                const char *assistantResponse = cJSON_GetStringValue(content);
                ESP_LOGI(TAG, "Chatgpt: %s", assistantResponse);

                // 释放 cJSON 对象
                cJSON_Delete(root);
            } else { 
                ESP_LOGE(TAG, "Failed to read response"); 
            } 
        } 
    } 
    esp_http_client_cleanup(client);
    cJSON_free(request_data);
}

void app_main(void)
{
    wifi_init();
    chat_with_gpt("Hello World!");
    while(1){
        vTaskDelay(100);
    }
}
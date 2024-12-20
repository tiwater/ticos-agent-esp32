#include "ticos_agent.h"
#include "ticos_audio.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "TicosAgent";
static esp_websocket_client_handle_t client = NULL;
static QueueHandle_t parse_message_queue = NULL;
static TaskHandle_t message_task_handle = NULL;
static ticos_message_handler message_handler_cb = NULL;

#define MESSAGE_QUEUE_SIZE 100

static const char *server_host = CONFIG_TICOS_SERVER;

void process_message_str(const char *str) {
    // TODO: For evaluation we use base64 encoded audio data. We expect raw data in real situation to ease the embedded device.
    cJSON *json = cJSON_Parse(str);
    if(json){
        cJSON *event = cJSON_GetObjectItem(json, "event");
        if (event) {
          cJSON *delta = cJSON_GetObjectItem(event, "delta");
          if (delta) {
            cJSON *audio = cJSON_GetObjectItem(delta, "audio");
            if (cJSON_IsString(audio)) {
                // Audio data, parse and transfer to audio driver
                const char *base64_audio = audio->valuestring;
                size_t decoded_len;
                mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)base64_audio, strlen(base64_audio));
                
                unsigned char *audio_data = (unsigned char *)malloc(decoded_len);
                if (!audio_data) {
                    ESP_LOGE(TAG, "Memory allocation failed");
                    cJSON_Delete(json);
                    return;
                }

                int ret = mbedtls_base64_decode(audio_data, decoded_len, &decoded_len, (const unsigned char *)base64_audio, strlen(base64_audio));
                if (ret != 0) {
                    ESP_LOGE(TAG, "Base64 decoding failed");
                    free(audio_data);
                    cJSON_Delete(json);
                    return;
                }

                play_audio(audio_data, decoded_len);
                cJSON_Delete(json);
                return;
            }
          }
        }
    }
    cJSON_Delete(json);

    // Normal message, pass to client to handle
    if(message_handler_cb){
      message_handler_cb(str);
    }
}

bool rtc_send_init_message(esp_websocket_client_handle_t client) {
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "init");
    cJSON_AddStringToObject(message, "agent_id", "2");

    char *message_str = cJSON_PrintUnformatted(message);
    if (!message_str) {
        cJSON_Delete(message);
        return false;
    }

    bool success = esp_websocket_client_send_text(client, message_str, strlen(message_str), portMAX_DELAY);
    cJSON_free(message_str);
    cJSON_Delete(message);
    return success;
}

bool rtc_send_config_update(esp_websocket_client_handle_t client) {
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "command");
    cJSON *command = cJSON_AddObjectToObject(message, "command");
    cJSON_AddStringToObject(command, "name", "update_config");
    cJSON *params = cJSON_AddObjectToObject(command, "params");
    cJSON_AddNullToObject(params, "turn_detection");
    cJSON_AddStringToObject(params, "voice", "alloy");

    char *message_str = cJSON_PrintUnformatted(message);
    if (!message_str) {
        cJSON_Delete(message);
        return false;
    }

    bool success = esp_websocket_client_send_text(client, message_str, strlen(message_str), portMAX_DELAY);
    if (!success) {
        ESP_LOGE(TAG, "Failed to send config update message");
    }
    ESP_LOGI(TAG, "Sent config update message!");

    cJSON_free(message_str);
    cJSON_Delete(message);

    return success;
}

bool rtc_send_hello_message(esp_websocket_client_handle_t client) {
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "message");
    cJSON *content = cJSON_AddObjectToObject(message, "content");
    cJSON_AddStringToObject(content, "type", "text");
    cJSON_AddStringToObject(content, "text", "Hello");

    char *message_str = cJSON_PrintUnformatted(message);
    if (!message_str) {
        cJSON_Delete(message);
        return false;
    }

    bool success = esp_websocket_client_send_text(client, message_str, strlen(message_str), portMAX_DELAY);
    if (!success) {
        ESP_LOGE(TAG, "Failed to send hello message");
    }
    ESP_LOGI(TAG, "Sent hello message!");

    cJSON_free(message_str);
    cJSON_Delete(message);

    return success;
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    static char *buffer = NULL;
    static size_t buffer_len = 0;
    static int expected_payload_len = 0;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            rtc_send_init_message(client);
            rtc_send_config_update(client);
            rtc_send_hello_message(client);
            ESP_LOGI(TAG, "Realtime client inited");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            if (buffer) {
                free(buffer);
                buffer = NULL;
                buffer_len = 0;
            }
            expected_payload_len = 0;
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x8) {
                ESP_LOGI(TAG, "Closing websocket connection");
                // deinit_ticos_agent();
            } else if (data->op_code == 0x1) {  // Text frame
                if (data->payload_len > 0 && data->payload_offset == 0) {
                    expected_payload_len = data->payload_len;
                    buffer = malloc(expected_payload_len + 1);
                    if (buffer == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory for buffer");
                        return;
                    }
                    buffer_len = 0;
                }

                if (buffer && data->data_len > 0) {
                    memcpy(buffer + buffer_len, data->data_ptr, data->data_len);
                    buffer_len += data->data_len;
                }

                if (buffer && buffer_len == expected_payload_len) {
                    buffer[buffer_len] = '\0';
                    // Show message count in the queue
                    int delay = 0;
                    while(uxQueueMessagesWaiting(parse_message_queue) >= MESSAGE_QUEUE_SIZE && delay < 10){
                        vTaskDelay(pdMS_TO_TICKS(1));
                        delay ++;
                    }
                    if (xQueueSend(parse_message_queue, &buffer, 0) != pdTRUE) {
                        // Strange, if remove the uxQueueMessagesWaiting here will cause send fail sometime
                        ESP_LOGE(TAG, "Failed to send buffer to message queue and queue size: %d", uxQueueMessagesWaiting(parse_message_queue));
                        free(buffer);
                    }
                    buffer = NULL;
                    buffer_len = 0;
                    expected_payload_len = 0;
                }
            }
            break;
        default:
            break;
    }
}

static void process_message_task(void *pvParameters) {
    char *buffer;
    while (true) {
        if (xQueueReceive(parse_message_queue, &buffer, portMAX_DELAY) == pdTRUE) {
            process_message_str(buffer);
            free(buffer);
        }
    }
}

bool init_ticos_agent() {
    esp_websocket_client_config_t websocket_cfg = {
        .uri = server_host,
        .cert_pem = NULL, // Add root CA certificate if needed
        .transport = strncmp(server_host, "wss://", 6) == 0 ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_UNKNOWN,
        .buffer_size = 1024 * 20, // Increase buffer size
        .reconnect_timeout_ms = 10000,
        .task_stack = 1024 * 16,
    };

    client = esp_websocket_client_init(&websocket_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init websocket client");
        return false;
    }

    if (!init_ticos_audio(client)) {
        ESP_LOGE(TAG, "Failed to init audio module");
        esp_websocket_client_destroy(client);
        return false;
    }

    parse_message_queue = xQueueCreate(MESSAGE_QUEUE_SIZE, sizeof(char*));
    if (!parse_message_queue) {
        ESP_LOGE(TAG, "Failed to create message queue");
        deinit_ticos_audio();
        esp_websocket_client_destroy(client);
        return false;
    }

    BaseType_t ret = xTaskCreate(process_message_task, "message_task", 4096, NULL, 5, &message_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create message task");
        vQueueDelete(parse_message_queue);
        deinit_ticos_audio();
        esp_websocket_client_destroy(client);
        return false;
    }

    if (esp_websocket_client_start(client) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start websocket client");
        vTaskDelete(message_task_handle);
        vQueueDelete(parse_message_queue);
        deinit_ticos_audio();
        esp_websocket_client_destroy(client);
        return false;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    return true;
}

bool create_response() {
    if (!client || !esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "WebSocket is not connected");
        return false;
    }

    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "command");
    cJSON *command = cJSON_AddObjectToObject(message, "command");
    cJSON_AddStringToObject(command, "name", "create_response");

    char *message_str = cJSON_PrintUnformatted(message);
    if (!message_str) {
        ESP_LOGE(TAG, "Failed to print JSON message");
        cJSON_Delete(message);
        return false;
    }

    bool success = esp_websocket_client_send_text(client, message_str, strlen(message_str), portMAX_DELAY);
    if (!success) {
        ESP_LOGE(TAG, "Failed to send create response message");
    }
    ESP_LOGI(TAG, "Sent create response message!");

    cJSON_free(message_str);
    cJSON_Delete(message);

    return success;
}


bool send_message(const char *data) {
    if (!client || !esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "Web socket is not connected");
        return false;
    }
    return esp_websocket_client_send_text(client, (char*)data, strlen(data), portMAX_DELAY);
}

bool deinit_ticos_agent() {
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
    }

    deinit_ticos_audio();

    if (message_task_handle) {
        vTaskDelete(message_task_handle);
        message_task_handle = NULL;
    }

    // Free elements in parse_message_queue
    if (parse_message_queue) {
        char *buffer;
        while (xQueueReceive(parse_message_queue, &buffer, 0) == pdTRUE) {
            free(buffer);
        }
        vQueueDelete(parse_message_queue);
        parse_message_queue = NULL;
    }

    return true;
}

bool register_message_handler(ticos_message_handler handler) {
    if (handler == NULL) {
        return false;
    }
    message_handler_cb = handler;
    return true;
}

bool remove_message_handler() {
    message_handler_cb = NULL;
    return true;
}
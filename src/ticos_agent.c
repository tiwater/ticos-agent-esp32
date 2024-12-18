#include "ticos_agent.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cJSON.h"

static const char *TAG = "TicosAgent";
static esp_websocket_client_handle_t client = NULL;
static QueueHandle_t send_audio_queue = NULL;
static QueueHandle_t send_queue = NULL;
static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t send_task_handle = NULL;

static QueueHandle_t parse_message_queue = NULL;
static TaskHandle_t message_task_handle = NULL;

static ticos_message_handler message_handler_cb = NULL;

#define AUDIO_QUEUE_SIZE 20
#define AUDIO_TASK_STACK_SIZE 4096

// static const char *server_host = CONFIG_TICOS_SERVER;
// static const char *server_host = "ws://192.168.31.95:8765/v1/realtime";
static const char *server_host = "ws://192.168.20.188:8765/v1/realtime";

typedef struct {
    uint8_t *data;
    size_t len;
} audio_data_t;

// Encoding task to convert raw data to base64 and enqueuing it for sending
static void encode_audio_task(void *pvParameters) {
    audio_data_t audio_data;
    while (true) {
        if (xQueueReceive(send_audio_queue, &audio_data, portMAX_DELAY)) {
            size_t base64_len;
            mbedtls_base64_encode(NULL, 0, &base64_len, audio_data.data, audio_data.len);
            char *base64_audio = (char *)malloc(base64_len + 1);
            if (base64_audio) {
                int ret = mbedtls_base64_encode((unsigned char *)base64_audio, base64_len + 1, &base64_len, audio_data.data, audio_data.len);
                if (ret == 0) {
                    base64_audio[base64_len] = '\0';
                    if (xQueueSend(send_queue, &base64_audio, portMAX_DELAY) != pdPASS) {
                        ESP_LOGE(TAG, "Failed to queue base64 audio data");
                        free(base64_audio);
                    }
                } else {
                    ESP_LOGE(TAG, "Base64 encoding failed");
                    free(base64_audio);
                }
            } else {
                ESP_LOGE(TAG, "Memory allocation failed for base64_audio");
            }
            free(audio_data.data);
        }
    }
}

// Sending task for sending JSON wrapped base64 audio data over websocket
static void send_audio_task(void *pvParameters) {
    char *base64_audio;
    while (true) {
        if (xQueueReceive(send_queue, &base64_audio, portMAX_DELAY)) {
            const char *json_fmt = "{\"type\":\"message\",\"content\":{\"type\":\"audio\",\"audio\":\"%s\"}}";
            size_t json_msg_len = strlen(json_fmt) + strlen(base64_audio);
            char *json_msg = (char *)malloc(json_msg_len);
            if (json_msg) {
                snprintf(json_msg, json_msg_len, json_fmt, base64_audio);
                if (!esp_websocket_client_send_text(client, json_msg, strlen(json_msg), portMAX_DELAY)) {
                    ESP_LOGE(TAG, "Failed to send audio message");
                }
                free(json_msg);
            } else {
                ESP_LOGE(TAG, "Failed to allocate memory for JSON message");
            }
            free(base64_audio);
        }
    }
}


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
                    // ESP_LOGI(TAG, "Received a complete package");
                    if (xQueueSend(parse_message_queue, &buffer, 0) != pdTRUE) {
                        ESP_LOGE(TAG, "Failed to send buffer to audio queue");
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
    ESP_LOGI(TAG, "Init websocket client against %s", server_host);

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
        ESP_LOGE(TAG, "Failed to initialize websocket client");
        return false;
    }

    if (esp_websocket_client_start(client) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start websocket client");
        esp_websocket_client_destroy(client);
        client = NULL;
        return false;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    
    // Initialize queues and tasks
    send_audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_data_t));
    send_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(char*));
    
    xTaskCreate(encode_audio_task, "encode_audio_task", AUDIO_TASK_STACK_SIZE, NULL, 5, &audio_task_handle);
    xTaskCreate(send_audio_task, "send_audio_task", AUDIO_TASK_STACK_SIZE, NULL, 5, &send_task_handle);


    parse_message_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(char*)); 
    xTaskCreate(process_message_task, "process_message_task", AUDIO_TASK_STACK_SIZE, NULL, 5, &message_task_handle);
    
    return true;
}

bool send_audio(uint8_t *data, size_t len) {
    if (!client || !esp_websocket_client_is_connected(client)) {
        return false;
    }

    audio_data_t audio_data = {
        .data = malloc(len),
        .len = len
    };
    
    if (!audio_data.data) {
        return false;
    }
    
    memcpy(audio_data.data, data, len);
    
    if (xQueueSend(send_audio_queue, &audio_data, 0) != pdPASS) {
        free(audio_data.data);
        return false;
    }
    
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
        
        if (audio_task_handle) {
            vTaskDelete(audio_task_handle);
            audio_task_handle = NULL;
        }

        if (send_task_handle) {
            vTaskDelete(send_task_handle);
            send_task_handle = NULL;
        }

        vQueueDelete(send_audio_queue);
        send_audio_queue = NULL;

        vQueueDelete(send_queue);
        send_queue = NULL;

        if (message_task_handle) {
            vTaskDelete(message_task_handle);
            message_task_handle = NULL;
        }

        vQueueDelete(parse_message_queue);
        parse_message_queue = NULL;
    }
    return true;
}



// Register a callback function to handle messages from server
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
#include "ticos_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TicosAudio";
static esp_websocket_client_handle_t client = NULL;
static QueueHandle_t send_audio_queue = NULL;
static QueueHandle_t send_queue = NULL;
static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t send_task_handle = NULL;

#define AUDIO_QUEUE_SIZE 20
#define AUDIO_TASK_STACK_SIZE 4096

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

bool init_ticos_audio(esp_websocket_client_handle_t ws_client) {
    client = ws_client;
    
    // Create queues
    send_audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_data_t));
    if (!send_audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return false;
    }

    send_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(char*));
    if (!send_queue) {
        ESP_LOGE(TAG, "Failed to create send queue");
        vQueueDelete(send_audio_queue);
        return false;
    }

    // Create tasks
    BaseType_t ret = xTaskCreate(encode_audio_task, "encode_audio", AUDIO_TASK_STACK_SIZE, NULL, 5, &audio_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create encode audio task");
        vQueueDelete(send_audio_queue);
        vQueueDelete(send_queue);
        return false;
    }

    ret = xTaskCreate(send_audio_task, "send_audio", AUDIO_TASK_STACK_SIZE, NULL, 5, &send_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create send audio task");
        vTaskDelete(audio_task_handle);
        vQueueDelete(send_audio_queue);
        vQueueDelete(send_queue);
        return false;
    }

    return true;
}

void deinit_ticos_audio(void) {
    if (audio_task_handle) {
        vTaskDelete(audio_task_handle);
        audio_task_handle = NULL;
    }
    if (send_task_handle) {
        vTaskDelete(send_task_handle);
        send_task_handle = NULL;
    }
    
    // Free elements in send_audio_queue
    if (send_audio_queue) {
        audio_data_t audio_data;
        while (xQueueReceive(send_audio_queue, &audio_data, 0) == pdTRUE) {
            free(audio_data.data);
        }
        vQueueDelete(send_audio_queue);
        send_audio_queue = NULL;
    }

    // Free elements in send_queue (contains base64 encoded strings)
    if (send_queue) {
        char *base64_audio;
        while (xQueueReceive(send_queue, &base64_audio, 0) == pdTRUE) {
            free(base64_audio);
        }
        vQueueDelete(send_queue);
        send_queue = NULL;
    }
    client = NULL;
}

bool send_audio(uint8_t *data, size_t len) {
    if (!send_audio_queue) {
        ESP_LOGE(TAG, "Audio queue not initialized");
        return false;
    }

    uint8_t *audio_copy = (uint8_t *)malloc(len);
    if (!audio_copy) {
        ESP_LOGE(TAG, "Failed to allocate memory for audio data");
        return false;
    }

    memcpy(audio_copy, data, len);
    audio_data_t audio_data = {
        .data = audio_copy,
        .len = len
    };

    if (xQueueSend(send_audio_queue, &audio_data, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to queue audio data");
        free(audio_copy);
        return false;
    }

    return true;
}

__attribute__((weak)) bool play_audio(uint8_t *audio_data, size_t decoded_len) {
    // This is a weak function that should be implemented by the application
    ESP_LOGW(TAG, "play_audio function not implemented");
    free(audio_data);
    return false;
}

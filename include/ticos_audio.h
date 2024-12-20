#ifndef TICOS_AUDIO_H
#define TICOS_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_websocket_client.h"

// Initialize audio processing components
bool init_ticos_audio(esp_websocket_client_handle_t ws_client);

// Deinitialize audio processing components
void deinit_ticos_audio(void);

// Send audio data for processing
bool send_audio(uint8_t *data, size_t len);

// Process received audio data
bool play_audio(uint8_t *audio_data, size_t decoded_len);

#endif // TICOS_AUDIO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Agent's functions
bool init_ticos_agent();
// Send audio data from mic to ticos server
bool send_audio(uint8_t *data, size_t len);
// Request server to create a response
bool create_response();
// Send a websocket message to server
bool send_message(const char *data);
bool deinit_ticos_agent();

// client's functions to be implemented
// Play audio received from server. It's client's responsibility to free data after the task is done
bool play_audio(uint8_t *data, size_t len);

// Function pointer type for message handler callback
typedef bool (*ticos_message_handler)(const char *data);

// Register a callback function to handle messages from server
bool register_message_handler(ticos_message_handler);
// Remove the registered message handler
bool remove_message_handler();

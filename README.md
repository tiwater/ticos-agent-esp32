# Ticos Agent Component

基于 ESP-IDF 的 Ticos Agent 组件。

## 编译

将本目录放入主工程的 components 目录，通过在主工程的 CMakeLists.txt 中添加指向本组件的配置，例如：

```
set(EXTRA_COMPONENT_DIRS ${EXTRA_COMPONENT_DIRS} components/ticos_agent)
idf_component_register(REQUIRES ticos_agent)
```

以上仅为示例，请根据实际工程结构调整。

## 配置

可在 idf.py menuconfig 的 Component config -> Ticos Agent Configuration 下对本组件进行配置。

目前可配置服务器地址 Ticos Server URL。

## API 说明

### 初始化与反初始化

```c
bool init_ticos_agent();
bool deinit_ticos_agent();
```

- `init_ticos_agent`: 初始化 Ticos Agent，建立与服务器的 WebSocket 连接。成功返回 true，失败返回 false。
- `deinit_ticos_agent`: 清理并关闭 Ticos Agent，断开与服务器的连接。成功返回 true，失败返回 false。

### 音频相关接口

```c
bool send_audio(uint8_t *data, size_t len);
bool play_audio(uint8_t *data, size_t len);
```

- `send_audio`: 发送音频数据到服务器。参数 data 为音频数据缓冲区，len 为数据长度。成功返回 true，失败返回 false。
- `play_audio`: 播放从服务器接收到的音频数据。**注意：此函数需要由应用程序实现**。参数 data 为音频数据缓冲区，len 为数据长度。使用完毕后需要释放 data 内存。成功返回 true，失败返回 false。

### 消息处理

```c
typedef bool (*ticos_message_handler)(const char *data);
bool register_message_handler(ticos_message_handler handler);
bool remove_message_handler();
```

- `register_message_handler`: 注册消息处理回调函数。当收到服务器消息时，会调用此函数处理。成功返回 true，失败返回 false。
- `remove_message_handler`: 移除已注册的消息处理回调函数。成功返回 true，失败返回 false。

### 其他接口

```c
bool create_response();
bool send_message(const char *data);
```

- `create_response`: 请求服务器创建响应。成功返回 true，失败返回 false。
- `send_message`: 发送文本消息到服务器。参数 data 为要发送的消息内容。成功返回 true，失败返回 false。
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
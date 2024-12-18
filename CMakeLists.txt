set(COMPONENT_SRCDIRS "src"
                      )

set(COMPONENT_REQUIRES esp_websocket_client)

set(COMPONENT_ADD_INCLUDEDIRS
    "include"
    )

idf_component_register(SRC_DIRS "${COMPONENT_SRCDIRS}"
                        SRCS "src/ticos_agent.c"
                        INCLUDE_DIRS "${COMPONENT_ADD_INCLUDEDIRS}"
                        REQUIRES "${COMPONENT_REQUIRES}"
                        PRIV_REQUIRES "json"
                        )
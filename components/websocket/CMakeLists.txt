idf_component_register(
    SRCS
        "websocket.cpp"
        "websocket_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client
)

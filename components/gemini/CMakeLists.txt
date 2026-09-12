idf_component_register(
    SRCS
        "gemini.cpp"
        "gemini_protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES websocket audio_engine cjson
)

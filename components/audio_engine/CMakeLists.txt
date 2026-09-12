idf_component_register(
    SRCS
        "audio_engine.cpp"
        "audio_input.cpp"
        "audio_output.cpp"
    INCLUDE_DIRS "include"
    REQUIRES audio_hal
)

idf_component_register(
    SRCS
        "main.c"
        "audio_input.c"
        "lora_sx1278.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp-sr
)

component_compile_options(-Wall -Wextra)


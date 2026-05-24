idf_component_register(
    SRCS "codex_pet_app.cpp"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core esp_http_client esp_wifi esp_netif esp_event nvs_flash
    WHOLE_ARCHIVE
)

target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-missing-field-initializers)

idf_component_register(
    SRCS "main.c"
         "radio_app.c"
         "radio_catalog.c"
         "radio_favorites.c"
         "radio_fonts.c"
         "radio_frame.c"
         "radio_icy.c"
         "radio_keyboard.c"
         "radio_keyboard_layout.c"
         "radio_player.c"
         "radio_store.c"
         "radio_styles.c"
         "radio_ui.c"
         "radio_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_netif esp_timer esp_wifi nvs_flash
             heap mbedtls esp_audio_codec
)

# The baseline demo pages (demo_*.c, ui_pixel*.c) stay in the tree as BSP
# references but are intentionally not compiled into this application: the
# radio app replaces the demo menu with its own screens and theme. Their host
# tests keep running because they compile those sources directly.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/radio_font_cjk_16.c"
)

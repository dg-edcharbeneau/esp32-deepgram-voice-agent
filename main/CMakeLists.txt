idf_component_register(
    SRCS
        main.c
        wifi_sta.c
        wifi_creds.c
        wifi_prov.c
        boot_button.c
        dg_agent.c
        audio_io.c
        audio_codecs.c
        heap_probe.c
        battery.c
        ui.c
        face_spectrum.c
        orb_geometry.c
        orb_raster.c
        face_orb.c
        session_ctl.c
        voices.c
        faces.c
        orb_colors.c
        agent_prompt.c
        agent_name.c
        api_key.c
        history_store.c
        arc_text.c
    INCLUDE_DIRS .
    # Flash rodata, no RAM until something reads it. Editing any of these
    # rebuilds, so a page or prompt change is a normal edit-and-flash.
    EMBED_TXTFILES
        # The captive-portal setup page, served verbatim by wifi_prov.c. It was a
        # C string literal until it grew past the point of being editable.
        #
        # Note the build directory: what gets embedded is the minified page that
        # the custom command below generates from portal.src.html. Edit the .src
        # file. The generated name is kept as portal.html so the embed symbol
        # stays _binary_portal_html_start and wifi_prov.c does not have to care.
        ${CMAKE_CURRENT_BINARY_DIR}/portal.html
        # The persona, one named block per file. Order lives in agent_prompt.c,
        # not here -- this list only decides what gets embedded.
        prompt/formatting.md
        prompt/identity.md
        prompt/conversation.md
        prompt/speaking.md
        prompt/substance.md
        prompt/substance-flux.md
        # Both duplex blocks are embedded; agent_prompt.c picks one at compile
        # time from CONFIG_MIC_GATE_WHILE_AGENT_SPEAKS. The unused one costs
        # flash rodata and no RAM.
        prompt/half-duplex.md
        prompt/full-duplex.md
        prompt/boundaries.md
        prompt/session.md
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_partition
        esp_http_server
        lwip
        mbedtls
        json
        esp_codec_dev
        waveshare__esp32_s3_touch_amoled_1_75c
        lvgl__lvgl
        espressif__esp_lvgl_adapter
        espressif__esp-dsp
        espressif__esp-sr
        espressif__button
)

# Minify the portal page into the build directory before it is embedded, so the
# source can stay commented and indented without spending the bytes in flash.
# The DEPENDS list is what makes an edit to either file rebuild the page.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/portal.html
    COMMAND ${python} ${COMPONENT_DIR}/minify_html.py
            ${COMPONENT_DIR}/portal.src.html
            ${CMAKE_CURRENT_BINARY_DIR}/portal.html
    DEPENDS ${COMPONENT_DIR}/portal.src.html
            ${COMPONENT_DIR}/minify_html.py
    COMMENT "Minifying portal.src.html"
    VERBATIM)
add_custom_target(portal_html DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/portal.html)
add_dependencies(${COMPONENT_LIB} portal_html)

# The HTTPS certificate is generated per build, never committed.
# tools/gen_dev_cert.py runs automatically when either PEM file is missing.
idf_build_get_property(python PYTHON)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/servercert.pem" OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/serverkey.pem")
    execute_process(
        COMMAND "${python}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen_dev_cert.py"
                --out "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE cert_result
        OUTPUT_VARIABLE cert_output
        ERROR_VARIABLE cert_error)
    if(NOT cert_result EQUAL 0)
        message(FATAL_ERROR "Could not generate the HTTPS development certificate: ${cert_error}")
    endif()
endif()

idf_component_register(
    SRCS
        "main.c"
        "board_hw.c"
        "sa818.c"
        "sa818_cfg.c"
        "sa818_freq.c"
        "ptt.c"
        "bringup.c"
        "audio_i2s.c"
        "audio_codec.c"
        "eq6.c"
        "voice.c"
        "speaker_diag.c"
        "adc_audio_clock.c"
        "voice_tail.c"
        "mp3.c"
        "ctcss.c"
        "channels.c"
        "net_wifi.c"
        "wifi_policy.c"
        "device_identity.c"
        "device_name.c"
        "control_lease.c"
        "app_control.c"
        "net_http.c"
        "netcom.c"
        "g711.c"
        "g722.c"     # ITU G.722 64 kbit/s, public domain (relay codec)
        "opus_link.c"   # Opus for the audio link (see opus_link.h)
        "opus_sweep.c"  # quality sweep across Opus configurations (opus_sweep.h)
        "netp2pnet.c"
        "netrelay.c"
        "bridge.c"   # analog <-> network bridge (NET-P2P / relay)
        "sdr.c"      # SDR: radio <-> browser web audio over /ws/sdr
        "frontpanel.c"  # front panel: 3 LEDs + PTT/function keys
        "radio_led.c"   # RF steady / real network voice 5 Hz activity
        "radio_activity.c" # compact read-only App activity snapshots
    INCLUDE_DIRS
        "."
    EMBED_TXTFILES
        "panel_index.html"
        "setup.html"
        "app_control.js"
        "device_name.js"
        "speaker_debug.html"
        "servercert.pem"    # self-signed LAN cert: HTTPS page -> microphone
        "serverkey.pem"
    EMBED_FILES
        "brand_icon.png"
        "favicon.png"
    REQUIRES
        driver
        esp_adc
        esp_timer
        esp_psram
        esphome__micro-opus   # Opus codec (managed component)
        nvs_flash
        spiffs          # storage partition: custom end-of-transmission tones
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2c
        esp_driver_i2s
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_https_server   # https://<ip>/ so browsers expose the microphone
        app_update      # esp_ota_ops: the panel's firmware upload (/api/ota)
        bootloader_support # verify running image length for full OTA readback
        esp_app_format  # esp_app_get_description(), for the build stamp in the log
        dns_server      # vendored captive-portal DNS redirect (see components/)
        mdns            # vendored mDNS responder -> http://orl.local/
        es8311          # vendored under firmware/components/es8311 (see PATCH.md)
        mqtt            # esp-mqtt client for the EMQX relay engine (netrelay.c)
        mbedtls         # HMAC-SHA256 PSK handshake in the NET-P2P engine (netp2pnet.c)
        lwip            # sockets + getaddrinfo for the NET-P2P engine (netp2pnet.c)
        json            # bounded app/provision/control JSON request bodies
)

# Apply the exact ADC sample clock inside the driver's start sequence, before
# conversion is enabled, without modifying the SDK shared with the original.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=adc_hal_digi_controller_config")

# HTTP tasks have 10 KB stacks. Reserve at least 4 KB beyond each local frame
# for routing, TLS and newlib formatting, and retain compiler stack reports.
set_source_files_properties(net_http.c PROPERTIES
    COMPILE_OPTIONS "-Werror=frame-larger-than=6144;-fstack-usage")

idf_component_register(
    SRCS "main.c" "usb_host_cdc.c" "wifi_manager.c" "web_server.c" "web_util.c" "spectrum.c" "tcp_bridge.c" "spectrogram.c" "web_waterfall.c" "boot_config.c" "wf_offload.c" "monitor.c" "net_time.c" "battery.c" "rgb_led.c" "board_i2c.c" "tft_display.c" "font_cps64.c" "font_cps104.c" "font_cps80.c" "rtc_ds3231.c" "sd_card.c" "sd_log.c" "sys_load.c" "web_files.c" "ota_update.c" "cps_alarm.c" "autocal.c" "kerfur_face.c" "isoid.c" "iso_live.c" "log_capture.c" "hang_watch.c"
    INCLUDE_DIRS "."
    REQUIRES usb_host_cdc_acm esp_wifi esp_http_server nvs_flash esp_event esp_timer littlefs shproto json mdns esp_http_client mbedtls esp_app_format driver esp_netif led_strip esp_lcd fatfs sdmmc esp_driver_sdspi app_update
    EMBED_FILES "${PROJECT_DIR}/web/index.html" "${PROJECT_DIR}/web/setup.html" "${PROJECT_DIR}/web/waterfall.html" "${PROJECT_DIR}/web/saved.html" "${PROJECT_DIR}/web/system.html" "${PROJECT_DIR}/web/service.html" "${PROJECT_DIR}/web/monitor.html" "${PROJECT_DIR}/web/common-time.js" "${PROJECT_DIR}/web/captive.html"
)

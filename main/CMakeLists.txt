set(MAIN_SRCS
    "espd.c"
    "espd_config_file.c"
    "espd_gpio_peripherals.c"
    "espd_usb.c"
    "espd_pd_io.c"
    "pd_lseek64_compat.c"
    "pd_posix_file_compat.c"
    "pd_pdcontrol_compat.c"
    "espd_storage.c"
    "espd_dev.c"
    "pdmain.c"
    "espdsp_osc_override.c"
    "pd_cputime_compat.c"
    "../pd/src/g_canvas.c"
    "../pd/src/g_graph.c"
    "../pd/src/g_text.c"
    "../pd/src/g_rtext.c"
    "../pd/src/g_array.c"
    "../pd/src/g_bang.c"
    "../pd/src/g_template.c"
    "../pd/src/g_io.c"
    "../pd/src/g_scalar.c"
    "../pd/src/g_all_guis.c"
    "../pd/src/g_mycanvas.c"
    "../pd/src/g_numbox.c"
    "../pd/src/g_radio.c"
    "../pd/src/g_guiconnect.c"
    "../pd/src/g_readwrite.c"
    "../pd/src/g_clone.c"
    "../pd/src/g_slider.c"
    "../pd/src/g_toggle.c"
    "../pd/src/g_vumeter.c"
    "../pd/src/m_pd.c"
    "../pd/src/m_class.c"
    "../pd/src/m_obj.c"
    "../pd/src/m_atom.c"
    "../pd/src/m_memory.c"
    "../pd/src/m_binbuf.c"
    "../pd/src/s_net.c"
    "../pd/src/s_print.c"
    "../pd/src/s_utf8.c"
    "../pd/src/d_ugen.c"
    "../pd/src/d_resample.c"
    "../pd/src/d_soundfile.c"
    "../pd/src/d_soundfile_wave.c"
    "../pd/src/d_soundfile_aiff.c"
    "../pd/src/d_soundfile_caf.c"
    "../pd/src/d_soundfile_next.c"
    "../pd/src/d_global.c"
    "../pd/src/d_osc.c"
    "../pd/src/d_ctl.c"
    "../pd/src/d_arithmetic.c"
    "../pd/src/d_array.c"
    "../pd/src/d_dac.c"
    "../pd/src/d_delay.c"
    "../pd/src/d_filter.c"
    "../pd/src/d_math.c"
    "../pd/src/d_fft.c"
    "../pd/src/d_fft_fftsg.c"
    "../pd/src/d_misc.c"
    "../pd/src/x_arithmetic.c"
    "../pd/src/x_connective.c"
    "../pd/src/x_file.c"
    "../pd/src/x_interface.c"
    "../pd/src/x_misc.c"
    "../pd/src/x_net.c"
    "../pd/src/x_time.c"
    "../pd/src/x_acoustics.c"
    "../pd/src/x_gui.c"
    "../pd/src/x_array.c"
    "../pd/src/x_scalar.c"
    "../pd/src/x_text.c"
    "../pd/src/x_list.c"
    "../pd/src/x_vexp.c"
    "../pd/src/x_vexp_fun.c"
    "../pd/src/x_vexp_if.c"
    "../pd/src/s_inter_gui.c"
)

# FIXME: remove these warning suppression flags once Pd is fixed
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPD -DHAVE_UNISTD_H -DHAVE_ALLOCA_H -DPD_HEADLESS -DPD_INTERNAL -DSYMTABHASHSIZE=512 -DSTUPID_SORT -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-format -Wno-stringop-truncation -Wno-unused-const-variable -Wno-parentheses -Wno-maybe-uninitialized -Wno-unused-label -Wno-char-subscripts -Wno-cast-function-type -Wno-compare-distinct-pointer-types -Wno-cpp -Wno-empty-body -Wno-type-limits -Wno-unterminated-string-initialization -Wno-missing-field-initializers -Wno-implicit-fallthrough")

set(MAIN_REQUIRES
    console
    driver
    esp_adc
    nvs_flash
    spi_flash
    fatfs
    wear_levelling
    esp_timer
    esp_event
    esp_netif
    esp_driver_ledc
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_gpio
    esp_driver_uart
    esp_driver_dac
    lwip
    pthread
    sock_utils
    espd_integration
    espd_boards
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${MAIN_REQUIRES}
)

if(CONFIG_ESPD_USE_USB_OTG)
    idf_component_optional_requires(PRIVATE esp_tinyusb espressif__esp_tinyusb
        esp_partition esp_driver_usb_serial_jtag)
endif()

if(CONFIG_ESPD_USE_WIFI)
    target_sources(${COMPONENT_LIB} PRIVATE "../wifi/wifi.c" "../wifi/net.c")
    idf_component_get_property(_main_lib main COMPONENT_LIB)
    target_link_libraries(${_main_lib} PRIVATE idf::esp_wifi)
    if(CONFIG_ESP_WIFI_REMOTE_ENABLED)
        idf_component_optional_requires(PRIVATE espressif__esp_hosted)
    endif()
endif()

if(CONFIG_ESPD_AUDIO_BACKEND_BSP_CODEC STREQUAL "y")
    target_sources(${COMPONENT_LIB} PRIVATE "espd_audio_codec.c")
elseif(CONFIG_ESPD_AUDIO_BACKEND_GENERIC_I2S STREQUAL "y")
    target_sources(${COMPONENT_LIB} PRIVATE "espd_audio_generic.c")
elseif(CONFIG_ESPD_AUDIO_BACKEND_DAC STREQUAL "y")
    target_sources(${COMPONENT_LIB} PRIVATE "espd_audio_dac.c")
endif()

idf_component_get_property(_espd_main_lib ${COMPONENT_NAME} COMPONENT_LIB)
# Ooura FFT in pd/src/d_fft_fftsg.c: default FFTFLT is double; use float IRAM.
target_compile_definitions(${_espd_main_lib} PRIVATE "FFTFLT=float")

# Aggressive math optimization for DSP hot paths. -ffast-math allows the
# compiler to reassociate, fold reciprocals, drop spurious double promotions
# (e.g. `1. - frac` in cubic interpolators), and assume no NaN/Inf. Safe here:
#   - PD_BIGORSMALL() is a bit-pattern union check, not a FP compare
#   - none of these files rely on errno or isnan/isinf/fpclassify
# Scope is just the per-sample DSP files; non-audio-rate code keeps strict math.
set_source_files_properties(
    "espdsp_osc_override.c"
    "../pd/src/d_arithmetic.c"
    "../pd/src/d_array.c"
    "../pd/src/d_ctl.c"
    "../pd/src/d_delay.c"
    "../pd/src/d_fft.c"
    "../pd/src/d_fft_fftsg.c"
    "../pd/src/d_filter.c"
    "../pd/src/d_global.c"
    "../pd/src/d_math.c"
    "../pd/src/d_misc.c"
    "../pd/src/d_osc.c"
    "../pd/src/d_resample.c"
    PROPERTIES COMPILE_OPTIONS "-ffast-math;-fno-math-errno"
)

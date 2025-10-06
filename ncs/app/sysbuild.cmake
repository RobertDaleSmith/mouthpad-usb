# Sysbuild configuration for MouthPad USB

# For Adafruit Feather board, use static partition file
if(BOARD STREQUAL "adafruit_feather_nrf52840")
  set(PM_STATIC_YML_FILE ${APP_DIR}/pm_static_adafruit_feather_nrf52840.yml CACHE INTERNAL "")
endif()
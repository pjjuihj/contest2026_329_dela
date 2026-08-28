# VelaWear Agent - CMakeLists.txt

if(CONFIG_LVX_USE_DEMO_CONTEST2026_329_VELAWEAR_AGENT)
  nuttx_add_application(
    NAME velawear
    SRCS
      velawear_main.c
      core/event_manager.c
      core/state_manager.c
      core/decision_engine.c
      core/action_manager.c
      config/velawear_config.c
      drivers/imu_sensor.c
      drivers/velawear_ble.c
      drivers/velawear_pan.c
      drivers/velawear_agent_protocol.c
      drivers/velawear_llm.c
      drivers/audio_sensor.c
      drivers/display_manager.c
      drivers/velawear_mvp_font.c
      drivers/audio_hw_test.c
      skills/velawear_wellness_skill.c
      xiaozhi/xiaozhi.c
      xiaozhi/xiaozhi_audio.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}
    INCLUDE_DIRECTORIES
      ${NUTTX_APPS_DIR}/crypto/mbedtls/mbedtls/include
      ${NUTTX_APPS_DIR}/netutils/cjson/cJSON
      ${CMAKE_SOURCE_DIR}/../frameworks/connectivity/bluetooth/framework/include
      ${CMAKE_SOURCE_DIR}/../external/opus/opus/include
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/sifli/chips/external/CMSIS/Include
    # Derive SDK and generated-header paths from the source/build roots so
    # this project remains reproducible outside the original VM checkout.
    INCLUDE_DIRECTORIES
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/sifli/chips/drivers/Include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/sifli/chips/drivers/cmsis/Include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/sifli/chips/drivers/cmsis/sf32lb52x
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../apps/external/zblue/zblue/port/include
      ${CMAKE_BINARY_DIR}/apps/external/zblue/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../apps/external/zblue/zblue/port/kernel/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../apps/external/zblue/zblue/include
      ${CMAKE_CURRENT_SOURCE_DIR}/../../../apps/external/zblue/zblue/port/include/zephyr
      ${CMAKE_BINARY_DIR}/apps/external/zblue/include/zephyr/sys
      ${CMAKE_BINARY_DIR}/apps/external/zblue/include/zephyr/kernel
      ${CMAKE_BINARY_DIR}/apps/external/zblue/include/zephyr/arch
    DEPENDS
      lvgl
      libopus
    STACKSIZE 40960)

  # BLE is initialized after the display.  The Agent GATT gateway requires it.
  target_compile_definitions(apps_velawear PRIVATE VELAWEAR_ENABLE_BLE=1)
endif()

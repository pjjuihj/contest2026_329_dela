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
      drivers/velawear_llm.c
      drivers/audio_sensor.c
      drivers/display_manager.c
      drivers/audio_hw_test.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}
    INCLUDE_DIRECTORIES
      /home/cmj/桌面/1/contest2026_329_dela/openvela/vendor/sifli/chips/drivers/Include
      /home/cmj/桌面/1/contest2026_329_dela/openvela/vendor/sifli/chips/drivers/cmsis/Include
      /home/cmj/桌面/1/contest2026_329_dela/openvela/vendor/sifli/chips/drivers/cmsis/sf32lb52x
    DEPENDS
      lvgl
    STACKSIZE 40960)
endif()

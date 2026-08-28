############################################################################
# VelaWear Agent - Makefile
############################################################################

include $(APPDIR)/Make.defs

# Main program
PROGNAME  = velawear
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_329_VELAWEAR_AGENT)

# Source files
MAINSRC   = velawear_main.c

CSRCS     = core/event_manager.c
CSRCS    += core/state_manager.c
CSRCS    += core/decision_engine.c
CSRCS    += core/action_manager.c
CSRCS    += config/velawear_config.c
CSRCS    += drivers/imu_sensor.c
CSRCS    += drivers/velawear_llm.c
CSRCS    += drivers/audio_sensor.c

include $(APPDIR)/Application.mk

fm_drv-objs := fmdrv_main.o fmdrv_v4l2.o fmdrv_rx.o
obj-$(CONFIG_V4L2_ANT_DRIVER) := fm_drv.o
ccflags-y += -w -DVERSION="\"1.2\""
ifneq ($(CONFIG_SOMC_FM_AUDIO_PATH),)
  ccflags-y += -DDEF_V4L2_FM_AUDIO_PATH=$(CONFIG_SOMC_FM_AUDIO_PATH)
endif

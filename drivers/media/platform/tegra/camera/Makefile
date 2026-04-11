GCOV_PROFILE := y
ccflags-y += -Idrivers/video/tegra/host
ccflags-y += -Idrivers/media/platform/tegra
ccflags-y += -Werror
ccflags-y += -Idrivers/media/platform/tegra/mipical
obj-$(CONFIG_VIDEO_TEGRA_VI) += camera_common.o mc_common.o core.o channel.o graph.o \
	 t124_capture.o t124_singleshot.o isp_trace.o
obj-$(CONFIG_VIDEO_TEGRA_VI) += isp_t124.o

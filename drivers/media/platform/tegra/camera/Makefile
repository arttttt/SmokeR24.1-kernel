GCOV_PROFILE := y
ccflags-y += -Idrivers/video/tegra/host
ccflags-y += -Idrivers/media/platform/tegra
ccflags-y += -Werror
ccflags-y += -Idrivers/media/platform/tegra/mipical
obj-y += camera_common.o mc_common.o core.o channel.o graph.o \
	 t124_capture.o singleshot_capture.o
obj-$(CONFIG_TEGRA_GRHOST_ISP) += isp_t124.o
obj-y += isp_trace.o

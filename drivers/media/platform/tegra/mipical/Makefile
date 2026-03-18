GCOV_PROFILE := y
#
# Makefile for Mipical driver
#
# T210 has full runtime calibration driver
obj-$(CONFIG_ARCH_TEGRA_21x_SOC) += mipi_cal.o
# T124 only needs bias pad init, no runtime calibration
obj-$(CONFIG_ARCH_TEGRA_12x_SOC) += mipi_cal_t124.o

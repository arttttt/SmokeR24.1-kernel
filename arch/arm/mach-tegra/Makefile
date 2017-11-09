GCOV_PROFILE := y
GCOV_PROFILE_panel-a-1080p-11-6.o := n
GCOV_PROFILE_panel-a-1080p-14-0.o := n
GCOV_PROFILE_panel-a-1200-1920-8-0.o := n
GCOV_PROFILE_panel-a-1200-800-8-0.o := n
GCOV_PROFILE_panel-a-edp-1080p-14-0.o := n
GCOV_PROFILE_panel-c-lvds-1366-14.o := n
GCOV_PROFILE_panel-i-edp-1080p-11-6.o := n
GCOV_PROFILE_panel-j-1440-810-5-8.o := n
GCOV_PROFILE_panel-j-720p-5.o := n
GCOV_PROFILE_panel-l-720p-5-loki.o := n
GCOV_PROFILE_panel-lgd-wxga-7-0.o := n
GCOV_PROFILE_panel-o-720p-6-0.o := n
GCOV_PROFILE_panel-o-720p-6-0-01.o := n
GCOV_PROFILE_panel-p-wuxga-10-1.o := n
GCOV_PROFILE_panel-s-edp-uhdtv-15-6.o := n
GCOV_PROFILE_panel-s-edp-wqxgap-15-6.o := n
GCOV_PROFILE_panel-s-wqxga-10-1.o := n
GCOV_PROFILE_panel-s-wuxga-7-0.o := n
GCOV_PROFILE_panel-s-wuxga-8-0.o := n


asflags-y				+= -march=armv7-a
subdir-ccflags-y := -Werror
subdir-ccflags-y += -I$(srctree)/drivers/platform/tegra/include

obj-y                                   += devices.o
obj-y                                   += board-info.o
obj-$(CONFIG_HAVE_ARM_TWD)              += cpu_timer_twd.o


ifeq ($(CONFIG_ARCH_TEGRA_2x_SOC),y)
obj-y                                   += common-t2.o
obj-y                                   += pm-t2.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= sleep-t20.o
obj-$(CONFIG_PM_SLEEP)                  += wakeups-t2.o
ifeq ($(CONFIG_CPU_IDLE),y)
obj-$(CONFIG_PM_SLEEP)                  += cpuidle-t2.o
endif
else
obj-y                                   += sleep-t30.o
obj-$(CONFIG_TEGRA_USE_SIMON)           += tegra_simon.o

ifeq ($(CONFIG_CPU_IDLE),y)
ifeq ($(CONFIG_PM_SLEEP),y)
obj-$(CONFIG_ARCH_TEGRA_12x_SOC)        += cpuidle-t11x.o
endif
endif
endif

obj-y                                   += delay.o
obj-y                                   += pm.o

obj-y                                   += sleep.o
obj-$(CONFIG_TEGRA_USE_NCT)             += nct.o nct_sysfs.o

plus_sec := $(call as-instr,.arch_extension sec,+sec)
AFLAGS_sleep.o :=-Wa,-march=armv7-a$(plus_sec)

obj-y                                   += kfuse.o

obj-y                                   += apbio.o
obj-$(CONFIG_TEGRA_ARB_SEMAPHORE)       += arb_sema.o

obj-$(CONFIG_TEGRA_CORE_EDP_LIMITS)     += edp_core.o
obj-$(CONFIG_ARCH_TEGRA_12x_SOC)        += tegra12_edp.o
ifeq ($(CONFIG_ARCH_TEGRA_13x_SOC),y)
obj-y                                   += tegra13_speedo.o
else
obj-$(CONFIG_ARCH_TEGRA_12x_SOC)        += tegra12_speedo.o
endif
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)         += tegra2_emc.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)         += tegra3_emc.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= tegra20_speedo.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= tegra30_speedo.o
obj-$(CONFIG_HOTPLUG_CPU)               += hotplug.o
obj-$(CONFIG_CPU_IDLE)                  += cpuidle.o
obj-$(CONFIG_CPU_IDLE)                  += sleep.o
obj-$(CONFIG_SMP)                       += platsmp.o
obj-y                                   += headsmp.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)         += board-dt-tegra20.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)         += board-dt-tegra30.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-harmony-pcie.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-paz00.o

obj-y					+= board-common.o

obj-y                                   += board-touch-raydium_spi.o
obj-y                                   += board-touch-synaptics-spi.o
obj-y                                   += board-touch-maxim_sti-spi.o

obj-y                                   += board-panel.o
obj-y                                   += panel-p-wuxga-10-1.o
obj-y                                   += panel-lgd-wxga-7-0.o
obj-y                                   += panel-s-wqxga-10-1.o
obj-y                                   += panel-s-wuxga-7-0.o
obj-y                                   += panel-s-wuxga-8-0.o
obj-y                                   += panel-a-1200-800-8-0.o
obj-y                                   += panel-a-1200-1920-8-0.o
obj-y                                   += panel-a-edp-1080p-14-0.o
obj-y                                   += panel-i-edp-1080p-11-6.o
obj-y                                   += panel-c-lvds-1366-14.o
obj-y                                   += panel-a-1080p-14-0.o
obj-y                                   += panel-j-1440-810-5-8.o
obj-y                                   += panel-j-720p-5.o
obj-y                                   += panel-l-720p-5-loki.o
obj-y                                   += panel-s-edp-uhdtv-15-6.o
obj-y                                   += panel-o-720p-6-0.o
obj-y                                   += panel-o-720p-6-0-01.o
obj-y                                   += panel-s-wqxga-7-9-x6.o

obj-${CONFIG_MACH_ARDBEG}               += board-ardbeg.o
obj-${CONFIG_MACH_ARDBEG}               += board-ardbeg-sensors.o
obj-${CONFIG_MACH_ARDBEG}               += board-ardbeg-memory.o
obj-${CONFIG_MACH_ARDBEG}               += board-norrin-power.o
obj-${CONFIG_MACH_ARDBEG}               += board-norrin-memory.o
obj-${CONFIG_MACH_ARDBEG}               += panel-a-1080p-11-6.o
obj-${CONFIG_MACH_ARDBEG}               += panel-s-edp-wqxgap-15-6.o

obj-${CONFIG_MACH_LOKI}               	+= board-loki.o
obj-${CONFIG_MACH_LOKI}               	+= board-loki-kbc.o
obj-${CONFIG_MACH_LOKI}               	+= board-loki-sensors.o
obj-${CONFIG_MACH_LOKI}               	+= board-loki-panel.o
obj-${CONFIG_MACH_LOKI}               	+= board-loki-memory.o
obj-${CONFIG_MACH_LOKI}               	+= board-loki-power.o
obj-${CONFIG_MACH_LOKI}               	+= board-loki-sdhci.o
obj-${CONFIG_MACH_LOKI}               	+= panel-a-1080p-11-6.o

obj-${CONFIG_MACH_LAGUNA}               += board-ardbeg.o
obj-${CONFIG_MACH_LAGUNA}               += board-ardbeg-sensors.o
obj-${CONFIG_MACH_LAGUNA}               += board-laguna-power.o
obj-${CONFIG_MACH_LAGUNA}               += panel-a-1080p-11-6.o

obj-${CONFIG_ARCH_TEGRA_VCM30T124}      += vcm30_t124.o

obj-${CONFIG_MACH_P1859}                += board-p1859.o
obj-${CONFIG_MACH_P1859}                += board-p1859-audio.o

obj-${CONFIG_MACH_P1889}                += board-p1889.o
obj-${CONFIG_MACH_P1889}                += board-p1889-power.o
obj-${CONFIG_MACH_P1889}                += board-p1889-audio.o

obj-${CONFIG_MACH_P2360}                += board-p2360.o

obj-${CONFIG_MACH_P1855}                += board-p1855.o

obj-${CONFIG_TEGRA_BB_XMM_POWER}        += baseband-xmm-power.o
obj-${CONFIG_TEGRA_BB_XMM_POWER2}       += baseband-xmm-power2.o

obj-${CONFIG_TEGRA_BASEBAND}            += tegra_bb.o
obj-$(CONFIG_TEGRA_BBC_PROXY)           += tegra_bbc_proxy.o
obj-$(CONFIG_TEGRA_BBC_THERMAL)         += tegra_bbc_thermal.o



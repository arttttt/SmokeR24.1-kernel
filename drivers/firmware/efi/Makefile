#
# Makefile for linux kernel
#

# Prevents link failures: __sanitizer_cov_trace_pc() is not linked in.
KCOV_INSTRUMENT	:= n

obj-y					+= efi.o vars.o
obj-$(CONFIG_EFI_VARS)			+= efivars.o
obj-$(CONFIG_EFI_VARS_PSTORE)		+= efi-pstore.o

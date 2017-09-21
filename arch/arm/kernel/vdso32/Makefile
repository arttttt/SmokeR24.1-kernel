
# List of files in the vdso, has to be asm only for now

obj-vdso32 = vgettimeofday.o $(obj-vdso32-y)

# Handle FTRACE
CFLAGS_REMOVE_vgettimeofday.o = -pg

# Build rules

targets := $(obj-vdso32) vdso32.so vdso32.so.dbg
obj-vdso32 := $(addprefix $(obj)/, $(obj-vdso32))

GCOV_PROFILE := n

ccflags-y := -shared -fPIC -fno-common -fno-builtin -march=armv7-a
ccflags-y += -nostdlib -Wl,-soname=linux-vdso32.so.1 \
		$(call cc-ldoption, -Wl$(comma)--hash-style=sysv) \
		$(call cc-option, -fuse-ld=bfd)
asflags-y := -D__VDSO32__ -s

obj-y += vdso32_wrapper.o
extra-y += vdso32.lds vdso-offsets.h
CPPFLAGS_vdso32.lds += -P -C

# Force dependency (incbin is bad)
$(obj)/vdso32_wrapper.o : $(obj)/vdso32.so

# link rule for the .so file, .lds has to be first
$(obj)/vdso32.so.dbg: $(src)/vdso32.lds $(obj-vdso32)
	$(call if_changed,vdso32ld)

# strip rule for the .so file
$(obj)/%.so: OBJCOPYFLAGS := -S
$(obj)/%.so: $(obj)/%.so.dbg FORCE
	$(call if_changed,objcopy)

# Generate VDSO offsets using helper script
gen-vdsosym := $(srctree)/$(src)/gen_vdso_offsets.sh
quiet_cmd_vdsosym = VDSOSYM $@
define cmd_vdsosym
	$(NM) $< | $(gen-vdsosym) | LC_ALL=C sort > $@ && \
	cp $@ include/generated/
endef

$(obj)/vdso-offsets.h: $(obj)/vdso32.so.dbg FORCE
	$(call if_changed,vdsosym)

# assembly rules for the .S files
$(obj-vdso32): %.o: %.c
	$(call if_changed_dep,vdso32cm)

# actual build commands
quiet_cmd_vdso32ld = VDSO32L $@
      cmd_vdso32ld = $(CC) $(c_flags) -Wl,-T $^ -o $@
quiet_cmd_vdso32as = VDSO32A $@
      cmd_vdso32as = $(CC) $(a_flags) -c -o $@ $<
quiet_cmd_vdso32cm = VDSO32C $@
      cmd_vdso32cm = $(CC) $(c_flags) -c -o $@ $<

# install commands for the unstripped file
quiet_cmd_vdso_install = INSTALL $@
      cmd_vdso_install = cp $(obj)/$@.dbg $(MODLIB)/vdso/$@

vdso32.so: $(obj)/vdso32.so.dbg
	@mkdir -p $(MODLIB)/vdso
	$(call cmd,vdso_install)

vdso_install: vdso32.so



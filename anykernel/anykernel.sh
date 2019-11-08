# AnyKernel2 Ramdisk Mod Script
# osm0sis @ xda-developers

## AnyKernel setup
# begin properties
properties() {
kernel.string=SmokeR24.1-stable
do.devicecheck=1
do.modules=0
do.cleanup=1
do.cleanuponabort=0
device.name1=mocha
} # end properties

# shell variables
block=/dev/block/platform/700b0600.sdhci/by-name/boot;
is_slot_device=0;


## AnyKernel methods (DO NOT CHANGE)
# import patching functions/variables - see for reference
. /tmp/anykernel/tools/ak2-core.sh;


## AnyKernel file attributes
# set permissions/ownership for included ramdisk files
chmod -R 750 $ramdisk/*;
chmod 644 $ramdisk/sbin/media_profiles.xml;
chmod -R root:root $ramdisk/*;


## AnyKernel install
dump_boot;

write_boot;

## end install


# ZephyrOS system build configuration (package 28b).
#
# ZOS_BOOT_PROGRAM is the one setting that selects the program the kernel
# launcher starts (kernel/main.c, kernel_boot_launcher).  It is compiled
# into the kernel as a constant string; switching the boot target is this
# line plus adding the ZEX to the selected recovery manifest, with no
# kernel edit.  tools/bootprogram.py enforces the rules and fails the
# build otherwise:
#
#   - an absolute path under /system, so the program boots without SD;
#   - at most 127 bytes (ZOS_PATH_MAX - 1);
#   - the named file is a ZEX present in the selected recovery manifest.
#
# The production default is the monitor; M10 sets it to the shell.
ZOS_BOOT_PROGRAM ?= /system/monitor.zex

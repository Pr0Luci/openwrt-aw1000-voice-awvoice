# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Pr0Luci
#
# OpenWrt kernel-module package: the line chip's register bus, the PCM block and the audio bridge of the phone port.
# Only this package depends on the kernel build; kmodloader loads it at boot from /etc/modules.d with register writes
# allowed (the daemon needs them). It is deliberately not in /etc/modules-boot.d: the preinit loader reads that from
# the firmware image before the overlay is mounted, so after an upgrade the image's older module would be loaded and
# the installed one never used (1.1.0 did exactly that; see the postinst below and the daemon's ABI check).

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=kmod-awvoice-fxs
PKG_VERSION:=1.1.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=COPYING
PKG_MAINTAINER:=Pr0Luci

include $(INCLUDE_DIR)/package.mk

define KernelPackage/awvoice-fxs
  SUBMENU:=Other modules
  TITLE:=AWvoice phone port (RJ11): line chip access and audio bridge
  DEPENDS:=@TARGET_qualcommax_ipq807x
  FILES:=$(PKG_BUILD_DIR)/awvoice_fxs.ko
  AUTOLOAD:=$(call AutoLoad,50,awvoice_fxs)
  MODPARAMS.awvoice_fxs:=allow_write=1
endef

define KernelPackage/awvoice-fxs/description
  Register bus (bit-banged), PCM block, the kernel audio bridge between the modem and the line chip, and the ring watchdog
  that puts the chip back in reset when the phone daemon stops. Source: GPL-2.0-only.
endef

# the boot-time entry 1.1.0 wrote is gone from the package; an upgrade that kept it loses it here (see the top).
# The hook is named for the package the kernel machinery builds (kmod-awvoice-fxs), not for the KernelPackage section.
define Package/kmod-awvoice-fxs/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
if [ -e /etc/modules-boot.d/50-awvoice-fxs ]; then
	rm -f /etc/modules-boot.d/50-awvoice-fxs
	logger -t awvoice "removed the boot-time module entry of 1.1.0: awvoice_fxs loads from the installed module at the next boot"
fi
exit 0
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src ./include ./Kbuild ./COPYING $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,awvoice-fxs))

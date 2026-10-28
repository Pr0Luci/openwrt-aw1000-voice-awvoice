# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Pr0Luci
#
# OpenWrt kernel-module package: the line chip's register bus, the PCM block and the audio bridge of the phone port.
# Only this package depends on the kernel build; it is loaded at boot with register writes allowed (the daemon needs them),
# so the phone service does not load it by path any more.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=kmod-awvoice-fxs
PKG_VERSION:=1.1.0
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
  AUTOLOAD:=$(call AutoLoad,50,awvoice_fxs,1)
  MODPARAMS.awvoice_fxs:=allow_write=1
endef

define KernelPackage/awvoice-fxs/description
  Register bus (bit-banged), PCM block, the kernel audio bridge between the modem and the line chip, and the ring watchdog
  that puts the chip back in reset when the phone daemon stops. Source: GPL-2.0-only.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src ./include ./Kbuild ./COPYING $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,awvoice-fxs))

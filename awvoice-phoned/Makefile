# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Pr0Luci
#
# OpenWrt package: the phone daemon, its service script and its default settings.

include $(TOPDIR)/rules.mk

PKG_NAME:=awvoice-phoned
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Pr0Luci

include $(INCLUDE_DIR)/package.mk

define Package/awvoice-phoned
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=AWvoice phone daemon (RJ11 phone port: calls, tones, caller id, ubus awvoice)
  DEPENDS:=+libubox +libubus +libuci +kmod-awvoice-fxs
endef

define Package/awvoice-phoned/description
  Serves the phone port: the hook, the dial tone and progress tones, digit collection and dialling, the ring with the caller's
  identity, call control through the modem's AT port, the ubus object awvoice and the settings in UCI awvoice. The board
  file /etc/awvoice/board.bin ships with this package (the default is installed at the first start; a file already there
  is never overwritten) - configuration data only, no chip-maker microcode.
endef

define Package/awvoice-phoned/conffiles
/etc/config/awvoice
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src ./files ./Makefile.host $(PKG_BUILD_DIR)/
	$(CP) ../kmod-awvoice-fxs/include $(PKG_BUILD_DIR)/kmod-include
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) -f Makefile.host awvoice-phoned \
		CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -Wall -Wextra" CPPFLAGS="$(TARGET_CPPFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" UAPI="$(PKG_BUILD_DIR)/kmod-include/uapi"
endef

define Package/awvoice-phoned/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config $(1)/usr/share/awvoice
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/awvoice-phoned $(1)/usr/sbin/awvoice-phoned
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/awvoice.init $(1)/etc/init.d/awvoice
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/files/awvoice.config $(1)/etc/config/awvoice
	$(INSTALL_DATA) files/board.bin $(1)/usr/share/awvoice/board-default.bin
	# board.bin lives under /etc, which sysupgrade keeps, and the default board
	# file is reinstalled at the first start if it is gone - it is never listed
	# here: a package may not ship a file another package already owns
	# (/etc/sysupgrade.conf belongs to base-files; apk rejects the overlap).
endef

# enabled at boot, not started here: a package install never touches a live phone line (the installer starts it)
define Package/awvoice-phoned/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/awvoice enable
exit 0
endef

define Package/awvoice-phoned/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/awvoice stop 2>/dev/null
/etc/init.d/awvoice disable 2>/dev/null
exit 0
endef

$(eval $(call BuildPackage,awvoice-phoned))

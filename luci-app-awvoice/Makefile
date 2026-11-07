# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Pr0Luci
#
# luci-app-awvoice: the AWvoice phone page (Services > AWvoice(RJ11)). Pure resources, nothing to compile. The page
# talks to the phone daemon through ubus (object awvoice) and UCI (package awvoice).

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-awvoice
PKG_VERSION:=1.1.5
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Pr0Luci

LUCI_TITLE:=AWvoice phone page (RJ11)
LUCI_DESCRIPTION:=Status, settings, health and test tools of the RJ11 phone port
# luci-lib-uqr draws the code a phone scans: the SIP tab requires the module when it
# loads, so without it the whole page fails, not only the button (measured by taking
# uqr.js away from a running router).
LUCI_DEPENDS:=+luci-base +luci-lib-uqr
LUCI_PKGARCH:=all

LUCI_URL:=
LUCI_MAINTAINER:=Pr0Luci


include $(TOPDIR)/feeds/luci/luci.mk


# call BuildPackage - OpenWrt buildroot signature

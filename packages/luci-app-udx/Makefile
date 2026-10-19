SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=luci-app-udx
PKG_VERSION:=3.1
PKG_RELEASE:=6
PKG_LICENSE:=GPL-2.0-only
PKGARCH:=all
PKG_BUILD_DEPENDS:=luci-base/host
SY108_SOURCE_PATHS := packages/luci-app-udx
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/luci-app-udx
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=UDX710 device management
  DEPENDS:=+udx-device +luci-base +luci-mod-system +luci-i18n-base-zh-cn +luci-app-sy108-system
endef

define Build/Compile
	po2lmo $(PKG_BUILD_DIR)/packages/luci-app-udx/po/zh_Hans/udx.po $(PKG_BUILD_DIR)/udx.zh-cn.lmo
endef

define Package/luci-app-udx/install
	$(INSTALL_DIR) $(1)
	$(CP) $(PKG_BUILD_DIR)/packages/luci-app-udx/root/. $(1)/
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/i18n
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/udx.zh-cn.lmo $(1)/usr/lib/lua/luci/i18n/
endef
$(eval $(call BuildPackage,luci-app-udx))

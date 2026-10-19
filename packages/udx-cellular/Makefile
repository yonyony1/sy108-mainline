SCAN_DEPS := ../source.mk
SY108_RECIPE_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
include $(TOPDIR)/rules.mk
PKG_NAME:=udx-cellular
PKG_VERSION:=3.1
PKG_RELEASE:=5
PKG_LICENSE:=GPL-2.0-only
SY108_SOURCE_PATHS := packages/udx-cellular components/cpnv/runtime/components/common
include $(SY108_RECIPE_DIR)../source.mk
include $(INCLUDE_DIR)/package.mk

Build/Prepare = $(call SY108/Prepare,$(PKG_BUILD_DIR))

define Package/udx-cellular
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UDX710 native cellular and SMS owner
  DEPENDS:=+udx-cpnv +netifd +ubus +uci +procd +odhcp6c +libjson-c +libpthread
endef

define Package/udx-cellular/conffiles
/etc/config/udx-cellular
/etc/udx-cellular/
endef

define Build/Compile
	CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -Wno-misleading-indentation" \
	 CPPFLAGS="$(TARGET_CPPFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
	 JSON_INCLUDE="$(STAGING_DIR)/usr/include" \
	 sh $(PKG_BUILD_DIR)/packages/udx-cellular/build-native.sh $(PKG_BUILD_DIR)/bin -ljson-c
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 \
	 $(PKG_BUILD_DIR)/packages/udx-cellular/src/netifd.c $(TARGET_LDFLAGS) -ljson-c -o $(PKG_BUILD_DIR)/bin/udx-netifd
endef

define Package/udx-cellular/install
	$(INSTALL_DIR) $(1)
	$(CP) $(PKG_BUILD_DIR)/packages/udx-cellular/files/. $(1)/
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/udx-cellulard $(PKG_BUILD_DIR)/bin/udx-cellular-control $(PKG_BUILD_DIR)/bin/udx-system-quiesce $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/udx-netifd $(1)/usr/libexec/
endef
$(eval $(call BuildPackage,udx-cellular))

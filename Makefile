include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=uwe5622
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

MAC80211_DIR:=$(KERNEL_BUILD_DIR)/mac80211-regular/backports-7.2
MAC80211_SYMVERS:=$(KERNEL_BUILD_DIR)/symvers/mac80211.symvers

define KernelPackage/uwe5622-bsp
  SUBMENU:=Wireless Drivers
  TITLE:=Unisoc UWE5622 SDIO BSP driver
  DEPENDS:=+kmod-mmc +kmod-cfg80211 +kmod-rfkill +kmod-hci-uart +bluez-utils
  FILES:= \
	$(PKG_BUILD_DIR)/unisocwcn/uwe5622_bsp_sdio.ko \
	$(PKG_BUILD_DIR)/unisocwifi/sprdwl_ng.ko \
	$(PKG_BUILD_DIR)/tty-sdio/sprdbt_tty.ko

  AUTOLOAD:=$(call AutoProbe,uwe5622_bsp_sdio sprdwl_ng sprdbt_tty)
endef

define KernelPackage/uwe5622-bsp/description
 Unisoc UWE5622 SDIO BSP driver for Allwinner platforms.
endef

define Build/Prepare
	$(RM) -r $(PKG_BUILD_DIR)
	$(CP) ./src $(PKG_BUILD_DIR)
endef

define Build/Compile
	# Common WCN/BSP
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)/unisocwcn" \
		CONFIG_AW_WIFI_DEVICE_UWE5622=y \
		UNISOC_FW_PATH_CONFIG="/lib/firmware/" \
		modules

	# WiFi
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)/unisocwifi" \
		UNISOC_BSP_INCLUDE="$(PKG_BUILD_DIR)/unisocwcn/include" \
		BACKPORT_DIR="$(MAC80211_DIR)" \
		KBUILD_EXTRA_SYMBOLS="$(PKG_BUILD_DIR)/unisocwcn/Module.symvers $(MAC80211_SYMVERS)" \
		modules

	# Bluetooth TTY
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)/tty-sdio" \
		CONFIG_TTY_OVERY_SDIO=m \
		UNISOC_BSP_INCLUDE="$(PKG_BUILD_DIR)/unisocwcn/include" \
		KBUILD_EXTRA_SYMBOLS="$(PKG_BUILD_DIR)/unisocwcn/Module.symvers" \
		modules
endef

define KernelPackage/uwe5622-bsp/install
	$(INSTALL_DIR) $(1)/lib/firmware

	$(INSTALL_DATA) $(PKG_BUILD_DIR)/unisocwcn/fw/wcnmodem.bin \
		$(1)/lib/firmware/wcnmodem.bin

	$(INSTALL_DATA) $(PKG_BUILD_DIR)/unisocwcn/fw/wifi_2355b001_1ant.ini \
		$(1)/lib/firmware/wifi_2355b001_1ant.ini

	$(INSTALL_DATA) $(PKG_BUILD_DIR)/unisocwcn/fw/bt_configure_pskey.ini \
		$(1)/lib/firmware/bt_configure_pskey.ini

	$(INSTALL_DATA) $(PKG_BUILD_DIR)/unisocwcn/fw/bt_configure_rf.ini \
		$(1)/lib/firmware/bt_configure_rf.ini

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/uwe5622-bt.init $(1)/etc/init.d/uwe5622-bt

endef

$(eval $(call KernelPackage,uwe5622-bsp))

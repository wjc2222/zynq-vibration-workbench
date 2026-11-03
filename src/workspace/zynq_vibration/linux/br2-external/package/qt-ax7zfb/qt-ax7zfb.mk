################################################################################
# Independently named, explicitly selected QPA plugin; no Qt source mutation.
################################################################################
QT_AX7ZFB_VERSION = 0.1.0
QT_AX7ZFB_SITE = $(BR2_EXTERNAL_AX7Z020_PATH)/../qt-ax7zfb
QT_AX7ZFB_SITE_METHOD = local
QT_AX7ZFB_LICENSE = LGPL-3.0
QT_AX7ZFB_DEPENDENCIES = qt5base host-python3

define QT_AX7ZFB_CONFIGURE_CMDS
	$(HOST_DIR)/bin/python3 $(@D)/prepare.py $(QT5BASE_DIR) $(@D)/prepared --refresh
	cd $(@D)/prepared && $(TARGET_MAKE_ENV) $(QT5_QMAKE) ax7zfb.pro
endef

define QT_AX7ZFB_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/prepared
endef

define QT_AX7ZFB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/prepared/libqax7zfb.so \
		$(TARGET_DIR)/usr/lib/qt/plugins/platforms/libqax7zfb.so
	$(INSTALL) -D -m 0644 $(@D)/prepared/source-manifest.json \
		$(TARGET_DIR)/usr/share/qt-ax7zfb/source-manifest.json
endef

$(eval $(generic-package))

################################################################################
# Project-owned source; no third-party acquisition code is embedded.
################################################################################
VIBRATION_OFFLINE_VERSION = 0.1.0
VIBRATION_OFFLINE_SITE = $(BR2_EXTERNAL_AX7Z020_PATH)/../..
VIBRATION_OFFLINE_SITE_METHOD = local
VIBRATION_OFFLINE_LICENSE = MIT
# Buildroot legal-info ignores OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS. Its default
# archive would copy the entire project (including an active WSL VHD). The
# wrapper archives only CMakeLists.txt/software via archive-app-source.py.
# This disables the overbroad automatic archive, not MIT redistribution rights.
VIBRATION_OFFLINE_REDISTRIBUTE = NO
VIBRATION_OFFLINE_SUPPORTS_IN_SOURCE_BUILD = NO
VIBRATION_OFFLINE_CONF_OPTS = -DBUILD_TESTING=OFF

ifeq ($(BR2_PACKAGE_VIBRATION_OFFLINE_GUI),y)
VIBRATION_OFFLINE_DEPENDENCIES += qt5base
VIBRATION_OFFLINE_CONF_OPTS += -DVIBRATION_BUILD_GUI=ON
else
VIBRATION_OFFLINE_CONF_OPTS += -DVIBRATION_BUILD_GUI=OFF
endif

# Only CMake and software sources are needed. Never copy host toolchains/data.
VIBRATION_OFFLINE_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--include=/CMakeLists.txt --include=/software/*** --exclude=*

$(eval $(cmake-package))

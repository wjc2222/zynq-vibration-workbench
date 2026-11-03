# Built only for the matching PS+display platform; no hardware access at build time.
AX7Z020_DISPLAY_TOOLS_VERSION = 0.1.0
AX7Z020_DISPLAY_TOOLS_SITE = $(BR2_EXTERNAL_AX7Z020_PATH)/../../hardware/display/tools
AX7Z020_DISPLAY_TOOLS_SITE_METHOD = local
AX7Z020_DISPLAY_TOOLS_LICENSE = GPL-2.0-only, MIT
AX7Z020_DISPLAY_TOOLS_LICENSE_FILES = fb_scanout_test.c uinput_touch_probe.c

define AX7Z020_DISPLAY_TOOLS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/fb_scanout_test.c -o $(@D)/fb_scanout_test $(TARGET_LDFLAGS)
	$(TARGET_CC) $(TARGET_CFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		$(@D)/uinput_touch_probe.c -o $(@D)/uinput_touch_probe $(TARGET_LDFLAGS)
endef

define AX7Z020_DISPLAY_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fb_scanout_test $(TARGET_DIR)/usr/bin/fb_scanout_test
	$(INSTALL) -D -m 0755 $(@D)/uinput_touch_probe $(TARGET_DIR)/usr/bin/uinput_touch_probe
endef

$(eval $(generic-package))

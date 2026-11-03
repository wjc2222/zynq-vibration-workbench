include $(sort $(wildcard $(BR2_EXTERNAL_AX7Z020_PATH)/package/*/*.mk))

define AX7Z020_INSTALL_NATIVE_UBOOT_ENV
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_AX7Z020_PATH)/board/ax7z020/uboot.env $(@D)/board/xilinx/zynq/ax7z020.env
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_AX7Z020_PATH)/board/ax7z020/zynq-ax7z020.h $(@D)/include/configs/zynq-ax7z020.h
endef
UBOOT_POST_PATCH_HOOKS += AX7Z020_INSTALL_NATIVE_UBOOT_ENV
UBOOT_PRE_CONFIGURE_HOOKS += AX7Z020_INSTALL_NATIVE_UBOOT_ENV

ifeq ($(BR2_AX7Z020_DISPLAY_PLATFORM),y)
define AX7Z020_INJECT_DISPLAY_PLATFORM
	python3 $(BR2_EXTERNAL_AX7Z020_PATH)/../scripts/inject-display.py $(@D)
endef
LINUX_POST_PATCH_HOOKS += AX7Z020_INJECT_DISPLAY_PLATFORM
# Reconfiguration also refreshes a reviewed source lock after a driver update.
LINUX_PRE_CONFIGURE_HOOKS += AX7Z020_INJECT_DISPLAY_PLATFORM
endif

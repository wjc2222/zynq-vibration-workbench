#!/usr/bin/env python3
"""Fail if Kconfig silently discarded a required board/profile choice."""
import pathlib
import sys

config = pathlib.Path(sys.argv[1]).read_text()
profile = sys.argv[2]
required = {
    "BR2_arm": "y", "BR2_cortex_a9": "y", "BR2_ARM_EABIHF": "y",
    "BR2_TOOLCHAIN_EXTERNAL_BOOTLIN_ARMV7_EABIHF_GLIBC_STABLE": "y",
    "BR2_DOWNLOAD_FORCE_CHECK_HASHES": "y", "BR2_REPRODUCIBLE": "y",
    "BR2_TARGET_ROOTFS_CPIO_GZIP": "y", "BR2_LINUX_KERNEL_ZIMAGE": "y",
    "BR2_TARGET_UBOOT_FORMAT_REMAKE_ELF": "y", "BR2_PACKAGE_VIBRATION_OFFLINE": "y",
    "BR2_TARGET_GENERIC_GETTY_PORT": '"ttyPS0"',
}
if profile in ("qt", "display"):
    required.update({
        "BR2_PACKAGE_QT5BASE_WIDGETS": "y", "BR2_PACKAGE_QT5BASE_LINUXFB": "y",
        "BR2_PACKAGE_WQY_ZENHEI": "y", "BR2_PACKAGE_VIBRATION_OFFLINE_GUI": "y",
    })
if profile == "display":
    required["BR2_AX7Z020_DISPLAY_PLATFORM"] = "y"
    required["BR2_PACKAGE_QT_AX7ZFB"] = "y"
    required["BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV"] = "y"
    required["BR2_PACKAGE_EUDEV"] = "y"
    required["BR2_PACKAGE_HAS_UDEV"] = "y"
    required["BR2_PACKAGE_PROVIDES_UDEV"] = '"eudev"'
for symbol, expected in required.items():
    assert f"{symbol}={expected}" in config.splitlines(), f"Missing required config: {symbol}={expected}"
assert "BR2_TARGET_UBOOT_SPL=y" not in config.splitlines(), "SPL must be disabled; use the board XSA FSBL"
assert "zynq-zed" not in config, "Do not use the ZedBoard DTS"
assert 'BR2_TARGET_UBOOT_DEFAULT_ENV_FILE=""' in config.splitlines(), "Use the native board text environment"
assert "BR2_TARGET_UBOOT_NEEDS_XXD=y" not in config.splitlines(), "Native board environment must not pull host Vim just for xxd"
print(f"Buildroot configuration checks passed: {profile}")

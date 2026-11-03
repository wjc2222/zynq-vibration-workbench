# Staging recipe only. Not run by default, and does not write the card.
# Requires a separate reviewed SD deployment with this project's FSBL BOOT.bin.
if fatload mmc 0:1 ${kernel_addr_r} zImage; then
    if fatload mmc 0:1 ${fdt_addr_r} zynq-ax7z020.dtb; then
        if fatload mmc 0:1 ${ramdisk_addr_r} rootfs.cpio.gz; then
            setenv ramdisk_size ${filesize}
            setenv bootargs 'console=ttyPS0,115200 earlycon rdinit=/sbin/init'
            run ramboot
        fi
    fi
fi

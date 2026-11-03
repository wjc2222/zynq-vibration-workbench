# AX7Z020 volatile RAM boot; only the main operator executes this against hardware.
# xsdb jtag-ram-uboot.tcl <hardware-dir> <images-dir> [ps7|fsbl] [server-url] [--bit filename]
# Does not write SD, QSPI or NAND. A system reset interrupts current execution.
if {$argc < 2} {
    error "Usage: xsdb jtag-ram-uboot.tcl hardware-dir images-dir ?ps7|fsbl? ?server-url? ?--bit filename?"
}
set hardware [file normalize [lindex $argv 0]]
set images [file normalize [lindex $argv 1]]
set bit_filename ax7z020_ps_base.bit
set positional {}
for {set i 2} {$i < $argc} {incr i} {
    set arg [lindex $argv $i]
    if {$arg eq "--bit"} {
        incr i
        if {$i >= $argc} {error "--bit requires a filename within hardware-dir"}
        set bit_filename [lindex $argv $i]
        if {[file tail $bit_filename] ne $bit_filename || $bit_filename in {. ..}} {
            error "--bit must name a file within hardware-dir"
        }
    } elseif {[string match --* $arg]} {
        error "Unknown option: $arg"
    } else {
        lappend positional $arg
    }
}
if {[llength $positional] > 2} {error "Too many positional arguments"}
set method ps7
if {[llength $positional] >= 1} {set method [lindex $positional 0]}
if {$method ni {ps7 fsbl}} {error "Initialization method must be ps7 or fsbl"}
set server_url tcp:127.0.0.1:3121
if {[llength $positional] >= 2} {set server_url [lindex $positional 1]}
set required_hardware [list $bit_filename ps7_init.tcl]
if {$method eq "fsbl"} {lappend required_hardware fsbl.elf}
foreach filename $required_hardware {
    if {![file isfile [file join $hardware $filename]]} {error "Missing hardware input: $filename"}
}
puts "Selected hardware bit: [file join $hardware $bit_filename]"
puts "Selected PS initializer: [file join $hardware ps7_init.tcl]"
foreach filename {u-boot.elf zImage zynq-ax7z020.dtb rootfs.cpio.gz SHA256SUMS artifact-verification.json} {
    if {![file isfile [file join $images $filename]]} {error "Missing verified image: $filename"}
}
set ramdisk_bytes [file size [file join $images rootfs.cpio.gz]]
if {$ramdisk_bytes >= 0x10000000} {error "Rootfs exceeds reserved RAM load range"}
proc stop_cpu0_checked {} {
    if {[catch {stop} why] && ![string match -nocase "*already stopped*" $why]} {error $why}
}
proc resume_cpu1_checked {} {
    targets -set -filter {name == "ARM Cortex-A9 MPCore #1"}
    # System reset returns it to the reset/BootROM context. Resume a debug halt;
    # never install a smoke image or leave this core debugger-stopped for Linux.
    if {[catch {con} why] && ![string match -nocase "*already running*" $why]} {
        error "Cannot release CPU1 debugger halt after system reset: $why"
    }
    puts "CPU1 after resume:"
    puts [targets -filter {name == "ARM Cortex-A9 MPCore #1"}]
}
connect -url $server_url
puts "Targets before reset:"
puts [targets]
targets -set -filter {name =~ "APU*"}
rst -system -stop
after 1000
puts "Targets after system reset:"
puts [targets]
resume_cpu1_checked
targets -set -filter {name =~ "xc7z020*"}
fpga -file [file join $hardware $bit_filename]
targets -set -filter {name == "ARM Cortex-A9 MPCore #0"}
stop_cpu0_checked
rst -processor
after 300
stop_cpu0_checked
scan [mrd -value 0xf800025c] %x boot_mode_raw
puts [format "BOOT_MODE raw=0x%08x, mode=%d; initialization=%s" $boot_mode_raw [expr {$boot_mode_raw & 7}] $method]
if {$method eq "fsbl"} {
    if {($boot_mode_raw & 7) != 0} {
        error "FSBL JTAG handoff requires physical JTAG boot mode; use the matching XSA ps7 method for this RAM-only test"
    }
    dow [file join $hardware fsbl.elf]
    con
    after 3000
    stop_cpu0_checked
    # No breakpoint is left installed on either core. Serial boot is the test.
} else {
    source [file join $hardware ps7_init.tcl]
    ps7_init
    ps7_post_config
}
resume_cpu1_checked
targets -set -filter {name == "ARM Cortex-A9 MPCore #0"}
stop_cpu0_checked
dow [file join $images u-boot.elf]
dow -data [file join $images zynq-ax7z020.dtb] 0x01f00000
dow -data [file join $images zImage] 0x02000000
dow -data [file join $images rootfs.cpio.gz] 0x10000000
puts "Targets before U-Boot execution (CPU1 must not remain debug-stopped):"
puts [targets]
con
puts "U-Boot started in RAM. Wait for COM5 115200-8N1 prompt, then execute:"
puts [format "setenv ramdisk_size 0x%x" $ramdisk_bytes]
puts "run ramboot"
puts "Success requires UART Linux boot log and /sys/devices/system/cpu/online = 0-1."
puts "JTAG download or a running CPU0 alone is not a Linux/SMP pass."

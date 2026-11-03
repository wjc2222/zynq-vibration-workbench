# New fixed PS+capture+framebuffer platform. Never connects to hardware.
# vivado -mode batch -source create_combined.tcl -tclargs NEW_ASCII_BUILD
set root [file normalize [file join [file dirname [info script]] ..]]
set display_root [file normalize [file join $root .. display]]
if {[llength $argv]<1 || [llength $argv]>2} {error "Usage: create_combined.tcl new_ascii_build ?--sg?"}
set use_sg 0
if {[llength $argv]==2} {
    if {[lindex $argv 1] ne "--sg"} {error "Only --sg is supported as the optional argument"}
    set use_sg 1
}
set build [file normalize [lindex $argv 0]]
if {[regexp {[^\x00-\x7f]} $build]} {error "ASCII build path required"}
if {[file exists $build]} {error "Refusing to overwrite a build"}
if {![string match "2025.2*" [version -short]]} {error "Vivado 2025.2 required"}
source [file join $display_root board ps_config_base.tcl]
file mkdir $build
create_project ax7z020_combined [file join $build vivado] -part $ax7z020_part
set_property target_language Verilog [current_project]
set_property simulator_language Mixed [current_project]
add_files -norecurse [glob [file join $root rtl *.v]]
add_files -norecurse [glob [file join $display_root rtl *.v]]
add_files -norecurse [glob [file join $display_root vendor rgb2dvi src *.vhd]]
add_files -fileset constrs_1 -norecurse [file join $root constraints ax7z020_combined.xdc]
set analysis_root [file normalize [file join $root .. analysis_v6]]
add_files -norecurse [glob [file join $analysis_root rtl *.sv]]
add_files -norecurse [glob [file join $analysis_root rtl *.v]]
add_files -norecurse [glob [file join $analysis_root data *.mem]]
read_ip [file normalize [file join $root .. .. fft_ip fft_v6.srcs sources_1 ip detection_fft_8192 detection_fft_8192.xci]]
update_compile_order -fileset sources_1
create_bd_design combined
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7]
set_property -dict $ax7z020_ps_config $ps
set_property -dict [list CONFIG.PCW_USE_M_AXI_GP0 {1} CONFIG.PCW_USE_S_AXI_HP0 {1} \
    CONFIG.PCW_S_AXI_HP0_DATA_WIDTH {64} CONFIG.PCW_USE_S_AXI_HP1 {1} CONFIG.PCW_S_AXI_HP1_DATA_WIDTH {64} CONFIG.PCW_EN_CLK0_PORT {1} \
    CONFIG.PCW_EN_RST0_PORT {1} CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100.000000} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} CONFIG.PCW_IRQ_F2P_INTR {1}] $ps
make_bd_intf_pins_external [get_bd_intf_pins ps7/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7/FIXED_IO]

proc ip {name vlnv config} {
    set cell [create_bd_cell -type ip -vlnv $vlnv $name]
    if {[llength $config]} {set_property -dict $config $cell}
    return $cell
}
proc wire {from args} {
    set pins [list [get_bd_pins $from]]
    foreach name $args {lappend pins [get_bd_pins $name]}
    connect_bd_net {*}$pins
}
proc reset_cell {name clock locked hold} {
    ip $name xilinx.com:ip:proc_sys_reset:5.0 [list CONFIG.C_AUX_RESET_HIGH {1}]
    # C_EXT_RESET_HIGH is propagate-only in Vivado 2025.2. Explicitly set
    # the input reset polarity, then assert the derived parameter below.
    set_property CONFIG.POLARITY ACTIVE_LOW [get_bd_pins $name/ext_reset_in]
    wire $clock $name/slowest_sync_clk
    wire ps7/FCLK_RESET0_N $name/ext_reset_in
    wire $locked $name/dcm_locked
    wire $hold $name/aux_reset_in
    wire zero/dout $name/mb_debug_sys_rst
}
ip one xilinx.com:ip:xlconstant:1.1 [list CONFIG.CONST_WIDTH {1} CONFIG.CONST_VAL {1}]
ip zero xilinx.com:ip:xlconstant:1.1 [list CONFIG.CONST_WIDTH {1} CONFIG.CONST_VAL {0}]
create_bd_cell -type module -reference combined_clocks clocks
create_bd_cell -type module -reference display_phy phy
create_bd_cell -type module -reference display_status status_monitor
set clkport [create_bd_port -dir I -type clk -freq_hz 50000000 clk50]
connect_bd_net $clkport [get_bd_pins clocks/clk50]
reset_cell axi_reset ps7/FCLK_CLK0 one/dout zero/dout
reset_cell pixel_reset clocks/pixel_clk clocks/locked zero/dout
reset_cell video_axi_reset ps7/FCLK_CLK0 one/dout status_monitor/video_hold
reset_cell video_pixel_reset clocks/pixel_clk clocks/locked status_monitor/video_hold

ip vdma xilinx.com:ip:axi_vdma:6.3 [list \
    CONFIG.c_include_mm2s {1} CONFIG.c_include_s2mm {0} CONFIG.c_include_sg {0} \
    CONFIG.c_m_axi_mm2s_data_width {64} CONFIG.c_m_axis_mm2s_tdata_width {32} \
    CONFIG.c_include_mm2s_dre {0} CONFIG.c_num_fstores {2} \
    CONFIG.c_mm2s_max_burst_length {16} CONFIG.c_mm2s_linebuffer_depth {4096} \
    CONFIG.c_use_fsync {0} \
    CONFIG.c_include_internal_genlock {0} CONFIG.c_mm2s_genlock_mode {0} \
    CONFIG.c_enable_vidprmtr_reads {1} CONFIG.c_enable_mm2s_frmstr_reg {1}]
ip subset xilinx.com:ip:axis_subset_converter:1.1 [list \
    CONFIG.S_TDATA_NUM_BYTES {4} CONFIG.M_TDATA_NUM_BYTES {3} \
    CONFIG.S_TUSER_WIDTH {1} CONFIG.M_TUSER_WIDTH {1} \
    CONFIG.S_HAS_TLAST {1} CONFIG.M_HAS_TLAST {1} CONFIG.S_HAS_TKEEP {1} CONFIG.M_HAS_TKEEP {0} \
    CONFIG.TDATA_REMAP {tdata[23:0]} CONFIG.TUSER_REMAP {tuser[0:0]} CONFIG.TLAST_REMAP {tlast[0:0]}]
ip video_out xilinx.com:ip:v_axi4s_vid_out:4.0 [list \
    CONFIG.C_S_AXIS_VIDEO_FORMAT {2} CONFIG.C_S_AXIS_VIDEO_DATA_WIDTH {8} \
    CONFIG.C_NATIVE_COMPONENT_WIDTH {8} CONFIG.C_PIXELS_PER_CLOCK {1} \
    CONFIG.C_HAS_ASYNC_CLK {1} CONFIG.C_ADDR_WIDTH {12} \
    CONFIG.C_HYSTERESIS_LEVEL {128} CONFIG.C_VTG_MASTER_SLAVE {1}]
# VTC 6.2 legacy normal timing interface (arbitrary-resolution mode disabled).
# Master mode follows PG044's With-VDMA example: generator runs continuously,
# Video Out applies AXIS backpressure. No vtg_ce -> gen_clken feedback loop.
ip vtc xilinx.com:ip:v_tc:6.2 [list CONFIG.HAS_AXI4_LITE {true} CONFIG.ARBITRARY_RES_EN {false} \
    CONFIG.enable_detection {false} CONFIG.enable_generation {true} \
    CONFIG.VIDEO_MODE {Custom} CONFIG.GEN_HACTIVE_SIZE {1280} \
    CONFIG.GEN_HFRAME_SIZE {1650} CONFIG.GEN_HSYNC_START {1390} CONFIG.GEN_HSYNC_END {1430} \
    CONFIG.GEN_VACTIVE_SIZE {720} CONFIG.GEN_F0_VFRAME_SIZE {750} \
    CONFIG.GEN_F0_VSYNC_VSTART {724} CONFIG.GEN_F0_VSYNC_VEND {729} \
    CONFIG.GEN_F0_VSYNC_HSTART {1390} CONFIG.GEN_F0_VSYNC_HEND {1390} \
    CONFIG.GEN_F0_VBLANK_HSTART {1280} CONFIG.GEN_F0_VBLANK_HEND {1280} \
    CONFIG.GEN_F1_VFRAME_SIZE {750} CONFIG.GEN_F1_VSYNC_VSTART {724} CONFIG.GEN_F1_VSYNC_VEND {729} \
    CONFIG.GEN_F1_VSYNC_HSTART {1390} CONFIG.GEN_F1_VSYNC_HEND {1390} \
    CONFIG.GEN_F1_VBLANK_HSTART {1280} CONFIG.GEN_F1_VBLANK_HEND {1280} \
    CONFIG.GEN_HSYNC_POLARITY {High} CONFIG.GEN_VSYNC_POLARITY {High}]
ip status_gpio xilinx.com:ip:axi_gpio:2.0 [list CONFIG.C_GPIO_WIDTH {32} \
    CONFIG.C_ALL_INPUTS {1} CONFIG.C_IS_DUAL {1} CONFIG.C_GPIO2_WIDTH {32} \
    CONFIG.C_ALL_OUTPUTS_2 {1} CONFIG.C_DOUT_DEFAULT_2 {0x00000001}]
ip control_bus xilinx.com:ip:axi_interconnect:2.1 [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {5}]
ip memory_bus xilinx.com:ip:axi_interconnect:2.1 [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}]
connect_bd_intf_net [get_bd_intf_pins ps7/M_AXI_GP0] [get_bd_intf_pins control_bus/S00_AXI]
foreach {mi target} {M00_AXI vdma/S_AXI_LITE M01_AXI vtc/ctrl M02_AXI status_gpio/S_AXI} {
    connect_bd_intf_net [get_bd_intf_pins control_bus/$mi] [get_bd_intf_pins $target]
}
connect_bd_intf_net [get_bd_intf_pins vdma/M_AXI_MM2S] [get_bd_intf_pins memory_bus/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins memory_bus/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP0]
connect_bd_intf_net [get_bd_intf_pins vdma/M_AXIS_MM2S] [get_bd_intf_pins subset/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins subset/M_AXIS] [get_bd_intf_pins video_out/video_in]
foreach pin {ps7/M_AXI_GP0_ACLK ps7/S_AXI_HP0_ACLK vdma/s_axi_lite_aclk vdma/m_axi_mm2s_aclk vdma/m_axis_mm2s_aclk subset/aclk video_out/aclk vtc/s_axi_aclk status_gpio/s_axi_aclk status_monitor/axi_clk control_bus/ACLK control_bus/S00_ACLK control_bus/M00_ACLK control_bus/M01_ACLK control_bus/M02_ACLK memory_bus/ACLK memory_bus/S00_ACLK memory_bus/M00_ACLK} {
    wire ps7/FCLK_CLK0 $pin
}
foreach pin {vdma/axi_resetn subset/aresetn vtc/s_axi_aresetn status_gpio/s_axi_aresetn status_monitor/axi_resetn control_bus/ARESETN control_bus/S00_ARESETN control_bus/M00_ARESETN control_bus/M01_ARESETN control_bus/M02_ARESETN memory_bus/ARESETN memory_bus/S00_ARESETN memory_bus/M00_ARESETN} {
    wire axi_reset/peripheral_aresetn $pin
}
wire video_axi_reset/peripheral_aresetn video_out/aresetn
wire clocks/pixel_clk video_out/vid_io_out_clk vtc/clk phy/pixel_clk status_monitor/pixel_clk
wire clocks/serial_clk phy/serial_clk
wire video_pixel_reset/peripheral_reset video_out/vid_io_out_reset phy/pixel_reset
wire video_pixel_reset/peripheral_aresetn vtc/resetn
wire pixel_reset/peripheral_aresetn status_monitor/pixel_resetn
wire clocks/locked status_monitor/mmcm_locked
wire one/dout video_out/aclken video_out/vid_io_out_ce vtc/clken vtc/gen_clken vtc/s_axi_aclken
wire zero/dout video_out/fid
wire zero/dout vtc/sof_state vtc/fsync_in
foreach {from to} {hsync_out vtg_hsync vsync_out vtg_vsync hblank_out vtg_hblank vblank_out vtg_vblank active_video_out vtg_active_video} {
    wire vtc/$from video_out/$to
}
wire zero/dout video_out/vtg_field_id
wire video_out/vid_data phy/rgb
wire video_out/vid_active_video phy/de
wire video_out/vid_hsync phy/hsync
wire video_out/vid_vsync phy/vsync status_monitor/vsync
wire video_out/locked status_monitor/video_locked
wire video_out/underflow status_monitor/underflow
wire video_out/overflow status_monitor/overflow
wire status_monitor/status status_gpio/gpio_io_i
wire status_gpio/gpio2_io_o status_monitor/control
foreach name {hdmi_clk_p hdmi_clk_n hdmi_data_p hdmi_data_n} {
    set args [list -dir O]
    if {[string match hdmi_data_* $name]} {lappend args -from 2 -to 0}
    set port [create_bd_port {*}$args $name]
    connect_bd_net [get_bd_pins phy/$name] $port
}

# Independent50MHz capture branch. GP0 stays100MHz; crossing is explicit.
ip capture_cdc xilinx.com:ip:axi_clock_converter:2.1 [list CONFIG.ADDR_WIDTH {32} CONFIG.DATA_WIDTH {32} CONFIG.ID_WIDTH {12} CONFIG.PROTOCOL {AXI4LITE} CONFIG.SYNCHRONIZATION_STAGES {3}]
ip capture_control xilinx.com:ip:axi_interconnect:2.1 [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {2}]
ip capture_memory xilinx.com:ip:axi_interconnect:2.1 [list CONFIG.NUM_SI [expr {$use_sg ? 2 : 1}] CONFIG.NUM_MI {1}]
ip capture_dma xilinx.com:ip:axi_dma:7.1 [list CONFIG.c_include_sg $use_sg CONFIG.c_include_mm2s {0} CONFIG.c_include_s2mm {1} CONFIG.c_s_axis_s2mm_tdata_width {64} CONFIG.c_m_axi_s2mm_data_width {64} CONFIG.c_sg_length_width {26} CONFIG.c_s2mm_burst_size {16} CONFIG.c_addr_width {32} CONFIG.c_sg_include_stscntrl_strm {0}]
create_bd_cell -type module -reference capture_ip capture
reset_cell capture_reset clocks/capture_clk50 one/dout zero/dout
connect_bd_intf_net [get_bd_intf_pins control_bus/M03_AXI] [get_bd_intf_pins capture_cdc/S_AXI]
connect_bd_intf_net [get_bd_intf_pins capture_cdc/M_AXI] [get_bd_intf_pins capture_control/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins capture_control/M00_AXI] [get_bd_intf_pins capture_dma/S_AXI_LITE]
set cap_axi [get_bd_intf_pins -of_objects [get_bd_cells capture] -filter {MODE == Slave}]
set cap_axis [get_bd_intf_pins -of_objects [get_bd_cells capture] -filter {MODE == Master}]
connect_bd_intf_net [get_bd_intf_pins capture_control/M01_AXI] $cap_axi
connect_bd_intf_net $cap_axis [get_bd_intf_pins capture_dma/S_AXIS_S2MM]
connect_bd_intf_net [get_bd_intf_pins capture_dma/M_AXI_S2MM] [get_bd_intf_pins capture_memory/S00_AXI]
if {$use_sg} {
    connect_bd_intf_net [get_bd_intf_pins capture_dma/M_AXI_SG] [get_bd_intf_pins capture_memory/S01_AXI]
    wire clocks/capture_clk50 capture_dma/m_axi_sg_aclk capture_memory/S01_ACLK
    wire capture_reset/peripheral_aresetn capture_memory/S01_ARESETN
}
connect_bd_intf_net [get_bd_intf_pins capture_memory/M00_AXI] [get_bd_intf_pins ps7/S_AXI_HP1]
wire ps7/FCLK_CLK0 capture_cdc/s_axi_aclk control_bus/M03_ACLK
wire axi_reset/peripheral_aresetn capture_cdc/s_axi_aresetn control_bus/M03_ARESETN
foreach pin {ps7/S_AXI_HP1_ACLK capture/aclk capture_dma/s_axi_lite_aclk capture_dma/m_axi_s2mm_aclk capture_cdc/m_axi_aclk capture_control/ACLK capture_control/S00_ACLK capture_control/M00_ACLK capture_control/M01_ACLK capture_memory/ACLK capture_memory/S00_ACLK capture_memory/M00_ACLK} {
 wire clocks/capture_clk50 $pin
}
foreach pin {capture/aresetn capture_dma/axi_resetn capture_cdc/m_axi_aresetn capture_control/ARESETN capture_control/S00_ARESETN capture_control/M00_ARESETN capture_control/M01_ARESETN capture_memory/ARESETN capture_memory/S00_ARESETN capture_memory/M00_ARESETN} {
 wire capture_reset/peripheral_aresetn $pin
}
foreach pin {ad_data ad_busy ad_firstdata speed_in ad_reset ad_cs_n ad_rd_n ad_convsta ad_convstb ad_range ad_par_ser ad_stby_n rs485_de} {
 make_bd_pins_external [get_bd_pins capture/$pin]
 set_property name $pin [get_bd_ports ${pin}_0]
}
# The only RS485 output is physical U19 DE tied0; no UART EMIO is enabled.

ip irq_zero xilinx.com:ip:xlconstant:1.1 [list CONFIG.CONST_WIDTH {14} CONFIG.CONST_VAL {0}]
ip irq_concat xilinx.com:ip:xlconcat:2.1 [list CONFIG.NUM_PORTS {3} CONFIG.IN0_WIDTH {1} CONFIG.IN1_WIDTH {1} CONFIG.IN2_WIDTH {14}]
wire vdma/mm2s_introut irq_concat/In0
wire capture_dma/s2mm_introut irq_concat/In1
wire irq_zero/dout irq_concat/In2
wire irq_concat/dout ps7/IRQ_F2P
# Independent replay coprocessor at 100MHz. It has no capture input/output.
create_bd_cell -type module -reference detection_ip detection
connect_bd_intf_net [get_bd_intf_pins control_bus/M04_AXI] [get_bd_intf_pins detection/S_AXI]
wire ps7/FCLK_CLK0 detection/aclk control_bus/M04_ACLK
wire axi_reset/peripheral_aresetn detection/aresetn control_bus/M04_ARESETN
assign_bd_address
set det_seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces ps7/Data] -filter {NAME =~ *detection*}]
set_property -dict [list offset 0x43c20000 range 64K] $det_seg
set_property -dict [list offset 0x43000000 range 64K] [get_bd_addr_segs ps7/Data/SEG_vdma_Reg]
set_property -dict [list offset 0x43c00000 range 64K] [get_bd_addr_segs ps7/Data/SEG_vtc_Reg]
set_property -dict [list offset 0x41200000 range 64K] [get_bd_addr_segs ps7/Data/SEG_status_gpio_Reg]
set_property -dict [list offset 0x40400000 range 64K] [get_bd_addr_segs ps7/Data/SEG_capture_dma_Reg]
set cap_seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces ps7/Data] -filter {NAME =~ *capture* && NAME !~ *dma*}]
set_property -dict [list offset 0x43c10000 range 64K] $cap_seg
validate_bd_design
foreach rst {axi_reset pixel_reset video_axi_reset video_pixel_reset capture_reset} {
    if {[get_property CONFIG.C_EXT_RESET_HIGH [get_bd_cells $rst]] != 0} {
        error "$rst must interpret PS FCLK_RESET0_N as active-low"
    }
}
save_bd_design
set bd [get_files */combined.bd]
set_property synth_checkpoint_mode None $bd
generate_target all $bd
set wrapper [make_wrapper -files $bd -top]
add_files -norecurse $wrapper
set_property top combined_wrapper [current_fileset]
update_compile_order -fileset sources_1
set export [file join $build export]
file mkdir $export
set cfg [open [file join $export block_parameters.txt] w]
foreach cell [get_bd_cells] {
    puts $cfg "CELL $cell VLNV=[get_property -quiet VLNV $cell]"
    foreach property [lsort [list_property $cell]] {
        if {[string match CONFIG.* $property]} {puts $cfg "$property=[get_property $property $cell]"}
    }
}
close $cfg
write_hw_platform -fixed -file [file join $export ax7z020_combined_prebit.xsa]
write_bd_tcl -force [file join $export recreate_combined_bd.tcl]
set fp [open [file join $export address_map.txt] w]
foreach seg [get_bd_addr_segs -of_objects [get_bd_addr_spaces]] {
    puts $fp "$seg OFFSET=[get_property OFFSET $seg] RANGE=[get_property RANGE $seg]"
}
puts $fp {VDMA mm2s_introut -> IRQ_F2P[0] -> GIC interrupt61 -> DT <0 29 4>}
puts $fp {Capture DMA s2mm_introut -> IRQ_F2P[1] -> GIC interrupt62 -> DT <0 30 4>}
puts $fp {Capture clock = U18 buffered50MHz; display AXI = FCLK0 100MHz; explicit AXI-Lite CDC; DMA capture HP1 and display HP0}
puts $fp "Capture DMA scatter-gather enabled=$use_sg; descriptor and payload masters share capture HP1"
close $fp
proc find_named {dir name} {
    set found {}
    foreach item [glob -nocomplain -directory $dir *] {
        if {[file isdirectory $item]} {set found [concat $found [find_named $item $name]]} elseif {[file tail $item] eq $name} {lappend found $item}
    }
    return $found
}
foreach name {ps7_init.tcl ps7_init.c ps7_init.h} {
    set paths [find_named [file join $build vivado ax7z020_combined.gen] $name]
    if {[llength $paths]!=1} {error "Expected exactly one new combined $name: $paths"}
    file copy [lindex $paths 0] [file join $export $name]
}
close_project
puts "PASS: combined BD validated and pre-bit XSA exported; implementation and runtime still pending"




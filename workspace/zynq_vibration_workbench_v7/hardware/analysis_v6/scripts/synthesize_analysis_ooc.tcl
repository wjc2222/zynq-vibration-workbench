# Synthesis-only comparison. FFT stays an explicit vendor black box; no route,
# bitstream, hardware connection or baseline source mutation occurs.
# Args: NEW_WORK ANALYSIS_SOURCE VENDOR_FFT_STUB SYMMETRY(0|1)
set work [file normalize [lindex $argv 0]]
set src [file normalize [lindex $argv 1]]
set fft_stub [file normalize [lindex $argv 2]]
set symmetry [lindex $argv 3]
if {$symmetry ne "0" && $symmetry ne "1"} {error "SYMMETRY must be 0 or 1"}
file mkdir $work
cd $work
set_param general.maxThreads 4
create_project analysis_ooc [file join $work project] -part xc7z020clg400-2
read_verilog -sv [file join $src rtl detection_accel.sv]
read_verilog $fft_stub
foreach f [glob [file join $src data *.mem]] {file copy -force $f $work; add_files -norecurse [file join $work [file tail $f]]}
set xdc [open [file join $work analysis_clock.xdc] w]
puts $xdc {create_clock -period 10.000 -name analysis_clk [get_ports aclk]}
close $xdc
read_xdc [file join $work analysis_clock.xdc]
synth_design -top detection_accel -part xc7z020clg400-2 -mode out_of_context -generic FIR_USE_SYMMETRY=$symmetry
report_utilization -hierarchical -file [file join $work utilization.txt]
report_ram_utilization -include_lutram -file [file join $work ram_utilization.txt]
report_timing_summary -file [file join $work timing_summary.txt]
puts "ANALYSIS_OOC_SYNTH_PASS symmetry=$symmetry scope=SYNTH_ONLY_FFT_BLACKBOX"
close_project
exit

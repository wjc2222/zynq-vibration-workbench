# Opens only a separately created display project, never hardware manager.
if {[llength $argv]!=1} {error "Usage: build_display.tcl display_build"}
set build [file normalize [lindex $argv 0]]
open_project [file join $build vivado ax7z020_combined.xpr]
set_param general.maxThreads 2
launch_runs synth_1 -jobs 1
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} {error "Synthesis failed"}
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true [get_runs impl_1]
launch_runs impl_1 -to_step write_bitstream -jobs 1
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {error "Implementation failed"}
open_run impl_1
set export [file join $build export]
set reports [file join $build reports]
file mkdir $reports
report_timing_summary -delay_type min_max -check_timing_verbose -report_unconstrained -file [file join $reports timing_summary.rpt]
report_clocks -file [file join $reports clocks.rpt]
report_clock_utilization -file [file join $reports clock_utilization.rpt]
report_utilization -file [file join $reports utilization.rpt]
report_drc -file [file join $reports drc.rpt]
report_methodology -file [file join $reports methodology.rpt]
report_cdc -file [file join $reports cdc.rpt]
report_io -file [file join $reports io.rpt]
set setup [get_timing_paths -delay_type max -max_paths 1]
set hold [get_timing_paths -delay_type min -max_paths 1]
if {[get_property SLACK $setup]<0 || [get_property SLACK $hold]<0} {error "Negative internal setup/hold slack"}
if {[llength [get_drc_violations -quiet -filter {SEVERITY == Error}]]} {error "DRC errors"}
file copy [file join $build vivado ax7z020_combined.runs impl_1 combined_wrapper.bit] [file join $export ax7z020_combined_720p60.bit]
write_checkpoint [file join $export ax7z020_combined_routed.dcp]
write_hw_platform -fixed -include_bit -file [file join $export ax7z020_combined.xsa]
set fp [open [file join $export BUILD_STATUS.txt] w]
puts $fp "PASS: implemented PS+DDR+capture50MHz+S2MM_HP1+display_HP0, Vivado [version -short]"
puts $fp "Setup slack=[get_property SLACK $setup] ns; hold slack=[get_property SLACK $hold] ns"
puts $fp "No hardware, Linux driver, framebuffer, screen or touch acceptance is implied by this build."
close $fp
close_project


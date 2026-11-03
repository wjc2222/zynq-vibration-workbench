# Export an already completed implementation, with the same strict timing gate.
if {[llength $argv]!=1} {error "Usage: export_implemented.tcl platform_build"}
set build [file normalize [lindex $argv 0]]
open_project [file join $build vivado ax7z020_combined.xpr]
set_param general.maxThreads 2
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {error "Implementation not complete"}
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
report_bus_skew -file [file join $reports bus_skew.rpt]
set setup [get_timing_paths -delay_type max -max_paths 1]
set hold [get_timing_paths -delay_type min -max_paths 1]
if {[get_property SLACK $setup]<0 || [get_property SLACK $hold]<0} {error "Negative internal setup/hold slack"}
if {[llength [get_drc_violations -quiet -filter {SEVERITY == Error}]]} {error "DRC errors"}
file copy -force [file join $build vivado ax7z020_combined.runs impl_1 combined_wrapper.bit] [file join $export ax7z020_combined_720p60.bit]
write_checkpoint -force [file join $export ax7z020_combined_routed.dcp]
write_hw_platform -fixed -include_bit -force -file [file join $export ax7z020_combined.xsa]
set fp [open [file join $export BUILD_STATUS.txt] w]
puts $fp "PASS: complete capture/display plus PS-fed AXIL FIR/FFT coprocessor, Vivado [version -short]"
puts $fp "Setup slack=[get_property SLACK $setup] ns; hold slack=[get_property SLACK $hold] ns"
puts $fp "No board, classifier, live raw tap or HP2 DMA acceptance is implied."
close $fp
close_project
exit

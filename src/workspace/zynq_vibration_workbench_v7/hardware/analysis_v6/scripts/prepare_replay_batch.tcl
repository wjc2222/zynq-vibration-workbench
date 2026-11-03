# Compile/elaborate the actual vendor XFFT model, then exit Vivado. Run the
# snapshot with xsim -runall in the generated xsim directory (no GUI waves).
# Args: ASCII_BUILD ANALYSIS_SOURCE
set base [file normalize [lindex $argv 0]]
set src [file normalize [lindex $argv 1]]
create_project sim_v7 [file join $base sim_project_batch] -part xc7z020clg400-2
read_ip [file join $base fft_ip fft_v6.srcs sources_1 ip detection_fft_8192 detection_fft_8192.xci]
add_files -norecurse [file join $src rtl detection_accel.sv]
add_files -fileset sim_1 -norecurse [file join $src tests tb_detection_accel.sv]
set_property top tb_detection_accel [get_filesets sim_1]
set work [file join $base sim_project_batch sim_v7.sim sim_1 behav xsim]
file mkdir $work
foreach f [concat [glob [file join $base sim *.mem]] [glob [file join $src data *.mem]]] {file copy -force $f $work}
launch_simulation -step compile
launch_simulation -step elaborate
puts "ACTUAL_XFFT_BATCH_READY $work"
close_project
exit

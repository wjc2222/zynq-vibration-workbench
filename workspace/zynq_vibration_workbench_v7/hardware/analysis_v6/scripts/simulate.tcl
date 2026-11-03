set base [file normalize [lindex $argv 0]]
set src [file normalize [lindex $argv 1]]
create_project sim_v6 [file join $base sim_project] -part xc7z020clg400-2
read_ip [file join $base fft_ip fft_v6.srcs sources_1 ip detection_fft_8192 detection_fft_8192.xci]
add_files -norecurse [file join $src rtl detection_accel.sv]
add_files -fileset sim_1 -norecurse [file join $src tests tb_detection_accel.sv]
set_property top tb_detection_accel [get_filesets sim_1]
set_property xsim.simulate.runtime 0ns [get_filesets sim_1]
set work [file join $base sim_project sim_v6.sim sim_1 behav xsim]
file mkdir $work
foreach f [concat [glob [file join $base sim *.mem]] [glob [file join $src data *.mem]]] {file copy -force $f $work}
launch_simulation
run all
if {[get_value -radix unsigned /tb_detection_accel/checked] != 32768 || [get_value -radix unsigned /tb_detection_accel/p] != 4} {
    error "Simulation did not reach all four profiles; simulator process exit alone is not acceptance"
}
close_sim
close_project
exit

set build [file normalize [lindex $argv 0]]
file mkdir $build
create_project fft_v6 $build -part xc7z020clg400-2
create_ip -name xfft -vendor xilinx.com -library ip -version 9.1 -module_name detection_fft_8192
set ip [get_ips detection_fft_8192]
set_property -dict [list CONFIG.channels {1} CONFIG.transform_length {8192} CONFIG.target_clock_frequency {100} CONFIG.implementation_options {radix_2_lite_burst_io} CONFIG.run_time_configurable_transform_length {false} CONFIG.data_format {fixed_point} CONFIG.input_width {24} CONFIG.phase_factor_width {24} CONFIG.scaling_options {block_floating_point} CONFIG.rounding_modes {convergent_rounding} CONFIG.aresetn {true} CONFIG.xk_index {true} CONFIG.throttle_scheme {nonrealtime} CONFIG.output_ordering {bit_reversed_order} CONFIG.memory_options_data {block_ram} CONFIG.memory_options_phase_factors {block_ram} CONFIG.memory_options_reorder {block_ram} CONFIG.complex_mult_type {use_mults_resources} CONFIG.butterfly_type {use_luts}] $ip
generate_target all $ip
create_ip_run $ip
set f [open [file join $build ip_properties.txt] w]
foreach p [lsort [list_property $ip]] {puts $f "$p=[get_property $p $ip]"}
close $f
close_project
exit

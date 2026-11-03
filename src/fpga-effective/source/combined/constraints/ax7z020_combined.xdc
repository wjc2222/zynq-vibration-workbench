# AX7Z020 independent PS+display target. The base platform is untouched.
set_property PACKAGE_PIN U18 [get_ports clk50]
set_property IOSTANDARD LVCMOS33 [get_ports clk50]
create_clock -name board_50mhz -period 20.000 [get_ports clk50]
set_property PACKAGE_PIN U13 [get_ports hdmi_clk_p]
set_property PACKAGE_PIN V13 [get_ports hdmi_clk_n]
set_property PACKAGE_PIN W14 [get_ports {hdmi_data_p[0]}]
set_property PACKAGE_PIN Y14 [get_ports {hdmi_data_n[0]}]
set_property PACKAGE_PIN Y18 [get_ports {hdmi_data_p[1]}]
set_property PACKAGE_PIN Y19 [get_ports {hdmi_data_n[1]}]
set_property PACKAGE_PIN Y16 [get_ports {hdmi_data_p[2]}]
set_property PACKAGE_PIN Y17 [get_ports {hdmi_data_n[2]}]
set_property IOSTANDARD TMDS_33 [get_ports {hdmi_clk_p hdmi_clk_n hdmi_data_p[*] hdmi_data_n[*]}]
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
# Only asynchronous reset pins and explicitly marked first CDC stages.
set_false_path -to [get_pins -hier -filter {REF_PIN_NAME == CLR || REF_PIN_NAME == PRE}]
set_false_path -to [get_pins -hier -filter {NAME =~ *status_monitor*flags_meta_reg*/D || NAME =~ *status_monitor*gray_meta_reg*/D}]
set_false_path -to [get_pins -hier -regexp {.*status_monitor.*/clear_sync_reg\[0\]/D}]

# ADC/SPEED original electrical/timing assignments; shared U18 clock declared above.
set_property PACKAGE_PIN G17 [get_ports {ad_data[0]}]
set_property PACKAGE_PIN A20 [get_ports {ad_data[1]}]
set_property PACKAGE_PIN K19 [get_ports {ad_data[2]}]
set_property PACKAGE_PIN C20 [get_ports {ad_data[3]}]
set_property PACKAGE_PIN B19 [get_ports {ad_data[4]}]
set_property PACKAGE_PIN G20 [get_ports {ad_data[5]}]
set_property PACKAGE_PIN B20 [get_ports {ad_data[6]}]
set_property PACKAGE_PIN M20 [get_ports {ad_data[7]}]
set_property PACKAGE_PIN G19 [get_ports {ad_data[8]}]
set_property PACKAGE_PIN D19 [get_ports {ad_data[9]}]
set_property PACKAGE_PIN M19 [get_ports {ad_data[10]}]
set_property PACKAGE_PIN L19 [get_ports {ad_data[11]}]
set_property PACKAGE_PIN D20 [get_ports {ad_data[12]}]
set_property PACKAGE_PIN F17 [get_ports {ad_data[13]}]
set_property PACKAGE_PIN L20 [get_ports {ad_data[14]}]
set_property PACKAGE_PIN J20 [get_ports {ad_data[15]}]
set_property PACKAGE_PIN F16 [get_ports {ad_convstb}]
set_property PACKAGE_PIN H20 [get_ports {ad_firstdata}]
set_property PACKAGE_PIN G18 [get_ports {ad_cs_n}]
set_property PACKAGE_PIN H16 [get_ports {ad_busy}]
set_property PACKAGE_PIN H17 [get_ports {ad_reset}]
set_property PACKAGE_PIN H15 [get_ports {ad_rd_n}]
set_property PACKAGE_PIN G15 [get_ports {ad_range}]
set_property PACKAGE_PIN K17 [get_ports {ad_convsta}]
set_property PACKAGE_PIN K18 [get_ports {ad_par_ser}]
set_property PACKAGE_PIN K16 [get_ports {ad_stby_n}]
set_property IOSTANDARD LVCMOS33 [get_ports {ad_*}]
set_property DRIVE 8 [get_ports {ad_reset ad_cs_n ad_rd_n ad_convsta ad_convstb ad_range ad_par_ser ad_stby_n}]
set_property SLEW SLOW [get_ports {ad_reset ad_cs_n ad_rd_n ad_convsta ad_convstb ad_range ad_par_ser ad_stby_n}]

# Asynchronous BUSY: false path ONLY to first synchronizer stage.
set_false_path -from [get_ports ad_busy] -to [get_pins -hier -filter {NAME =~ */busy_meta_reg/D}]
# Bundled asynchronous data is sampled 200 ns after registered RD assertion.
# Bound on-chip input/output delays to 10 ns each; see timing budget in docs.
set_max_delay -datapath_only 10.000 -from [get_ports {ad_data[*] ad_firstdata}] -to [all_registers]
set_max_delay -datapath_only 10.000 -to [get_ports {ad_reset ad_cs_n ad_rd_n ad_convsta ad_convstb ad_range ad_par_ser ad_stby_n}]

# Unused user IO: high impedance (adapter shares auxiliary interfaces).
set_property BITSTREAM.CONFIG.UNUSEDPIN Pullnone [current_design]

# SPEED: acquisition J7-34 -> adapter IO2_16P -> J21-34 -> N15.
# Connect only conditioned SPEED, NEVER raw 24 V to the FPGA connector.
set_property PACKAGE_PIN N15 [get_ports speed_in]
set_property IOSTANDARD LVCMOS33 [get_ports speed_in]
set_false_path -from [get_ports speed_in] -to [get_pins -hier -filter {NAME =~ */speed_meta_reg/D}]


# Physical MAX3485 DE is held low; UART1 stays MIO for Linux console.
set_property PACKAGE_PIN U19 [get_ports rs485_de]
set_property IOSTANDARD LVCMOS33 [get_ports rs485_de]
set_property DRIVE 8 [get_ports rs485_de]
set_property SLEW SLOW [get_ports rs485_de]


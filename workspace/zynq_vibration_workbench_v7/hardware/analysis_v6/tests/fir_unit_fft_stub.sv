// FIR-UNIT TEST ONLY. This is not an FFT implementation or FFT evidence.
module detection_fft_8192(
 input aclk,aresetn,input [7:0] s_axis_config_tdata,input s_axis_config_tvalid,
 output s_axis_config_tready,input [47:0] s_axis_data_tdata,input s_axis_data_tvalid,
 output s_axis_data_tready,input s_axis_data_tlast,output [47:0] m_axis_data_tdata,
 output [23:0] m_axis_data_tuser,output m_axis_data_tvalid,input m_axis_data_tready,
 output m_axis_data_tlast,output [7:0] m_axis_status_tdata,output m_axis_status_tvalid,
 input m_axis_status_tready,output event_frame_started,event_tlast_unexpected,
 output event_tlast_missing,event_status_channel_halt,event_data_in_channel_halt,
 output event_data_out_channel_halt
);
 assign s_axis_config_tready=0;assign s_axis_data_tready=0;
 assign m_axis_data_tdata=0;assign m_axis_data_tuser=0;assign m_axis_data_tvalid=0;
 assign m_axis_data_tlast=0;assign m_axis_status_tdata=0;assign m_axis_status_tvalid=0;
 assign event_frame_started=0;assign event_tlast_unexpected=0;assign event_tlast_missing=0;
 assign event_status_channel_halt=0;assign event_data_in_channel_halt=0;assign event_data_out_channel_halt=0;
endmodule

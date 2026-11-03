// V7 replay coprocessor. No dependency or signal path to the capture module.
// All input is PS copied raw signed16; this is deliberately NOT a raw tap.
module detection_accel #(
  parameter COEFF_FILE="fir_all.mem", parameter HANN_FILE="hann_8192.mem",
  // Keep the direct evaluator available for isolated numerical/cycle A/B tests.
  parameter integer FIR_USE_SYMMETRY=1
)(
  (* X_INTERFACE_PARAMETER="ASSOCIATED_BUSIF S_AXI, ASSOCIATED_RESET aresetn, FREQ_HZ 100000000" *)
  input wire aclk, input wire aresetn,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI AWADDR" *) input wire [15:0] s_axi_awaddr,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI AWPROT" *) input wire [2:0] s_axi_awprot,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI AWVALID" *) input wire s_axi_awvalid,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI AWREADY" *) output wire s_axi_awready,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI WDATA" *) input wire [31:0] s_axi_wdata,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI WSTRB" *) input wire [3:0] s_axi_wstrb,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI WVALID" *) input wire s_axi_wvalid,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI WREADY" *) output wire s_axi_wready,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI BRESP" *) output wire [1:0] s_axi_bresp,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI BVALID" *) output reg s_axi_bvalid,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI BREADY" *) input wire s_axi_bready,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI ARADDR" *) input wire [15:0] s_axi_araddr,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI ARPROT" *) input wire [2:0] s_axi_arprot,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI ARVALID" *) input wire s_axi_arvalid,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI ARREADY" *) output wire s_axi_arready,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI RDATA" *) output reg [31:0] s_axi_rdata,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI RRESP" *) output wire [1:0] s_axi_rresp,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI RVALID" *) output reg s_axi_rvalid,
  (* X_INTERFACE_INFO="xilinx.com:interface:aximm:1.0 S_AXI RREADY" *) input wire s_axi_rready
);
localparam IDLE=0, INGEST=1,FIR=2,FIR_SAVE=3,MEAN=4,FFT_CFG=5,FFT_READ=6,FFT_MUL=7,FFT_SEND=8,FFT_DRAIN=9,FFT_NEXT=10,DONE=11,FIR_ROUND=12,FIR_SQUARE=13,FFT_CENTER=14;
reg [3:0] state;
reg [1:0] profile, active_profile, axis;
reg [9:0] taps; reg [3:0] decimation,phase;
reg [31:0] required_raw, raw_count,low_count,errors;
reg [63:0] cycles;
reg [31:0] input12, result_index;
reg [15:0] awaddr_hold; reg aw_hold;
reg [31:0] wdata_hold; reg [3:0] wstrb_hold;reg w_hold;
wire input_space;
assign s_axi_awready=!aw_hold && !s_axi_bvalid;
assign s_axi_wready=!w_hold && !s_axi_bvalid;
assign s_axi_arready=!s_axi_rvalid;
assign s_axi_bresp=0; assign s_axi_rresp=0;
wire do_write=aw_hold&&w_hold&&!s_axi_bvalid&&((awaddr_hold!=16'h14)||input_space||state==IDLE||state==DONE);
wire reset_command=do_write&&awaddr_hold==4&&wdata_hold[1];
wire start_command=do_write&&awaddr_hold==4&&wdata_hold[0]&&!wdata_hold[1]&&(state==IDLE||state==DONE);
wire ack_command=do_write&&awaddr_hold==4&&wdata_hold[2];
// 512-frame FIFO permits batched writes; full FIFO delays AXI B response.
(* ram_style="distributed" *) reg [47:0] input_fifo[0:511];
reg [8:0] fifo_wr,fifo_rd;reg [9:0] fifo_count;
wire input_push=do_write&&awaddr_hold==16'h14&&(state==INGEST||state==FIR||state==FIR_SAVE||state==FIR_ROUND||state==FIR_SQUARE)&&raw_count+fifo_count<required_raw;
assign input_space=(fifo_count<511);
wire input_pop=state==INGEST&&fifo_count!=0;
reg [47:0] fifo_q;
// Circular FIR state, coefficients, and immutable result window.
(* ram_style="block" *) reg signed [15:0] history0[0:1023],history1[0:1023],history2[0:1023];
(* rom_style="block" *) reg signed [23:0] coefficients[0:4095];
(* rom_style="block" *) reg [23:0] hann[0:8191];
(* ram_style="block" *) reg signed [23:0] samples0[0:8191],samples1[0:8191],samples2[0:8191];
(* ram_style="block" *) reg [47:0] powers0[0:4095],powers1[0:4095],powers2[0:4095];
reg [47:0] nyquist[0:2];
initial begin $readmemh(COEFF_FILE,coefficients);$readmemh(HANN_FILE,hann); end
reg [9:0] history_wr,history_head;
reg [10:0] fir_cycle;
reg signed [15:0] hq0,hq1,hq2;
reg signed [15:0] reverse_hq0,reverse_hq1,reverse_hq2;
reg fir_center_q;
reg signed [16:0] pair0,pair1,pair2;
reg signed [23:0] cq,pair_cq;
// signed16 + signed16 needs 17 bits; the signed24 product needs 41.
// Do not truncate the pre-add or round each term: sum first, round once.
reg signed [40:0] prod0,prod1,prod2;
wire [9:0] fir_terms=FIR_USE_SYMMETRY?((taps>>1)+1):taps;
// Explicit TDP mapping: during INGEST port A writes the circular history;
// during FIR that same port reads forward while read-only port B reads back.
// Sharing the A address is essential; three independent addresses infer LUTRAM.
wire [9:0] history_port_a_address=(state==INGEST)?history_wr:(history_head-fir_cycle);
wire [9:0] history_port_b_address=history_head-(taps-1-fir_cycle);
always @(posedge aclk)begin
 if(aresetn&&!reset_command&&!start_command&&input_pop)begin
  history0[history_port_a_address]<=input_fifo[fifo_rd][15:0];
  history1[history_port_a_address]<=input_fifo[fifo_rd][31:16];
  history2[history_port_a_address]<=input_fifo[fifo_rd][47:32];
 end
 hq0<=history0[history_port_a_address];hq1<=history1[history_port_a_address];hq2<=history2[history_port_a_address];
 if(FIR_USE_SYMMETRY)begin
  reverse_hq0<=history0[history_port_b_address];reverse_hq1<=history1[history_port_b_address];reverse_hq2<=history2[history_port_b_address];
 end
end
reg signed [55:0] acc0,acc1,acc2;
reg signed [63:0] sums[0:2]; reg [63:0] sumsq[0:2];
reg signed [23:0] minima[0:2],maxima[0:2];
reg [31:0] rawclip[0:2],saturation[0:2];
reg signed [31:0] means[0:2];
reg [7:0] exponents[0:2];
reg [12:0] band_lo[0:12],band_hi[0:12],locked_lo[0:12],locked_hi[0:12];
reg [63:0] bandpower[0:2][0:17];
reg [63:0] result_value;
reg signed [23:0] read_sample0,read_sample1,read_sample2;
reg [47:0] read_power0,read_power1,read_power2;
reg [12:0] fft_input_index;
reg signed [23:0] fft_sample_q;
reg [23:0] fft_hann_q;
reg signed [49:0] fft_window_product;
reg signed [24:0] fft_centered;
wire [47:0] fft_input;
wire config_ready, fft_input_ready;
wire [47:0] fft_output;
wire [23:0] fft_user;
wire fft_valid,fft_last;wire [7:0] fft_status;wire fft_status_valid;
wire event_unexpected,event_missing;
wire fft_config_valid=state==FFT_CFG;
wire fft_send_valid=state==FFT_SEND;
reg [2:0] fft_reset_hold;
always @(posedge aclk)begin
 if(!aresetn||reset_command)fft_reset_hold<=7;
 else if(fft_reset_hold!=0)fft_reset_hold<=fft_reset_hold-1;
end
detection_fft_8192 fft_core(.aclk(aclk),.aresetn(aresetn&&fft_reset_hold==0),
 .s_axis_config_tdata(8'h01),.s_axis_config_tvalid(fft_config_valid),.s_axis_config_tready(config_ready),
 .s_axis_data_tdata(fft_input),.s_axis_data_tvalid(fft_send_valid),.s_axis_data_tready(fft_input_ready),.s_axis_data_tlast(fft_input_index==8191),
 .m_axis_data_tdata(fft_output),.m_axis_data_tuser(fft_user),.m_axis_data_tvalid(fft_valid),.m_axis_data_tready(1'b1),.m_axis_data_tlast(fft_last),
 .m_axis_status_tdata(fft_status),.m_axis_status_tvalid(fft_status_valid),.m_axis_status_tready(1'b1),
 .event_frame_started(),.event_tlast_unexpected(event_unexpected),.event_tlast_missing(event_missing),
 .event_status_channel_halt(),.event_data_in_channel_halt(),.event_data_out_channel_halt());
// Round any signed value to nearest, ties to even. Arithmetic right shift floors.
function automatic signed [63:0] round_even(input signed [63:0] x,input integer shift);
 reg signed [63:0] q;reg [63:0] remainder,half;
 begin q=x>>>shift;remainder=$unsigned(x)-( $unsigned(q)<<shift);half=64'd1<<(shift-1);
 round_even=q+((remainder>half)||((remainder==half)&&q[0]));end
endfunction
function automatic signed [23:0] sat24(input signed [63:0] x);
 begin if(x>8388607)sat24=24'sh7fffff;else if(x < -8388608)sat24=24'sh800000;else sat24=x[23:0];end
endfunction
wire signed [63:0] rounded0=round_even({{8{acc0[55]}},acc0},16);
wire signed [63:0] rounded1=round_even({{8{acc1[55]}},acc1},16);
wire signed [63:0] rounded2=round_even({{8{acc2[55]}},acc2},16);
reg signed [23:0] filtered0,filtered1,filtered2;
reg signed [47:0] square0,square1,square2;
wire signed [23:0] fft_re=fft_output[23:0],fft_im=fft_output[47:24];
reg [47:0] fft_re2,fft_im2;reg power_valid,power_last;reg [12:0] power_index;
wire [48:0] fft_power={1'b0,fft_re2}+{1'b0,fft_im2};
wire [63:0] weighted_power=(power_index==0||power_index==4096)?{15'b0,fft_power}:({15'b0,fft_power}<<1);
reg union_hit,integer_hit;integer b;
always @* begin
 union_hit=0;integer_hit=0;
 for(integer j=0;j<13;j=j+1) begin
  if(locked_lo[j]<=locked_hi[j]&&power_index>=locked_lo[j]&&power_index<=locked_hi[j])begin union_hit=1;if(j>=6)integer_hit=1;end
 end
end
// Sample index uses linear interleaving (point*3+axis), not bit packing.
reg [12:0] sample_read_point;
reg [1:0] sample_read_axis;
reg [31:0] result_ready_stage0,result_ready_index;
always @(posedge aclk)begin
 sample_read_point<=result_index[14:0]/15'd3;
 sample_read_axis<=result_index[14:0]%15'd3;
 result_ready_stage0<=result_index;
 result_ready_index<=result_ready_stage0;
end
wire [12:0] sample_memory_address=((state>=FFT_CFG&&state<=FFT_DRAIN)||state==FFT_CENTER)?fft_input_index:sample_read_point;
// Read ports are synchronous BRAM. Index is set by an independent AXI write.
always @(posedge aclk) begin
 read_sample0<=samples0[sample_memory_address];read_sample1<=samples1[sample_memory_address];read_sample2<=samples2[sample_memory_address];
 read_power0<=powers0[result_index[11:0]];read_power1<=powers1[result_index[11:0]];read_power2<=powers2[result_index[11:0]];
end
always @* begin
 result_value=0;
 if(result_index<24576)begin
  case(sample_read_axis)
   0:result_value={{40{read_sample0[23]}},read_sample0};
   1:result_value={{40{read_sample1[23]}},read_sample1};
   2:result_value={{40{read_sample2[23]}},read_sample2};
  endcase
 end else if(result_index>=32'h10000&&result_index<32'h10300)begin
  case(result_index[7:0])
   0:result_value={{40{minima[result_index[9:8]][23]}},minima[result_index[9:8]]};
   1:result_value={{40{maxima[result_index[9:8]][23]}},maxima[result_index[9:8]]};
   2:result_value=sums[result_index[9:8]];
   3:result_value=sumsq[result_index[9:8]];
   4:result_value=rawclip[result_index[9:8]];
   5:result_value=exponents[result_index[9:8]];
   6:result_value=1;
   7:result_value=saturation[result_index[9:8]];
  endcase
 end else if(result_index>=32'h20000&&result_index<32'h26000)begin
  if(result_index[12:0]==4096) result_value={16'b0,nyquist[result_index[14:13]]};
  else if(result_index[12:0]<4096)case(result_index[14:13])
   0:result_value={16'b0,read_power0};1:result_value={16'b0,read_power1};2:result_value={16'b0,read_power2};endcase
 end else if(result_index>=32'h30000&&result_index<32'h30060&&result_index[4:0]<18)
  result_value=bandpower[result_index[6:5]][result_index[4:0]];
end
integer i,j;
always @(posedge aclk) begin
 if(!aresetn)begin
  aw_hold<=0;w_hold<=0;s_axi_bvalid<=0;s_axi_rvalid<=0;s_axi_rdata<=0;
  profile<=0;input12<=0;result_index<=0;
  for(i=0;i<13;i=i+1)begin band_lo[i]<=8191;band_hi[i]<=0;end
 end else begin
  if(s_axi_awvalid&&s_axi_awready)begin aw_hold<=1;awaddr_hold<=s_axi_awaddr;end
  if(s_axi_wvalid&&s_axi_wready)begin w_hold<=1;wdata_hold<=s_axi_wdata;wstrb_hold<=s_axi_wstrb;end
  if(s_axi_bvalid&&s_axi_bready)s_axi_bvalid<=0;
  if(do_write)begin
   aw_hold<=0;w_hold<=0;s_axi_bvalid<=1;
   case(awaddr_hold)
    16'h0c:if(state==IDLE||state==DONE)profile<=wdata_hold[1:0];
    16'h10:input12<=wdata_hold;
    16'h20:result_index<=wdata_hold;
   endcase
   if(awaddr_hold>=16'h40&&awaddr_hold<16'h74)begin band_lo[(awaddr_hold-64)>>2]<=wdata_hold[12:0];band_hi[(awaddr_hold-64)>>2]<=wdata_hold[28:16];end
  end
  if(s_axi_rvalid&&s_axi_rready)s_axi_rvalid<=0;
  if(s_axi_arvalid&&s_axi_arready)begin
   s_axi_rvalid<=1;
   case(s_axi_araddr)
    16'h00:s_axi_rdata<=32'h36414e41;
    16'h08:s_axi_rdata<={27'd0,input_space&&(state==INGEST||state==FIR||state==FIR_SAVE||state==FIR_ROUND||state==FIR_SQUARE),errors!=0,state==DONE,state!=IDLE&&state!=DONE,state==IDLE};
    16'h0c:s_axi_rdata<=profile;
    16'h18:s_axi_rdata<=raw_count;
    16'h1c:s_axi_rdata<=low_count;
    16'h20:s_axi_rdata<=result_index;
    16'h24:s_axi_rdata<=result_value[31:0];
    16'h28:s_axi_rdata<=result_value[63:32];
    16'h2c:s_axi_rdata<=32'h00060001;
    16'h30:s_axi_rdata<=cycles[31:0];16'h34:s_axi_rdata<=cycles[63:32];
    16'h38:s_axi_rdata<=errors;
    16'h3c:s_axi_rdata<=required_raw;
    16'h78:s_axi_rdata<=result_ready_index;
    default:s_axi_rdata<=0;
   endcase
  end
 end
end
// Data path. It resets window metadata, not BRAM; warmup overwrites all FIR taps.
always @(posedge aclk) begin
 if(!aresetn||reset_command||start_command)begin
  state<=start_command?INGEST:IDLE;fifo_wr<=0;fifo_rd<=0;fifo_count<=0;
  raw_count<=0;low_count<=0;errors<=0;cycles<=0;history_wr<=0;history_head<=0;
  fir_cycle<=0;phase<=0;axis<=0;power_valid<=0;power_last<=0;fft_input_index<=0;
  active_profile<=profile;
  case(profile)
   0:begin taps<=641;decimation<=10;required_raw<=82551;end
   1:begin taps<=321;decimation<=5;required_raw<=41276;end
   2:begin taps<=129;decimation<=2;required_raw<=16511;end
   3:begin taps<=65;decimation<=1;required_raw<=8256;end
  endcase
  for(i=0;i<3;i=i+1)begin
   sums[i]<=0;sumsq[i]<=0;minima[i]<=24'sh7fffff;maxima[i]<=24'sh800000;rawclip[i]<=0;saturation[i]<=0;exponents[i]<=0;means[i]<=0;nyquist[i]<=0;
   for(j=0;j<18;j=j+1)bandpower[i][j]<=0;
  end
  for(i=0;i<13;i=i+1)begin locked_lo[i]<=band_lo[i];locked_hi[i]<=band_hi[i];end
 end else begin
  if(state!=IDLE&&state!=DONE)cycles<=cycles+1;
  if(ack_command&&state==DONE)state<=IDLE;
  if(input_push)begin input_fifo[fifo_wr]<={wdata_hold[15:0],input12};fifo_wr<=fifo_wr+1;end
  if(input_pop)fifo_rd<=fifo_rd+1;
  case({input_push,input_pop})2'b10:fifo_count<=fifo_count+1;2'b01:fifo_count<=fifo_count-1;default:;endcase
  // FFT complex square has one register stage. Bin identity travels with it.
  power_valid<=fft_valid;power_last<=fft_last&&fft_valid;
  if(fft_valid)begin fft_re2<=fft_re*fft_re;fft_im2<=fft_im*fft_im;power_index<=fft_user[12:0];exponents[axis]<=fft_user[23:16];end
  if(power_valid&&power_index<=4096)begin
   if(power_index<4096)case(axis)0:powers0[power_index[11:0]]<=fft_power[47:0];1:powers1[power_index[11:0]]<=fft_power[47:0];2:powers2[power_index[11:0]]<=fft_power[47:0];endcase
   else nyquist[axis]<=fft_power[47:0];
   for(b=0;b<13;b=b+1)if(locked_lo[b]<=locked_hi[b]&&power_index>=locked_lo[b]&&power_index<=locked_hi[b])bandpower[axis][b]<=bandpower[axis][b]+weighted_power;
   if(power_index>=4&&power_index<=819)begin
    bandpower[axis][13]<=bandpower[axis][13]+weighted_power;
    if(union_hit)bandpower[axis][15]<=bandpower[axis][15]+weighted_power;
    else bandpower[axis][17]<=bandpower[axis][17]+weighted_power;
    if(integer_hit)bandpower[axis][16]<=bandpower[axis][16]+weighted_power;
   end
   if(power_index<=3276)bandpower[axis][14]<=bandpower[axis][14]+weighted_power;
  end
  if(event_unexpected||event_missing)errors<=errors|1;
  case(state)
   INGEST:if(input_pop)begin
    if(input_fifo[fifo_rd][15:0]==16'h8000||input_fifo[fifo_rd][15:0]==16'h7fff)rawclip[0]<=rawclip[0]+1;
    if(input_fifo[fifo_rd][31:16]==16'h8000||input_fifo[fifo_rd][31:16]==16'h7fff)rawclip[1]<=rawclip[1]+1;
    if(input_fifo[fifo_rd][47:32]==16'h8000||input_fifo[fifo_rd][47:32]==16'h7fff)rawclip[2]<=rawclip[2]+1;
    raw_count<=raw_count+1;history_head<=history_wr;history_wr<=history_wr+1;
    if(raw_count>=taps-1)begin
     if(phase==0)begin state<=FIR;fir_cycle<=0;acc0<=0;acc1<=0;acc2<=0;phase<=decimation-1;end
     else phase<=phase-1;
    end
   end
   FIR:begin
    if(fir_cycle<fir_terms)begin
     cq<=coefficients[{active_profile,10'b0}+fir_cycle];
     if(FIR_USE_SYMMETRY)begin
      // Odd-length symmetric filters: evaluate the middle tap once.
      fir_center_q<=fir_cycle==(taps>>1);
     end
    end
    if(FIR_USE_SYMMETRY)begin
     pair0<=$signed({hq0[15],hq0})+(fir_center_q?17'sd0:$signed({reverse_hq0[15],reverse_hq0}));
     pair1<=$signed({hq1[15],hq1})+(fir_center_q?17'sd0:$signed({reverse_hq1[15],reverse_hq1}));
     pair2<=$signed({hq2[15],hq2})+(fir_center_q?17'sd0:$signed({reverse_hq2[15],reverse_hq2}));
     pair_cq<=cq;
     prod0<=pair0*pair_cq;prod1<=pair1*pair_cq;prod2<=pair2*pair_cq;
     if(fir_cycle>=3)begin acc0<=acc0+prod0;acc1<=acc1+prod1;acc2<=acc2+prod2;end
    end else begin
     prod0<=hq0*cq;prod1<=hq1*cq;prod2<=hq2*cq;
     if(fir_cycle>=2)begin acc0<=acc0+prod0;acc1<=acc1+prod1;acc2<=acc2+prod2;end
    end
    fir_cycle<=fir_cycle+1;
    if(fir_cycle==fir_terms+(FIR_USE_SYMMETRY?2:1))state<=FIR_ROUND;
   end
   FIR_ROUND:begin filtered0<=sat24(rounded0);filtered1<=sat24(rounded1);filtered2<=sat24(rounded2);state<=FIR_SQUARE;end
   FIR_SQUARE:begin square0<=filtered0*filtered0;square1<=filtered1*filtered1;square2<=filtered2*filtered2;state<=FIR_SAVE;end
   FIR_SAVE:begin
    samples0[low_count[12:0]]<=filtered0;samples1[low_count[12:0]]<=filtered1;samples2[low_count[12:0]]<=filtered2;
    sums[0]<=sums[0]+filtered0;sums[1]<=sums[1]+filtered1;sums[2]<=sums[2]+filtered2;
    sumsq[0]<=sumsq[0]+square0;sumsq[1]<=sumsq[1]+square1;sumsq[2]<=sumsq[2]+square2;
    if(filtered0<minima[0])minima[0]<=filtered0;if(filtered0>maxima[0])maxima[0]<=filtered0;
    if(filtered1<minima[1])minima[1]<=filtered1;if(filtered1>maxima[1])maxima[1]<=filtered1;
    if(filtered2<minima[2])minima[2]<=filtered2;if(filtered2>maxima[2])maxima[2]<=filtered2;
    if(rounded0>8388607||rounded0 < -8388608)saturation[0]<=saturation[0]+1;
    if(rounded1>8388607||rounded1 < -8388608)saturation[1]<=saturation[1]+1;
    if(rounded2>8388607||rounded2 < -8388608)saturation[2]<=saturation[2]+1;
    low_count<=low_count+1;if(low_count==8191)state<=MEAN;else state<=INGEST;
   end
   MEAN:begin for(i=0;i<3;i=i+1)means[i]<=round_even(sums[i],13);state<=FFT_CFG;end
   FFT_CFG:if(config_ready)begin state<=FFT_READ;fft_input_index<=0;end
   FFT_READ:begin
    fft_hann_q<=hann[fft_input_index];state<=FFT_CENTER;
   end
   FFT_CENTER:begin
    case(axis)
     0:fft_centered<=$signed(read_sample0)-$signed(means[axis]);
     1:fft_centered<=$signed(read_sample1)-$signed(means[axis]);
     2:fft_centered<=$signed(read_sample2)-$signed(means[axis]);
    endcase
    state<=FFT_MUL;
   end
   FFT_MUL:begin fft_window_product<=fft_centered*$signed({1'b0,fft_hann_q});state<=FFT_SEND;end
   FFT_SEND:begin
    // Product is stable during this state; valid must also remain stable under backpressure.
    if(fft_input_ready)begin if(fft_input_index==8191)state<=FFT_DRAIN;else begin fft_input_index<=fft_input_index+1;state<=FFT_READ;end end
   end
   FFT_DRAIN:if(power_last)state<=FFT_NEXT;
   FFT_NEXT:begin if(axis==2)state<=DONE;else begin axis<=axis+1;state<=FFT_CFG;end end
  endcase
 end
end
assign fft_input={24'd0,sat24(round_even({{14{fft_window_product[49]}},fft_window_product},24))};
endmodule

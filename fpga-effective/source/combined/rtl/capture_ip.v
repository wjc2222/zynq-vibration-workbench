`timescale 1ns/1ps
// MRG1-compatible finite registers; STR1 extensions begin at offset0x70.
module capture_ip #(
 parameter integer SAMPLE_RATE_HZ=50000,
 parameter integer SPEED_FILTER_CYCLES=5,
 parameter integer SPEED_TIMEOUT_CYCLES=100000000
)(
 (* X_INTERFACE_PARAMETER="ASSOCIATED_BUSIF s_axi:m_axis, ASSOCIATED_RESET aresetn, FREQ_HZ 50000000" *)
 (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 aclk CLK" *) input wire aclk,
 (* X_INTERFACE_PARAMETER="POLARITY ACTIVE_LOW" *)
 (* X_INTERFACE_INFO="xilinx.com:signal:reset:1.0 aresetn RST" *) input wire aresetn,
 input wire [15:0] ad_data,
 input wire ad_busy,ad_firstdata,speed_in,
 output wire ad_reset,ad_cs_n,ad_rd_n,ad_convsta,ad_convstb,ad_range,ad_par_ser,ad_stby_n,
 output wire rs485_de,
 input wire [7:0] s_axi_awaddr,input wire [2:0] s_axi_awprot,
 input wire s_axi_awvalid,output wire s_axi_awready,
 input wire [31:0] s_axi_wdata,input wire [3:0] s_axi_wstrb,
 input wire s_axi_wvalid,output wire s_axi_wready,
 output wire [1:0] s_axi_bresp,output reg s_axi_bvalid,input wire s_axi_bready,
 input wire [7:0] s_axi_araddr,input wire [2:0] s_axi_arprot,
 input wire s_axi_arvalid,output wire s_axi_arready,
 output reg [31:0] s_axi_rdata,output wire [1:0] s_axi_rresp,
 output reg s_axi_rvalid,input wire s_axi_rready,
 output wire [63:0] m_axis_tdata,output wire [7:0] m_axis_tkeep,
 output wire m_axis_tvalid,m_axis_tlast,input wire m_axis_tready
);
 localparam MAX_RECORDS=2097024;
 wire rst=!aresetn;
 reg cmd_start,cmd_abort,cmd_adc_reset,cmd_stream_stop;
 reg [31:0] target_samples,rate_selector,source_mode,cfg_reject;
 reg stream_mode_cfg;
 reg [63:0] stream_total;
 reg [31:0] stream_chunk;
 reg needs_reset;
 wire [31:0] actual_rate=rate_selector==0?50000:rate_selector==1?25000:rate_selector==2?10000:5000;
 wire [31:0] period_cycles=rate_selector==0?1000:rate_selector==1?2000:rate_selector==2?5000:10000;
 wire [31:0] actual_chunk=rate_selector==0?2000:rate_selector==1?1000:rate_selector==2?400:200;
 assign rs485_de=1'b0;
 wire [15:0] raw_x,raw_y,raw_z,raw_spare,adc_status;
 wire [31:0] uv_x,uv_y,uv_z,uv_spare,adc_frame_count;
 wire adc_trigger,adc_frame_valid;
 ad7606_controller #(.SAMPLE_RATE_HZ(SAMPLE_RATE_HZ),.EVEN_CHANNELS(0)) u_adc(
  .clk(aclk),.rst(rst||cmd_adc_reset),.sample_period_cycles(period_cycles),
  .ad_data(ad_data),.ad_busy(ad_busy),.ad_firstdata(ad_firstdata),
  .ad_reset(ad_reset),.ad_cs_n(ad_cs_n),.ad_rd_n(ad_rd_n),.ad_convsta(ad_convsta),.ad_convstb(ad_convstb),
  .ad_range(ad_range),.ad_par_ser(ad_par_ser),.ad_stby_n(ad_stby_n),
  .raw_ch1(raw_x),.raw_ch2(raw_y),.raw_ch3(raw_z),.raw_ch4(raw_spare),
  .uv_ch1(uv_x),.uv_ch2(uv_y),.uv_ch3(uv_z),.uv_ch4(uv_spare),
  .frame_valid(adc_frame_valid),.frame_count(adc_frame_count),.debug_status(adc_status),.sample_trigger(adc_trigger));
 wire [31:0] speed_period,speed_edges;
 wire speed_valid,speed_timeout,speed_edge,speed_level,speed_filtered_level;
 speed_meter #(.FILTER_CYCLES(SPEED_FILTER_CYCLES),.TIMEOUT_CYCLES(SPEED_TIMEOUT_CYCLES)) u_speed(
  .clk(aclk),.rst(rst),.speed_in(speed_in),.period_ticks(speed_period),.edge_count(speed_edges),
  .period_valid(speed_valid),.timed_out(speed_timeout),.edge_pulse(speed_edge),
  .synchronized_level(speed_level),.filtered_level(speed_filtered_level));
 wire active,source_done,stream_done,overflow,adc_error,stream_config_error,stream_idle;
 wire [31:0] captured,sent,dropped;
 wire [63:0] ticks;
 wire [63:0] captured64,sent64,dropped64,stop_records,first_tick,last_tick;
 wire stop_pending;
 reg [31:0] snap_state,snap_seq;
 reg [63:0] snap_captured,snap_sent,snap_dropped,snap_stop,snap_first,snap_last;
 reg [31:0] test_period_count,test_index,test_frame_count;
 reg test_frame_valid;
 reg [15:0] test_x,test_y,test_z;
 wire test_trigger=source_mode[0]&&test_period_count==period_cycles-1;
 always @(posedge aclk)begin
  if(rst||cmd_adc_reset)begin
   test_period_count<=0;test_index<=0;test_frame_count<=0;
   test_frame_valid<=0;test_x<=0;test_y<=0;test_z<=0;
  end else begin
   test_frame_valid<=test_trigger;
   if(cmd_start)test_index<=0;
   if(source_mode[0])begin
    if(test_trigger)begin
     test_period_count<=0;test_frame_count<=test_frame_count+1;
     test_x<=test_index[15:0];test_y<=-test_index[15:0];test_z<=test_index[15:0]^16'h55aa;
     if(active)test_index<=test_index+1;
    end else test_period_count<=test_period_count+1;
   end else test_period_count<=0;
  end
 end
 capture_stream u_capture(.clk(aclk),.rst(rst),.start(cmd_start),.abort_capture(cmd_abort),
  .target_samples(target_samples),.stream_mode(stream_mode_cfg),.stop_stream(cmd_stream_stop),
  .total_records(stream_total),.chunk_records(stream_chunk),
  .sample_trigger(source_mode[0]?test_trigger:adc_trigger),
  .frame_valid(source_mode[0]?test_frame_valid:adc_frame_valid),
  .adc_fault(source_mode[0]?1'b0:|adc_status[15:13]),
  .raw_x(source_mode[0]?test_x:raw_x),.raw_y(source_mode[0]?test_y:raw_y),.raw_z(source_mode[0]?test_z:raw_z),
  .speed_period(source_mode[0]?32'b0:speed_period),.speed_edges(source_mode[0]?32'b0:speed_edges),
  .speed_valid(source_mode[0]?1'b0:speed_valid),.speed_timeout(source_mode[0]?1'b1:speed_timeout),
  .speed_level(source_mode[0]?1'b0:speed_level),.speed_filtered_level(source_mode[0]?1'b0:speed_filtered_level),
  .active(active),.source_done(source_done),.stream_done(stream_done),.overflow(overflow),
  .adc_error(adc_error),.config_error(stream_config_error),.idle(stream_idle),
  .captured(captured),.sent(sent),.dropped(dropped),.ticks(ticks),
  .captured64(captured64),.sent64(sent64),.dropped64(dropped64),.stop_records(stop_records),
  .first_tick(first_tick),.last_tick(last_tick),.stop_pending(stop_pending),
  .m_axis_tdata(m_axis_tdata),.m_axis_tkeep(m_axis_tkeep),.m_axis_tvalid(m_axis_tvalid),
  .m_axis_tlast(m_axis_tlast),.m_axis_tready(m_axis_tready));
 wire busy=!stream_idle||cmd_start||cmd_abort||cmd_adc_reset;
 reg aw_pending,w_pending;
 reg [7:0] awaddr;
 reg [31:0] wdata;
 reg [3:0] wstrb;
 assign s_axi_awready=!aw_pending&&!s_axi_bvalid;
 assign s_axi_wready=!w_pending&&!s_axi_bvalid;
 assign s_axi_arready=!s_axi_rvalid;
 // Sticky readable rejection avoids synchronous CPU abort for invalid parameters.
 assign s_axi_bresp=2'b00;
 assign s_axi_rresp=2'b00;
 function [31:0] merged;
  input [31:0] old_value,new_value;input [3:0] strobes;
  integer k;
  begin
   merged=old_value;
   for(k=0;k<4;k=k+1)if(strobes[k])merged[k*8+:8]=new_value[k*8+:8];
  end
 endfunction
 wire [31:0] new_target=merged(target_samples,wdata,wstrb);
 wire [31:0] new_rate=merged(rate_selector,wdata,wstrb);
 wire [31:0] new_mode=merged(source_mode,wdata,wstrb);
 wire [31:0] new_stream_mode=merged({31'b0,stream_mode_cfg},wdata,wstrb);
 wire [31:0] new_stream_chunk=merged(stream_chunk,wdata,wstrb);
 always @(posedge aclk)begin
  if(rst)begin
   target_samples<=500000;rate_selector<=0;source_mode<=0;cfg_reject<=0;needs_reset<=1;
   cmd_start<=0;cmd_abort<=0;cmd_adc_reset<=0;cmd_stream_stop<=0;
   stream_mode_cfg<=0;stream_total<=0;stream_chunk<=2000;
   snap_state<=0;snap_seq<=0;snap_captured<=0;snap_sent<=0;snap_dropped<=0;
   snap_stop<=0;snap_first<=0;snap_last<=0;
   aw_pending<=0;w_pending<=0;awaddr<=0;wdata<=0;wstrb<=0;
   s_axi_bvalid<=0;s_axi_rvalid<=0;s_axi_rdata<=0;
  end else begin
   cmd_start<=0;cmd_abort<=0;cmd_adc_reset<=0;cmd_stream_stop<=0;
   if(s_axi_awvalid&&s_axi_awready)begin aw_pending<=1;awaddr<=s_axi_awaddr;end
   if(s_axi_wvalid&&s_axi_wready)begin w_pending<=1;wdata<=s_axi_wdata;wstrb<=s_axi_wstrb;end
   if(s_axi_bvalid&&s_axi_bready)s_axi_bvalid<=0;
   if(aw_pending&&w_pending&&!s_axi_bvalid)begin
    aw_pending<=0;w_pending<=0;s_axi_bvalid<=1;
    case(awaddr[7:2])
     1:if(wstrb[0])begin
      case(wdata)
       0:;
       1:if(busy)cfg_reject[0]<=1;else if(stream_mode_cfg)cfg_reject[7]<=1;else if(needs_reset)cfg_reject[5]<=1;else cmd_start<=1;
       2:if(busy)cfg_reject[6]<=1;else begin cmd_adc_reset<=1;needs_reset<=0;end
       4:begin cmd_abort<=1;stream_mode_cfg<=0;end
       16:cfg_reject<=0;
       default:cfg_reject[4]<=1;
      endcase
     end
     3:if(|wstrb)begin
      if(busy)cfg_reject[0]<=1;
      else if(new_target==0||new_target>MAX_RECORDS)cfg_reject[3]<=1;
      else target_samples<=new_target;
     end
     18:; // DE remains disabled in this native Linux design.
     21:if(|wstrb)begin
      if(busy)cfg_reject[0]<=1;
      else if(new_rate>3)cfg_reject[1]<=1;
      else if(new_rate!=rate_selector)begin rate_selector<=new_rate;needs_reset<=1;end
     end
     22:if(|wstrb)begin
      if(busy)cfg_reject[0]<=1;
      else if(new_mode>1)cfg_reject[2]<=1;
      else if(new_mode!=source_mode)begin source_mode<=new_mode;needs_reset<=1;end
     end
     29:if(|wstrb)begin //0x74: opt-in stream mode, no legacy feature changes.
      if(busy)cfg_reject[0]<=1;
      else if(new_stream_mode>1)cfg_reject[7]<=1;
      else stream_mode_cfg<=new_stream_mode[0];
     end
     30:if(|wstrb)begin
      if(busy)cfg_reject[0]<=1;else stream_total[31:0]<=merged(stream_total[31:0],wdata,wstrb);
     end
     31:if(|wstrb)begin
      if(busy)cfg_reject[0]<=1;else stream_total[63:32]<=merged(stream_total[63:32],wdata,wstrb);
     end
     32:if(|wstrb)begin
      if(busy)cfg_reject[0]<=1;
      else if(new_stream_chunk!=actual_chunk)cfg_reject[7]<=1;
      else stream_chunk<=new_stream_chunk;
     end
     33:if(wstrb[0])begin //0x84: explicit START / graceful STOP / coherent SNAPSHOT.
      case(wdata)
       0:;
       1:if(busy)cfg_reject[0]<=1;
         else if(!stream_mode_cfg||stream_chunk!=actual_chunk)cfg_reject[7]<=1;
         else if(needs_reset)cfg_reject[5]<=1;
         else cmd_start<=1;
       2:if(!stream_mode_cfg)cfg_reject[7]<=1;else cmd_stream_stop<=1;
       4:begin
        snap_state<={26'b0,overflow,
          stream_config_error|(|cfg_reject)|adc_error|overflow|(!source_mode[0]&&|adc_status[15:13]),
          stream_done,source_done,stop_pending,active};
        snap_seq<=snap_seq+1;
        snap_captured<=captured64;snap_sent<=sent64;snap_dropped<=dropped64;
        snap_stop<=stop_records;snap_first<=first_tick;snap_last<=last_tick;
       end
       default:cfg_reject[7]<=1;
      endcase
     end
     default:;
    endcase
   end
   if(s_axi_rvalid&&s_axi_rready)s_axi_rvalid<=0;
   if(s_axi_arvalid&&s_axi_arready)begin
    s_axi_rvalid<=1;
    case(s_axi_araddr[7:2])
     0:s_axi_rdata<=32'h32415043;
     1:s_axi_rdata<=0;
     2:s_axi_rdata<={source_mode[0]?16'b0:adc_status,9'b0,source_mode[0],
       stream_config_error|(|cfg_reject),adc_error,overflow,stream_done,source_done,active};
     3:s_axi_rdata<=target_samples;
     4:s_axi_rdata<=actual_rate;
     5:s_axi_rdata<=captured;
     6:s_axi_rdata<=sent;
     7:s_axi_rdata<=dropped;
     8:s_axi_rdata<=source_mode[0]?0:speed_period;
     9:s_axi_rdata<=source_mode[0]?0:speed_edges;
     10:s_axi_rdata<=source_mode[0]?32'h2:{28'b0,speed_filtered_level,speed_level,speed_timeout,speed_valid};
     11:s_axi_rdata<=50000000;
     12:s_axi_rdata<=ticks[31:0];
     13:s_axi_rdata<=ticks[63:32];
     14:s_axi_rdata<=source_mode[0]?test_frame_count:adc_frame_count;
     15:s_axi_rdata<=32;
     16:s_axi_rdata<=SPEED_FILTER_CYCLES;
     17:s_axi_rdata<=SPEED_TIMEOUT_CYCLES;
     18:s_axi_rdata<=0;
     19:s_axi_rdata<=32'h35383452;
     20:s_axi_rdata<=32'h4d524731;
     21:s_axi_rdata<=rate_selector;
     22:s_axi_rdata<=source_mode;
     23:s_axi_rdata<=cfg_reject;
     24:s_axi_rdata<=5000000;
     25:s_axi_rdata<=32'h00050301;
     26:s_axi_rdata<=32'h00000f03;
     28:s_axi_rdata<=32'h53545231;
     29:s_axi_rdata<={31'b0,stream_mode_cfg};
     30:s_axi_rdata<=stream_total[31:0];
     31:s_axi_rdata<=stream_total[63:32];
     32:s_axi_rdata<=stream_chunk;
     33:s_axi_rdata<=0;
     34:s_axi_rdata<=snap_state;
     35:s_axi_rdata<=snap_seq;
     36:s_axi_rdata<=snap_captured[31:0];
     37:s_axi_rdata<=snap_captured[63:32];
     38:s_axi_rdata<=snap_sent[31:0];
     39:s_axi_rdata<=snap_sent[63:32];
     40:s_axi_rdata<=snap_dropped[31:0];
     41:s_axi_rdata<=snap_dropped[63:32];
     42:s_axi_rdata<=snap_stop[31:0];
     43:s_axi_rdata<=snap_stop[63:32];
     44:s_axi_rdata<=snap_first[31:0];
     45:s_axi_rdata<=snap_first[63:32];
     46:s_axi_rdata<=snap_last[31:0];
     47:s_axi_rdata<=snap_last[63:32];
     default:s_axi_rdata<=0;
    endcase
   end
  end
 end
endmodule


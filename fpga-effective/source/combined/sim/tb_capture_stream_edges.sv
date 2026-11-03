`timescale 1ns/1ps
// Accelerated stream-unit edge tests. Counter seeding covers2^32 rollover;
// it is not a claim that a24-hour hardware acquisition was executed.
module tb_capture_stream_edges;
 reg clk=0;always #10 clk=~clk;
 reg rst=1,start=0,abort_capture=0,stop_stream=0,trigger=0,frame=0,fault=0,ready=1;
 reg [15:0] x=0,y=0,z=0;
 wire active,source_done,stream_done,overflow,adc_error,config_error,idle,stop_pending;
 wire [31:0] captured,sent,dropped;
 wire [63:0] captured64,sent64,dropped64,stop_records,first_tick,last_tick,ticks,data;
 wire [7:0] keep;wire valid,last;
 capture_stream #(.FIFO_DEPTH(4)) dut(.clk(clk),.rst(rst),.start(start),.abort_capture(abort_capture),
  .target_samples(32'd1),.stream_mode(1'b1),.stop_stream(stop_stream),.total_records(64'b0),.chunk_records(32'd2),
  .sample_trigger(trigger),.frame_valid(frame),.adc_fault(fault),.raw_x(x),.raw_y(y),.raw_z(z),
  .speed_period(32'b0),.speed_edges(32'b0),.speed_valid(1'b0),.speed_timeout(1'b1),.speed_level(1'b0),.speed_filtered_level(1'b0),
  .active(active),.source_done(source_done),.stream_done(stream_done),.overflow(overflow),.adc_error(adc_error),.config_error(config_error),.idle(idle),
  .captured(captured),.sent(sent),.dropped(dropped),.captured64(captured64),.sent64(sent64),.dropped64(dropped64),.stop_records(stop_records),
  .first_tick(first_tick),.last_tick(last_tick),.stop_pending(stop_pending),.ticks(ticks),
  .m_axis_tdata(data),.m_axis_tkeep(keep),.m_axis_tvalid(valid),.m_axis_tlast(last),.m_axis_tready(ready));
 integer seen=0,beat=0,packets=0,checks=0;
 reg [63:0] base=0,stimulus=0;
 reg [255:0] record;
 always @(posedge clk)if(!rst&&valid&&ready)begin
  record[beat*64+:64]=data;
  if(keep!=255||last!==((seen%2==1)&&beat==3))$fatal(1,"unit packet boundary");
  if(last)packets=packets+1;
  if(beat==3)begin
   if(record[63:0]!==base+seen)$fatal(1,"unit64 index discontinuity");
   if(record[143:128]!==16'(base+seen)||record[159:144]!==16'(-(base+seen))||record[175:160]!==(16'(base+seen)^16'h55aa))$fatal(1,"unit original sample bytes");
   if(record[191:176]!=5)$fatal(1,"unit original flags");
   seen=seen+1;beat=0;checks=checks+1;
  end else beat=beat+1;
 end
 task automatic arm;
  @(negedge clk);rst=1;start=0;stop_stream=0;trigger=0;frame=0;ready=1;
  repeat(3)@(negedge clk);rst=0;seen=0;beat=0;packets=0;base=0;stimulus=0;
  @(negedge clk);start=1;@(negedge clk);start=0;
 endtask
 task automatic emit_record(input bit stop_on_completion);
  @(negedge clk);trigger=1;
  @(negedge clk);trigger=0;
  repeat(3)@(negedge clk);
  x=16'(stimulus);y=16'(-stimulus);z=16'(stimulus)^16'h55aa;frame=1;stop_stream=stop_on_completion;stimulus=stimulus+1;
  @(negedge clk);frame=0;stop_stream=0;
  repeat(4)@(negedge clk);
 endtask
 task automatic finish(input integer count,input [63:0] final_count);
  wait(stream_done);repeat(5)@(negedge clk);
  if(!idle||active||!source_done||overflow||adc_error||config_error||dropped64||seen!=count||packets!=count/2||captured64!=final_count||sent64!=final_count||stop_records!=final_count)$fatal(1,"unit STOP/drain count%0d produced%0d expected%0d stop%0d",seen,captured64,final_count,stop_records);
 endtask
 initial begin
  arm();emit_record(0);emit_record(1);finish(2,2);
  $display("PASS STOP on the exact completing chunk boundary");
  arm();emit_record(0);emit_record(0);
  @(negedge clk);stop_stream=1;@(negedge clk);stop_stream=0;
  emit_record(0);emit_record(0);finish(4,4);
  $display("PASS STOP immediately after a boundary finishes the following chunk");
  arm();@(negedge clk);stop_stream=1;@(negedge clk);stop_stream=0;
  ready=0;emit_record(0);emit_record(0);
  if(!source_done||stream_done||captured64!=2||stop_records!=2)$fatal(1,"unit pre-sample STOP lost tail");
  @(negedge clk);ready=1;finish(2,2);
  $display("PASS STOP before first sample retains the full first chunk");
  arm();@(negedge clk);
  base=64'h00000000_ffffffff;stimulus=base;
  dut.trigger_index=base;dut.captured64=base;dut.sent64=base;dut.captured=32'hffffffff;dut.sent=32'hffffffff;
  emit_record(0);emit_record(1);finish(2,64'h00000001_00000001);
  if(captured!=1||sent!=1)$fatal(1,"legacy low counters did not wrap independently");
  $display("PASS seeded64-bit record/counter rollover across2^32");
  $display("ALL STREAM EDGE TESTS PASSED records=%0d",checks);$finish;
 end
 initial begin #100000;$fatal(1,"edge watchdog");end
endmodule

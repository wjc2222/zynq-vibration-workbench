`timescale 1ns/1ps
// AXI register integration: no DMA/card model is implied by this test.
module tb_capture_streaming;
 reg clk=0;always #10 clk=~clk;
 reg rn=0;
 reg [7:0] awa=0,ara=0;reg awv=0,wv=0,br=0,arv=0,rr=0;
 reg [31:0] wd=0;reg [3:0] ws=0;
 wire awr,wr,bv,arr,rv;wire [31:0] rdata;wire [1:0] bp,rp;
 wire [63:0] data;wire [7:0] keep;wire tv,tl;
 reg tr=1,random_ready=1;
 integer cycles=0;
 always @(negedge clk)begin cycles=cycles+1;if(random_ready)tr=(cycles%7!=0&&cycles%11!=0);end
 capture_ip dut(.aclk(clk),.aresetn(rn),.ad_data(16'b0),.ad_busy(1'b0),.ad_firstdata(1'b0),.speed_in(1'b0),
 .ad_reset(),.ad_cs_n(),.ad_rd_n(),.ad_convsta(),.ad_convstb(),.ad_range(),.ad_par_ser(),.ad_stby_n(),.rs485_de(),
 .s_axi_awaddr(awa),.s_axi_awprot(3'b0),.s_axi_awvalid(awv),.s_axi_awready(awr),
 .s_axi_wdata(wd),.s_axi_wstrb(ws),.s_axi_wvalid(wv),.s_axi_wready(wr),
 .s_axi_bresp(bp),.s_axi_bvalid(bv),.s_axi_bready(br),
 .s_axi_araddr(ara),.s_axi_arprot(3'b0),.s_axi_arvalid(arv),.s_axi_arready(arr),
 .s_axi_rdata(rdata),.s_axi_rresp(rp),.s_axi_rvalid(rv),.s_axi_rready(rr),
 .m_axis_tdata(data),.m_axis_tkeep(keep),.m_axis_tvalid(tv),.m_axis_tlast(tl),.m_axis_tready(tr));
 defparam dut.u_adc.STARTUP_CYCLES=20;
 task automatic send_aw(input [7:0] a);
  @(negedge clk);awa=a;awv=1;do @(posedge clk);while(!awr);@(negedge clk);awv=0;
 endtask
 task automatic send_w(input [31:0] d);
  @(negedge clk);wd=d;ws=15;wv=1;do @(posedge clk);while(!wr);@(negedge clk);wv=0;
 endtask
 task automatic write_reg(input [7:0] a,input [31:0] d);
  if(cycles%2)begin send_w(d);repeat(2)@(negedge clk);send_aw(a);end
  else begin send_aw(a);repeat(3)@(negedge clk);send_w(d);end
  wait(bv);repeat(2)@(negedge clk);if(!bv||bp!=0)$fatal(1,"B hold");
  br=1;@(negedge clk);br=0;
 endtask
 task automatic read_reg(input [7:0] a,output [31:0] d);
  @(negedge clk);ara=a;arv=1;do @(posedge clk);while(!arr);@(negedge clk);arv=0;
  wait(rv);d=rdata;repeat(2)@(negedge clk);if(!rv||rdata!==d||rp!=0)$fatal(1,"R hold");
  rr=1;@(negedge clk);rr=0;
 endtask
 task automatic read64(input [7:0] a,output [63:0] d);
  reg [31:0] lo,hi;begin read_reg(a,lo);read_reg(a+4,hi);d={hi,lo};end
 endtask
 integer seen=0,beat=0,chunk=2000,expected_step=1000,lasts=0,checked=0;
 reg monitor=0,finite=0;
 reg [255:0] record;
 reg [63:0] last_tick=0,first_tick=0;
 always @(posedge clk)if(monitor&&tv&&tr)begin
  record[beat*64+:64]=data;
  if(keep!==8'hff||tl!==((finite||(seen%chunk==chunk-1))&&beat==3))$fatal(1,"AXIS/TLAST seen%0d beat%0d",seen,beat);
  if(tl)lasts=lasts+1;
  if(beat==3)begin
   if(record[63:0]!==64'(seen))$fatal(1,"global index %0d != %0d",record[63:0],seen);
   if(seen&&record[127:64]-last_tick!=expected_step)$fatal(1,"timestamp discontinuity");
   if(!seen)first_tick=record[127:64];last_tick=record[127:64];
   if(record[143:128]!==16'(seen)||record[159:144]!==16'(-seen)||record[175:160]!==(16'(seen)^16'h55aa))$fatal(1,"test sample");
   if(record[191:176]!=5||record[255:192]!=0)$fatal(1,"test flags/speed");
   seen=seen+1;beat=0;checked=checked+1;
  end else beat=beat+1;
 end
 reg stalled=0;reg [63:0] stalled_data;reg stalled_last;
 reg [31:0] v,snapshot_seq;
 reg [63:0] a,b;
 always @(posedge clk)begin
  if(stalled&&!dut.cmd_abort&&rn&&(data!==stalled_data||tl!==stalled_last||!tv))$fatal(1,"unstable backpressure");
  stalled=rn&&tv&&!tr&&!dut.cmd_abort;stalled_data=data;stalled_last=tl;
 end
 task automatic arm(input integer sel,input [63:0] total);
  write_reg(4,4);write_reg(4,16);write_reg(8'h54,sel);write_reg(8'h58,1);write_reg(4,2);
  chunk=sel==0?2000:sel==1?1000:sel==2?400:200;
  expected_step=sel==0?1000:sel==1?2000:sel==2?5000:10000;
  write_reg(8'h74,1);write_reg(8'h78,total[31:0]);write_reg(8'h7c,total[63:32]);write_reg(8'h80,chunk);
  read_reg(8'h5c,v);if(v)$fatal(1,"arm rejected %h",v);
  @(negedge clk);seen=0;beat=0;lasts=0;monitor=1;finite=0;
 endtask
 task automatic snapshot;write_reg(8'h84,4);endtask
 task automatic finished(input integer count);
  wait(dut.stream_done);repeat(8)@(negedge clk);
  if(seen!=count||beat||lasts!=count/chunk||tv)$fatal(1,"stream terminal data count%0d expected%0d packets%0d",seen,count,lasts);
  snapshot();read_reg(8'h88,v);if(v!=(dut.stop_pending?14:12))$fatal(1,"stream terminal state %h",v);
  read64(8'h90,a);if(a!=count)$fatal(1,"snapshot produced");
  read64(8'h98,a);if(a!=count)$fatal(1,"snapshot sent");
  read64(8'ha0,a);if(a!=0)$fatal(1,"snapshot dropped");
  read64(8'ha8,a);if(a!=count)$fatal(1,"snapshot final count");
  read64(8'hb0,a);if(a!=first_tick)$fatal(1,"snapshot first tick");
  read64(8'hb8,a);if(a!=last_tick)$fatal(1,"snapshot last tick");
  monitor=0;
 endtask
 integer sel,n;
 initial begin
  repeat(5)@(negedge clk);rn=1;
  read_reg(8'h70,v);if(v!=32'h53545231)$fatal(1,"STR1 identity");
  read_reg(8'h50,v);if(v!=32'h4d524731)$fatal(1,"legacy revision changed");
  read_reg(8'h68,v);if(v!=32'hf03)$fatal(1,"legacy capabilities changed");
  // Reject an invalid chunk; finite START must not opt into continuous mode.
  write_reg(8'h80,1999);read_reg(8'h5c,v);if(!(v&128))$fatal(1,"bad chunk accepted");
  write_reg(4,16);write_reg(8'h74,1);write_reg(4,1);read_reg(8'h5c,v);if(!(v&128))$fatal(1,"legacy START selected stream");
  // Two packets at all rates, without restarting sample phase/index at TLAST.
  for(sel=0;sel<4;sel=sel+1)begin
   n=(sel==0?2000:sel==1?1000:sel==2?400:200)*2;
   arm(sel,n);write_reg(8'h84,1);finished(n);
  end
  $display("PASS four stream rates, per-chunk TLAST and continuous seq/ticks");
  arm(0,0);write_reg(8'h84,1);wait(seen>=10);
  snapshot();read_reg(8'h8c,snapshot_seq);read64(8'h90,a);wait(seen>=20);read64(8'h90,b);
  if(a!=b)$fatal(1,"snapshot changed without SNAPSHOT command");
  snapshot();read_reg(8'h8c,v);read64(8'h90,b);if(v!=snapshot_seq+1||b<=a)$fatal(1,"snapshot did not advance coherently");
  wait(seen>=2050);write_reg(8'h84,2);
  wait(dut.captured64>=3990);@(negedge clk);random_ready=0;tr=0;
  wait(dut.source_done);repeat(10)@(negedge clk);
  if(dut.stream_done||dut.stream_idle)$fatal(1,"STOP declared complete before draining");
  snapshot();read_reg(8'h88,v);if(v!=6)$fatal(1,"STOP source state %h",v);
  read64(8'ha8,a);if(a!=4000)$fatal(1,"STOP boundary lost accepted records");
  read64(8'h90,a);if(a!=4000)$fatal(1,"STOP producer count");
  write_reg(8'h74,0);read_reg(8'h5c,v);if(!(v&1))$fatal(1,"draining mode write accepted");
  write_reg(4,16);@(negedge clk);random_ready=1;finished(4000);
  // STOP is idempotent after completion and never erases the retained counters.
  write_reg(8'h84,2);snapshot();read64(8'h98,a);if(a!=4000)$fatal(1,"repeated STOP lost data");
  $display("PASS manual STOP next-boundary drain, immutable snapshot and final prefix");
  // All extension 64-bit words must be latched atomically, not live split reads.
  force dut.u_capture.captured64=64'h00000001_fffffff0;
  force dut.u_capture.sent64=64'h00000002_00000010;
  force dut.u_capture.dropped64=64'h00000003_00000001;
  force dut.u_capture.stop_records=64'h00000004_00000020;
  snapshot();read64(8'h90,a);if(a!=64'h00000001_fffffff0)$fatal(1,"produced64 truncation");
  read64(8'h98,a);if(a!=64'h00000002_00000010)$fatal(1,"sent64 truncation");
  read64(8'ha0,a);if(a!=64'h00000003_00000001)$fatal(1,"dropped64 truncation");
  read64(8'ha8,a);if(a!=64'h00000004_00000020)$fatal(1,"stop64 truncation");
  release dut.u_capture.captured64;release dut.u_capture.sent64;release dut.u_capture.dropped64;release dut.u_capture.stop_records;
  arm(0,0);monitor=0;random_ready=0;tr=0;write_reg(8'h84,1);wait(dut.overflow);
  snapshot();read_reg(8'h88,v);if((v&49)!=48)$fatal(1,"overflow not explicit %h",v);
  read64(8'ha0,a);if(!a)$fatal(1,"overflow missing dropped count");
  write_reg(4,4);repeat(8)@(negedge clk);read_reg(8'h74,v);if(v!=0)$fatal(1,"ABORT did not restore finite mode");
  if(!dut.stream_idle)$fatal(1,"ABORT did not release pending FIFO");
  $display("PASS 64-bit snapshot and explicit overflow/abort recovery");
  // A legacy client can run again after aborting a streaming producer.
  write_reg(4,16);write_reg(12,1);write_reg(4,2);
  @(negedge clk);seen=0;beat=0;lasts=0;monitor=1;finite=1;random_ready=1;
  write_reg(4,1);wait(dut.stream_done);repeat(8)@(negedge clk);
  if(seen!=1||lasts!=1)$fatal(1,"finite compatibility after streaming");
  $display("ALL STREAM RTL TESTS PASSED records=%0d",checked);$finish;
 end
 initial begin #1000000000;$fatal(1,"watchdog");end
endmodule

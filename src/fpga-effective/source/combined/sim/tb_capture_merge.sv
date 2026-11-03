`timescale 1ns/1ps
module tb_capture_merge;
 reg clk=0;always #10 clk=~clk;
 reg rn=0,busy=0,first=0,speed=0,adc_enabled=0;
 reg [15:0] adc_data=0;
 wire ad_reset,cs,rd,conv,convb,range_sel,par,stby,de;
 reg [7:0] awa=0,ara=0;reg awv=0,wv=0,br=0,arv=0,rr=0;
 reg [31:0] wd=0;reg [3:0] ws=0;
 wire awr,wr,bv,arr,rv;wire [31:0] rdata;wire [1:0] bp,rp;
 wire [63:0] data;wire [7:0] keep;wire tv,tl;
 reg tr=1,random_ready=1;
 integer cycles=0;
 always @(negedge clk)begin cycles=cycles+1;if(random_ready)tr=(cycles%7!=0&&cycles%11!=0);end
 capture_ip dut(.aclk(clk),.aresetn(rn),.ad_data(adc_data),.ad_busy(busy),.ad_firstdata(first),.speed_in(speed),
 .ad_reset(ad_reset),.ad_cs_n(cs),.ad_rd_n(rd),.ad_convsta(conv),.ad_convstb(convb),.ad_range(range_sel),.ad_par_ser(par),.ad_stby_n(stby),.rs485_de(de),
 .s_axi_awaddr(awa),.s_axi_awprot(3'b0),.s_axi_awvalid(awv),.s_axi_awready(awr),
 .s_axi_wdata(wd),.s_axi_wstrb(ws),.s_axi_wvalid(wv),.s_axi_wready(wr),
 .s_axi_bresp(bp),.s_axi_bvalid(bv),.s_axi_bready(br),
 .s_axi_araddr(ara),.s_axi_arprot(3'b0),.s_axi_arvalid(arv),.s_axi_arready(arr),
 .s_axi_rdata(rdata),.s_axi_rresp(rp),.s_axi_rvalid(rv),.s_axi_rready(rr),
 .m_axis_tdata(data),.m_axis_tkeep(keep),.m_axis_tvalid(tv),.m_axis_tlast(tl),.m_axis_tready(tr));
 defparam dut.u_adc.STARTUP_CYCLES=20;
 integer adc_reads=0,adc_frames=0;
 always @(posedge conv)if(adc_enabled)begin
  adc_frames=adc_frames+1;adc_reads=0;#35;busy=1;#4165;busy=0;
 end
 always @(negedge rd)if(rn&&adc_enabled)begin
  adc_data<=#37 (16'(adc_frames*10+adc_reads));first<=#37 (adc_reads==0);adc_reads=adc_reads+1;
 end
 always @(posedge rd)if(rn&&adc_enabled)begin adc_data<=#6 16'hzzzz;first<=#6 0;end
 always #700 speed=~speed;
 task automatic send_aw(input [7:0] a);
  @(negedge clk);awa=a;awv=1;do @(posedge clk);while(!awr);@(negedge clk);awv=0;
 endtask
 task automatic send_w(input [31:0] d,input [3:0] s);
  @(negedge clk);wd=d;ws=s;wv=1;do @(posedge clk);while(!wr);@(negedge clk);wv=0;
 endtask
 task automatic write_reg(input [7:0] a,input [31:0] d);
  if(cycles%2)begin send_w(d,15);repeat(2)@(negedge clk);send_aw(a);end
  else begin send_aw(a);repeat(3)@(negedge clk);send_w(d,15);end
  wait(bv);repeat(2)@(negedge clk);if(!bv||bp!=0)$fatal(1,"B hold");
  br=1;@(negedge clk);br=0;
 endtask
 task automatic read_reg(input [7:0] a,output [31:0] d);
  @(negedge clk);ara=a;arv=1;do @(posedge clk);while(!arr);@(negedge clk);arv=0;
  wait(rv);d=rdata;repeat(2)@(negedge clk);if(!rv||rdata!==d||rp!=0)$fatal(1,"R hold");
  rr=1;@(negedge clk);rr=0;
 endtask
 integer seen=0,beat=0,target=0,expected_step=1000,lasts=0,checked=0;
 reg monitor=0,expect_test=1;
 reg [255:0] record;
 reg [63:0] last_tick=0;
 always @(posedge clk)if(monitor&&tv&&tr)begin
  record[beat*64+:64]=data;
  if(keep!==8'hff||tl!==(seen==target-1&&beat==3))$fatal(1,"AXIS/TLAST seen%0d beat%0d",seen,beat);
  if(tl)lasts=lasts+1;
  if(beat==3)begin
   if(record[63:0]!==64'(seen))$fatal(1,"index %0d != %0d",record[63:0],seen);
   if(seen&&record[127:64]-last_tick!=expected_step)$fatal(1,"tick step");
   last_tick=record[127:64];
   if((record[191:176]&16'hffe0)!=0||!record[176])$fatal(1,"flags layout");
   if(expect_test)begin
    if(record[143:128]!==16'(seen)||record[159:144]!==16'(-seen)||record[175:160]!==(16'(seen)^16'h55aa))$fatal(1,"test sample");
    if(record[191:176]!=5||record[255:192]!=0)$fatal(1,"test speed unavailable");
   end else if(record[143:128]+2!=record[159:144]||record[159:144]+2!=record[175:160])$fatal(1,"real V1/V3/V5");
   seen=seen+1;beat=0;checked=checked+1;
  end else beat=beat+1;
 end
 reg stalled=0;reg [63:0] stalled_data;reg stalled_last;
 always @(posedge clk)begin
  if(stalled&&!dut.cmd_abort&&rn&&(data!==stalled_data||tl!==stalled_last||!tv))$fatal(1,"unstable backpressure");
  stalled=rn&&tv&&!tr&&!dut.cmd_abort;
  stalled_data=data;stalled_last=tl;
 end
 task automatic arm_monitor(input integer n,input integer step,input bit synthetic);
  @(negedge clk);seen=0;beat=0;lasts=0;target=n;expected_step=step;expect_test=synthetic;monitor=1;
 endtask
 reg [31:0] v;
 task automatic setup(input integer sel,input integer mode,input integer n);
  write_reg(4,4);write_reg(4,16);
  write_reg(8'h54,sel);write_reg(8'h58,mode);write_reg(12,n);write_reg(4,2);
  repeat(4)@(negedge clk);
  read_reg(8'h5c,v);if(v!=0)$fatal(1,"configuration rejected %h",v);
 endtask
 task automatic finish_run;
  wait(seen==target);repeat(12)@(negedge clk);read_reg(8,v);
  if((v&63)!=6||lasts!=1||tv)$fatal(1,"terminal state %h last%0d",v,lasts);
  read_reg(20,v);if(v!=target)$fatal(1,"captured count");
  read_reg(24,v);if(v!=target)$fatal(1,"sent count");
  monitor=0;
 endtask
 integer sel,n;
 initial begin
  repeat(5)@(negedge clk);rn=1;
  read_reg(0,v);if(v!=32'h32415043)$fatal(1,"identity");
  read_reg(8'h50,v);if(v!=32'h4d524731)$fatal(1,"revision");
  read_reg(8'h60,v);if(v!=5000000)$fatal(1,"range");
  read_reg(8'h64,v);if(v!=32'h50301)$fatal(1,"map");
  read_reg(8'h68,v);if(v!=32'hf03)$fatal(1,"features");
  write_reg(8'h48,1);if(de!==0)$fatal(1,"RS485 enabled");
  // Invalid fields stay unchanged and report distinct sticky causes.
  write_reg(8'h54,4);write_reg(8'h58,2);write_reg(12,0);write_reg(4,3);
  read_reg(8'h5c,v);if((v&30)!=30)$fatal(1,"invalid config mask %h",v);
  write_reg(4,16);write_reg(8'h58,1);write_reg(4,1);
  read_reg(8'h5c,v);if(!(v&32))$fatal(1,"missing reset gate");
  for(sel=0;sel<4;sel=sel+1)begin
   n=sel==0?1:sel==1?7:sel==2?2:5;
   setup(sel,1,n);
   read_reg(8'h10,v);if(v!=(sel==0?50000:sel==1?25000:sel==2?10000:5000))$fatal(1,"actual rate");
   read_reg(8,v);if(!(v&64))$fatal(1,"test source identity");
   arm_monitor(n,sel==0?1000:sel==1?2000:sel==2?5000:10000,1);write_reg(4,1);
   finish_run;
  end
  $display("PASS four rates finite test captures/TLAST");
  // A source_done producer may still have undrained FIFO/output beats.
  setup(0,1,8);random_ready=0;tr=0;write_reg(4,1);
  wait(dut.source_done);write_reg(8'h54,2);write_reg(8'h58,0);write_reg(12,7);write_reg(4,2);
  read_reg(8'h5c,v);if((v&65)!=65)$fatal(1,"undrained writes not rejected %h",v);
  read_reg(8'h54,v);if(v!=0)$fatal(1,"rate changed during drain");
  read_reg(8'h58,v);if(v!=1)$fatal(1,"mode changed during drain");
  write_reg(4,16);arm_monitor(8,1000,1);random_ready=1;
  finish_run;
  $display("PASS backpressure stable and undrained configuration refusal");
  // Abort an active job and start another without board/global reset.
  setup(0,1,100);write_reg(4,1);wait(dut.captured>=3);write_reg(8'h54,3);
  read_reg(8'h5c,v);if(!(v&1))$fatal(1,"active config refusal");
  write_reg(4,4);repeat(8)@(negedge clk);read_reg(8,v);
  if(v&7)$fatal(1,"abort did not empty producer");
  setup(1,1,3);arm_monitor(3,2000,1);write_reg(4,1);finish_run;
  $display("PASS active refusal abort/restart");
  // No ADC is present: REAL fails; self-test success never masks this.
  setup(0,0,2);write_reg(4,1);wait(dut.adc_error);read_reg(8,v);
  if(!(v&16)||(v&64))$fatal(1,"real ADC failure/source status");
  write_reg(4,4);
  $display("PASS missing real ADC fails");
  // Functional ADC model validates all four real conversion spacings.
  adc_enabled=1;
  for(sel=0;sel<4;sel=sel+1)begin
   setup(sel,0,4);wait(dut.adc_frame_count>=4);
   arm_monitor(4,sel==0?1000:sel==1?2000:sel==2?5000:10000,0);write_reg(4,1);finish_run;
   read_reg(8,v);if(v&64)$fatal(1,"real source flagged test");
  end
  $display("PASS real ADC rates/channel selection");
  $display("ALL MERGED CAPTURE TESTS PASSED records=%0d",checked);$finish;
 end
 initial begin #30000000;$fatal(1,"watchdog");end
endmodule


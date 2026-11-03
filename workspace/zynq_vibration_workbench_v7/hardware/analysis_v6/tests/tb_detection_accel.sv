`timescale 1ns/1ps
module tb_detection_accel;
reg clk=0;always #5 clk=~clk;
reg resetn=0;
reg [15:0] awaddr=0,araddr=0;reg [31:0] wdata=0;reg awvalid=0,wvalid=0,arvalid=0;
wire awready,wready,bvalid,arready,rvalid;wire [31:0] rdata;reg bready=1,rready=1;
detection_accel dut(.aclk(clk),.aresetn(resetn),.s_axi_awaddr(awaddr),.s_axi_awprot(3'b0),.s_axi_awvalid(awvalid),.s_axi_awready(awready),
 .s_axi_wdata(wdata),.s_axi_wstrb(4'hf),.s_axi_wvalid(wvalid),.s_axi_wready(wready),.s_axi_bresp(),.s_axi_bvalid(bvalid),.s_axi_bready(bready),
 .s_axi_araddr(araddr),.s_axi_arprot(3'b0),.s_axi_arvalid(arvalid),.s_axi_arready(arready),.s_axi_rdata(rdata),.s_axi_rresp(),.s_axi_rvalid(rvalid),.s_axi_rready(rready));
task write_reg(input [15:0] addr,input [31:0] data);
 begin
  @(negedge clk);awaddr=addr;wdata=data;awvalid=1;wvalid=1;
  fork
   begin do @(posedge clk);while(!awready);@(negedge clk);awvalid=0;end
   begin do @(posedge clk);while(!wready);@(negedge clk);wvalid=0;end
  join
  do @(posedge clk);while(!bvalid);
 end
endtask
task read_reg(input [15:0] addr,output [31:0] value);
 begin @(negedge clk);araddr=addr;arvalid=1;do @(posedge clk);while(!arready);@(negedge clk);arvalid=0;do @(posedge clk);while(!rvalid);value=rdata;end
endtask
reg [47:0] inputs[0:82550];reg [71:0] expected[0:8191];
integer p,n,i,c,fd,fdin,fdout,checked;reg [31:0] v;reg [63:0] totalsum[0:2],totalsq[0:2];
reg signed [23:0] actual0,actual1,actual2;
always @(posedge clk)begin
 if(resetn&&dut.state==3)begin
  actual0=dut.filtered0;actual1=dut.filtered1;actual2=dut.filtered2;
  if({actual2,actual1,actual0}!==expected[dut.low_count])$fatal(1,"FIR mismatch profile=%0d sample=%0d got=%h expected=%h",p,dut.low_count,{actual2,actual1,actual0},expected[dut.low_count]);
  checked=checked+1;
  if((dut.low_count&1023)==0)$display("FIR_PROGRESS profile=%0d sample=%0d",p,dut.low_count);
 end
 if(resetn&&dut.fft_send_valid&&dut.fft_input_ready)$fdisplay(fdin,"%0d %0d %0d",dut.axis,dut.fft_input_index,$signed(dut.fft_input[23:0]));
 if(resetn&&dut.fft_valid)$fdisplay(fdout,"%0d %0d %0d %0d %0d",dut.axis,dut.fft_user[12:0],$signed(dut.fft_output[23:0]),$signed(dut.fft_output[47:24]),dut.fft_user[23:16]);
end
initial begin
 checked=0;fdin=0;fdout=0;
 repeat(10)@(posedge clk);@(negedge clk);resetn=1;
 for(p=0;p<4;p=p+1)begin
  $readmemh($sformatf("input_%0d.mem",p),inputs);
  $readmemh($sformatf("expected_%0d.mem",p),expected);
  fdin=$fopen($sformatf("fft_input_%0d.txt",p),"w");fdout=$fopen($sformatf("fft_output_%0d.txt",p),"w");
  write_reg(4,2);write_reg(12,p);write_reg(16'h40,(200<<16)|100);write_reg(4,1);
  case(p)0:n=82551;1:n=41276;2:n=16511;3:n=8256;endcase
  for(i=0;i<n;i=i+1)begin write_reg(16'h10,inputs[i][31:0]);write_reg(16'h14,{16'd0,inputs[i][47:32]});end
  do begin repeat(1000)@(posedge clk);read_reg(8,v);end while(!v[2]);
  if(v[3])$fatal(1,"Hardware error profile=%0d",p);
  if(dut.low_count!=8192||dut.raw_count!=n)$fatal(1,"Counts");
  for(i=0;i<8192;i=i+1)for(c=0;c<3;c=c+1)begin
   write_reg(16'h20,i*3+c);read_reg(16'h20,v);read_reg(16'h24,v);
   if(v!={{8{expected[i][c*24+23]}},expected[i][c*24+:24]})$fatal(1,"Read sample mismatch index%0d axis%0d",i,c);
  end
  fd=$fopen($sformatf("rtl_stats_%0d.txt",p),"w");
  for(c=0;c<3;c=c+1)$fdisplay(fd,"%0d %0d %0d %0d %0d %0d %0d",c,$signed(dut.sums[c]),dut.sumsq[c],$signed(dut.minima[c]),$signed(dut.maxima[c]),dut.rawclip[c],dut.exponents[c]);
  $fclose(fd);$fclose(fdin);$fclose(fdout);
  $display("PROFILE_PASS profile=%0d raw=%0d filtered=8192 cycles=%0d",p,n,dut.cycles);
 end
 $display("RTL_PASS profiles=4 sample_checks=%0d",checked);$finish;
end
initial begin #2000000000;$fatal(1,"Simulation timeout");end
endmodule

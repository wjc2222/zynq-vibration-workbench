`timescale 1ns/1ps
// Bus/result-port isolation unit. Preloads internal RAM and forces DONE.
// This verifies address/ready/read semantics, never FIR/FFT arithmetic.
module result_port_unit_tb;
reg clk=0;always #5 clk=~clk;reg resetn=0;
reg [15:0] awaddr=0,araddr=0;reg [31:0] wdata=0;reg awvalid=0,wvalid=0,arvalid=0;
wire awready,wready,bvalid,arready,rvalid;wire [31:0] rdata;
detection_accel dut(.aclk(clk),.aresetn(resetn),.s_axi_awaddr(awaddr),.s_axi_awprot(3'b0),.s_axi_awvalid(awvalid),.s_axi_awready(awready),
 .s_axi_wdata(wdata),.s_axi_wstrb(4'hf),.s_axi_wvalid(wvalid),.s_axi_wready(wready),.s_axi_bresp(),.s_axi_bvalid(bvalid),.s_axi_bready(1'b1),
 .s_axi_araddr(araddr),.s_axi_arprot(3'b0),.s_axi_arvalid(arvalid),.s_axi_arready(arready),.s_axi_rdata(rdata),.s_axi_rresp(),.s_axi_rvalid(rvalid),.s_axi_rready(1'b1));
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
integer transactions=0,polls=0;
task check_result(input [31:0] index,input [63:0] expected);
 reg [31:0] tag,lo,hi;integer tries;
 begin
  write_reg(16'h20,index);tries=0;
  do begin read_reg(16'h78,tag);tries=tries+1;polls=polls+1;if(tries>10)$fatal(1,"READY timeout index%h",index);end while(tag!=index);
  read_reg(16'h24,lo);read_reg(16'h28,hi);
  if({hi,lo}!==expected)$fatal(1,"RESULT_PORT mismatch index=%h got=%h exp=%h",index,{hi,lo},expected);
  transactions=transactions+1;
 end
endtask
function [63:0] sample_value(input integer axis,input integer point);
 reg signed [23:0] value;
 begin value=(point*1619+axis*32'h115931+32'h755353)&24'hffffff;sample_value={{40{value[23]}},value};end
endfunction
function [63:0] power_value(input integer axis,input integer bin);
 begin power_value=64'h0000123400000000+axis*64'h0000234500000000+bin*64'h10201;end
endfunction
integer i,ch,field;reg [63:0] expected;
initial begin
 repeat(10)@(posedge clk);@(negedge clk);resetn=1;
 repeat(3)@(posedge clk);@(negedge clk);force dut.state=4'd11;
 for(i=0;i<8192;i=i+1)begin
  dut.samples0[i]=sample_value(0,i);dut.samples1[i]=sample_value(1,i);dut.samples2[i]=sample_value(2,i);
 end
 for(i=0;i<4096;i=i+1)begin
  dut.powers0[i]=power_value(0,i);dut.powers1[i]=power_value(1,i);dut.powers2[i]=power_value(2,i);
 end
 for(ch=0;ch<3;ch=ch+1)begin
  dut.nyquist[ch]=power_value(ch,4096);
  dut.minima[ch]=-123456-ch;dut.maxima[ch]=234567+ch;
  dut.sums[ch]=-64'sh1234567890-ch;dut.sumsq[ch]=64'h234567891234+ch;
  dut.rawclip[ch]=34567+ch;dut.exponents[ch]=7+ch;dut.saturation[ch]=23+ch;
  for(i=0;i<18;i=i+1)dut.bandpower[ch][i]=64'h9000000000000000+ch*256+i;
 end
 repeat(4)@(posedge clk);
 for(i=0;i<8192;i=i+1)for(ch=0;ch<3;ch=ch+1)check_result(i*3+ch,sample_value(ch,i));
 $display("RESULT_PORT_SAMPLES_PASS words=24576");
 for(ch=0;ch<3;ch=ch+1)for(i=0;i<=4096;i=i+1)check_result(32'h20000+ch*8192+i,power_value(ch,i));
 $display("RESULT_PORT_POWERS_PASS words=12291");
 for(ch=0;ch<3;ch=ch+1)for(i=0;i<18;i=i+1)check_result(32'h30000+ch*32+i,64'h9000000000000000+ch*256+i);
 for(ch=0;ch<3;ch=ch+1)for(field=0;field<8;field=field+1)begin
  case(field)
   0:expected=-64'sd123456-ch;1:expected=64'd234567+ch;
   2:expected=-64'sh1234567890-ch;3:expected=64'h234567891234+ch;
   4:expected=34567+ch;5:expected=7+ch;6:expected=1;7:expected=23+ch;
  endcase
  check_result(32'h10000+ch*256+field,expected);
 end
 // Repetition and distant alternating selectors test stable DONE idempotence.
 for(i=0;i<100;i=i+1)begin
  check_result(0,sample_value(0,0));check_result(0,sample_value(0,0));
  check_result(24575,sample_value(2,8191));check_result(32'h25000,power_value(2,4096));
 end
 $display("RESULT_PORT_UNIT_PASS transactions=%0d ready_polls=%0d scope=BUS_ONLY_FIR_FFT_NOT_TESTED",transactions,polls);$finish;
end
initial begin #100000000;$fatal(1,"Result port unit timeout");end
endmodule

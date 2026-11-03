`timescale 1ns/1ps
// Actual XFFT IP path. FIR result RAM is preloaded; FIR has separate RTL tests.
module tb_fft_unit;
reg clk=0;always #5 clk=~clk;reg resetn=0;
detection_accel dut(.aclk(clk),.aresetn(resetn),.s_axi_awaddr(16'd0),.s_axi_awprot(3'd0),.s_axi_awvalid(1'b0),.s_axi_awready(),.s_axi_wdata(32'd0),.s_axi_wstrb(4'd0),.s_axi_wvalid(1'b0),.s_axi_wready(),.s_axi_bresp(),.s_axi_bvalid(),.s_axi_bready(1'b1),.s_axi_araddr(16'd0),.s_axi_arprot(3'd0),.s_axi_arvalid(1'b0),.s_axi_arready(),.s_axi_rdata(),.s_axi_rresp(),.s_axi_rvalid(),.s_axi_rready(1'b1));
reg [71:0] expected[0:8191];integer i,c,p,b,fdin,fdout,fd,fdp,fdb;
reg signed [23:0] x;
reg signed [47:0] x_square;
initial begin
 for(p=0;p<4;p=p+1)begin
  resetn=0;repeat(10)@(posedge clk);@(negedge clk);resetn=1;repeat(10)@(posedge clk);@(negedge clk);
  $readmemh($sformatf("expected_%0d.mem",p),expected);
  fdin=$fopen($sformatf("fft_unit_input_%0d.txt",p),"w");fdout=$fopen($sformatf("fft_unit_output_%0d.txt",p),"w");
  for(c=0;c<3;c=c+1)begin dut.sums[c]=0;dut.sumsq[c]=0;dut.minima[c]=24'sh7fffff;dut.maxima[c]=24'sh800000;dut.rawclip[c]=2;end
  for(i=0;i<8192;i=i+1)begin
   dut.samples0[i]=expected[i][23:0];dut.samples1[i]=expected[i][47:24];dut.samples2[i]=expected[i][71:48];
   for(c=0;c<3;c=c+1)begin
    x=$signed(expected[i][c*24+:24]);x_square=x*x;dut.sums[c]=dut.sums[c]+x;dut.sumsq[c]=dut.sumsq[c]+x_square;
    if(x<dut.minima[c])dut.minima[c]=x;if(x>dut.maxima[c])dut.maxima[c]=x;
   end
  end
  for(b=0;b<13;b=b+1)begin dut.locked_lo[b]=8191;dut.locked_hi[b]=0;end
  dut.locked_lo[0]=100;dut.locked_hi[0]=200;
  dut.state=4;dut.axis=0;dut.fft_input_index=0;
  wait(dut.state==11);@(negedge clk);
  fd=$fopen($sformatf("fft_unit_stats_%0d.txt",p),"w");fdp=$fopen($sformatf("fft_unit_powers_%0d.txt",p),"w");fdb=$fopen($sformatf("fft_unit_bands_%0d.txt",p),"w");
  for(c=0;c<3;c=c+1)begin
   $fdisplay(fd,"%0d %0d %0d %0d %0d %0d %0d",c,$signed(dut.sums[c]),dut.sumsq[c],$signed(dut.minima[c]),$signed(dut.maxima[c]),dut.rawclip[c],dut.exponents[c]);
   for(b=0;b<18;b=b+1)$fdisplay(fdb,"%0d %0d %0d",c,b,dut.bandpower[c][b]);
  end
  for(i=0;i<4096;i=i+1)begin $fdisplay(fdp,"0 %0d %0d",i,dut.powers0[i]);$fdisplay(fdp,"1 %0d %0d",i,dut.powers1[i]);$fdisplay(fdp,"2 %0d %0d",i,dut.powers2[i]);end
  for(c=0;c<3;c=c+1)$fdisplay(fdp,"%0d 4096 %0d",c,dut.nyquist[c]);
  $fclose(fdin);$fclose(fdout);$fclose(fd);$fclose(fdp);$fclose(fdb);
  $display("FFT_UNIT_PROFILE_PASS profile=%0d cycles=%0d",p,dut.cycles);
 end
 $display("FFT_UNIT_PASS profiles=4 actual_XFFT=true FIR_preloaded=true");$finish;
end
always @(posedge clk)begin
 if(resetn&&dut.fft_send_valid&&dut.fft_input_ready)begin
  if(^dut.fft_input===1'bx)$fatal(1,"Unknown FFT input");
  $fdisplay(fdin,"%0d %0d %0d",dut.axis,dut.fft_input_index,$signed(dut.fft_input[23:0]));
 end
 if(resetn&&dut.fft_valid)begin
  if(^dut.fft_output===1'bx)$fatal(1,"Unknown FFT output");
  $fdisplay(fdout,"%0d %0d %0d %0d %0d",dut.axis,dut.fft_user[12:0],$signed(dut.fft_output[23:0]),$signed(dut.fft_output[47:24]),dut.fft_user[23:16]);
 end
end
initial begin #100000000;$fatal(1,"timeout");end
endmodule

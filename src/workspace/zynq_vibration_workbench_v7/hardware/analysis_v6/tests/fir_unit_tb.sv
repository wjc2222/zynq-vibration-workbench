`timescale 1ns/1ps
// FIR-only. FFT is stubbed; after MEAN, force DONE only to exercise the final
// immutable result RAM/control read protocol. No FFT or band values are tested.
module fir_unit_tb #(parameter integer FIR_USE_SYMMETRY=1,parameter integer FIRST_KIND_DEFAULT=0);
reg clk=0;always #5 clk=~clk;reg resetn=0;
reg [15:0] awaddr=0,araddr=0;reg [31:0] wdata=0;reg awvalid=0,wvalid=0,arvalid=0;
wire awready,wready,bvalid,arready,rvalid;wire [31:0] rdata;
detection_accel #(.FIR_USE_SYMMETRY(FIR_USE_SYMMETRY)) dut(.aclk(clk),.aresetn(resetn),.s_axi_awaddr(awaddr),.s_axi_awprot(3'b0),.s_axi_awvalid(awvalid),.s_axi_awready(awready),
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
task read_result(input [31:0] index,output [63:0] value);
 reg [31:0] tag,lo,hi;integer tries;
 begin
  write_reg(16'h20,index);tries=0;
  do begin read_reg(16'h78,tag);tries=tries+1;if(tries>10)$fatal(1,"READY timeout index%0d",index);end while(tag!=index);
  read_reg(16'h24,lo);read_reg(16'h28,hi);value={hi,lo};
 end
endtask
reg [47:0] inputs[0:82550];reg [71:0] expected[0:8191];reg [63:0] expected_stats[0:17];
integer profile,kind,n,i,ch,field,checked=0,cases=0,first_kind=FIRST_KIND_DEFAULT;
integer fir_state_cycles=0,expected_fir_state_cycles,profile_taps;
reg [31:0] status;reg [63:0] value;
always @(posedge clk)if(resetn&&dut.state==2)fir_state_cycles=fir_state_cycles+1;
initial begin
 if($value$plusargs("FIRST_KIND=%d",first_kind))$display("FIR_UNIT first_kind=%0d",first_kind);
 repeat(10)@(posedge clk);@(negedge clk);resetn=1;
 for(profile=0;profile<4;profile=profile+1)for(kind=first_kind;kind<7;kind=kind+1)begin
  case(profile)0:begin n=82551;profile_taps=641;end 1:begin n=41276;profile_taps=321;end 2:begin n=16511;profile_taps=129;end 3:begin n=8256;profile_taps=65;end endcase
  $readmemh($sformatf("fir_unit_%0d_%0d_input.mem",profile,kind),inputs,0,n-1);
  $readmemh($sformatf("fir_unit_%0d_%0d_expected.mem",profile,kind),expected);
  $readmemh($sformatf("fir_unit_%0d_%0d_stats.mem",profile,kind),expected_stats);
  write_reg(4,3);read_reg(8,status);if(!status[0]||status[1]||status[3])$fatal(1,"RESET must win START and clear error");
  write_reg(12,profile);fir_state_cycles=0;write_reg(4,1);
  for(i=0;i<n;i=i+1)begin write_reg(16'h10,inputs[i][31:0]);write_reg(16'h14,{16'd0,inputs[i][47:32]});end
  wait(dut.state==4);@(negedge clk);
  if(dut.low_count!=8192||dut.raw_count!=n||dut.errors!=0)$fatal(1,"FIR count/error");
  expected_fir_state_cycles=(FIR_USE_SYMMETRY?((profile_taps+1)/2+3):(profile_taps+2))*8192;
  if(fir_state_cycles!=expected_fir_state_cycles)$fatal(1,"FIR cycle mismatch p%0d got%0d expected%0d",profile,fir_state_cycles,expected_fir_state_cycles);
  $display("FIR_UNIT_CYCLES profile=%0d kind=%0d symmetric=%0d fir_state_cycles=%0d start_to_mean_cycles=%0d",profile,kind,FIR_USE_SYMMETRY,fir_state_cycles,dut.cycles);
  force dut.state=4'd11;
  repeat(3)@(posedge clk);
  for(i=0;i<8192;i=i+1)for(ch=0;ch<3;ch=ch+1)begin
   read_result(i*3+ch,value);
   if(value!=={{40{expected[i][ch*24+23]}},expected[i][ch*24+:24]})$fatal(1,"FIR_UNIT RAM mismatch p%0d kind%0d sample%0d axis%0d got%h exp%h",profile,kind,i,ch,value,expected[i][ch*24+:24]);
   checked=checked+1;
  end
  for(ch=0;ch<3;ch=ch+1)for(field=0;field<6;field=field+1)begin
   read_result(32'h10000+ch*256+(field==5?7:field),value);
   if(value!==expected_stats[ch*6+field])$fatal(1,"FIR_UNIT stat mismatch p%0d kind%0d axis%0d field%0d got%h exp%h",profile,kind,ch,field,value,expected_stats[ch*6+field]);
  end
  release dut.state;write_reg(4,2);cases=cases+1;
  $display("FIR_UNIT_CASE_PASS profile=%0d kind=%0d raw=%0d sample_reads=24576",profile,kind,n);
 end
 $display("FIR_UNIT_PASS cases=%0d exact_sample_reads=%0d scope=FIR_ONLY_FFT_STUBBED",cases,checked);$finish;
end
initial begin #(64'd4000000000);$fatal(1,"FIR unit simulation timeout");end
endmodule

// Four-profile adversarial A/B run, using the legacy direct evaluator.
module fir_unit_direct_tb;
 fir_unit_tb #(.FIR_USE_SYMMETRY(0),.FIRST_KIND_DEFAULT(6)) unit();
endmodule

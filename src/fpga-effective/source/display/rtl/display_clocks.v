`timescale 1ns/1ps
// Two MMCMs are deliberate: 50 MHz cannot directly produce both required
// exact frequencies with the 7-series integer/fractional output restrictions.
module display_clocks(
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 clk50 CLK", X_INTERFACE_PARAMETER="FREQ_HZ 50000000" *)
    input wire clk50,
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 pixel_clk CLK", X_INTERFACE_PARAMETER="FREQ_HZ 74250000" *)
    output wire pixel_clk,
    (* X_INTERFACE_INFO="xilinx.com:signal:clock:1.0 serial_clk CLK", X_INTERFACE_PARAMETER="FREQ_HZ 371250000" *)
    output wire serial_clk, output wire locked);
    reg [7:0] power_on_reset=8'hff;
    wire clk_in, clk50_buf, fb1, fb1_buf, intermediate, intermediate_buf, lock1;
    always @(posedge clk50_buf) power_on_reset<={power_on_reset[6:0],1'b0};
    wire fb2, fb2_buf, pixel_unbuf, serial_unbuf, lock2;
    IBUF input_buf(.I(clk50),.O(clk_in));
    BUFG reference_buffer(.I(clk_in),.O(clk50_buf));
    // VCO=50*37.125/2=928.125 MHz, output=928.125/12.5=74.25 MHz.
    MMCME2_BASE #(.BANDWIDTH("OPTIMIZED"),.CLKIN1_PERIOD(20.0),
        .DIVCLK_DIVIDE(2),.CLKFBOUT_MULT_F(37.125),
        .CLKOUT0_DIVIDE_F(12.5),.STARTUP_WAIT("FALSE")) frequency_stage (
        .CLKIN1(clk50_buf),.CLKFBIN(fb1_buf),.CLKFBOUT(fb1),
        .CLKOUT0(intermediate),.LOCKED(lock1),.RST(power_on_reset[7]),.PWRDWN(1'b0));
    BUFG fb1_buffer(.I(fb1),.O(fb1_buf));
    BUFG intermediate_buffer(.I(intermediate),.O(intermediate_buf));
    // VCO=74.25*10=742.5 MHz. Both outputs now use integer dividers.
    MMCME2_BASE #(.BANDWIDTH("OPTIMIZED"),.CLKIN1_PERIOD(13.468013468),
        .DIVCLK_DIVIDE(1),.CLKFBOUT_MULT_F(10.0),
        .CLKOUT0_DIVIDE_F(10.0),.CLKOUT1_DIVIDE(2),.STARTUP_WAIT("FALSE")) serializer_stage (
        .CLKIN1(intermediate_buf),.CLKFBIN(fb2_buf),.CLKFBOUT(fb2),
        .CLKOUT0(pixel_unbuf),.CLKOUT1(serial_unbuf),
        .LOCKED(lock2),.RST(~lock1),.PWRDWN(1'b0));
    BUFG fb2_buffer(.I(fb2),.O(fb2_buf));
    BUFG pixel_buffer(.I(pixel_unbuf),.O(pixel_clk));
    BUFG serial_buffer(.I(serial_unbuf),.O(serial_clk));
    // Stage 2 is held in reset whenever stage 1 is unlocked. Its LOCKED
    // output therefore already covers the chain; do not combine lock nets
    // in a LUT that would directly drive asynchronous reset pins.
    assign locked=lock2;
endmodule


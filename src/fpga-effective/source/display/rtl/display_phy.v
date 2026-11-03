`timescale 1ns/1ps
// Video Out preserves the 24 stream bits. Linux little-endian XRGB8888 is
// B,G,R,X in memory -> AXIS[23:0]=R,G,B -> upstream rgb2dvi expects R,B,G.
module display_phy(
    (* X_INTERFACE_PARAMETER="FREQ_HZ 74250000" *) input wire pixel_clk,
    (* X_INTERFACE_PARAMETER="FREQ_HZ 371250000" *) input wire serial_clk,
    input wire pixel_reset,
    input wire [23:0] rgb, input wire de, input wire hsync, input wire vsync,
    (* X_INTERFACE_PARAMETER="FREQ_HZ 74250000" *) output wire hdmi_clk_p,
    (* X_INTERFACE_PARAMETER="FREQ_HZ 74250000" *) output wire hdmi_clk_n,
    output wire [2:0] hdmi_data_p, output wire [2:0] hdmi_data_n
);
    wire [23:0] dvi_rbg={rgb[23:16],rgb[7:0],rgb[15:8]};
    rgb2dvi #(.kGenerateSerialClk(0),.kRstActiveHigh(1)) transmitter(
        .TMDS_Clk_p(hdmi_clk_p),.TMDS_Clk_n(hdmi_clk_n),
        .TMDS_Data_p(hdmi_data_p),.TMDS_Data_n(hdmi_data_n),
        .aRst(pixel_reset),.aRst_n(1'b1),
        .vid_pData(dvi_rbg),
        .vid_pVDE(de),.vid_pHSync(hsync),.vid_pVSync(vsync),
        .PixelClk(pixel_clk),.SerialClk(serial_clk));
endmodule

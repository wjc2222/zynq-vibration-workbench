`timescale 1ns/1ps
// GPIO channel 1: bits[0..5]=MMCM lock, VideoOut lock, live under/overflow,
// sticky under/overflow; bits[31:16]=real output VSYNC rising-edge count.
// GPIO channel 2 control: bit0 holds the separate video reset controllers;
// bit1 clears sticky errors (assert through initial lock acquisition).
module display_status(
    (* X_INTERFACE_PARAMETER="FREQ_HZ 100000000" *) input wire axi_clk,
    input wire axi_resetn,
    (* X_INTERFACE_PARAMETER="FREQ_HZ 74250000" *) input wire pixel_clk,
    input wire pixel_resetn,
    input wire mmcm_locked, input wire video_locked,
    input wire underflow, input wire overflow, input wire vsync,
    input wire [31:0] control, output wire [31:0] status,
    output wire video_hold
);
    assign video_hold=control[0];
    (* ASYNC_REG="TRUE" *) reg [1:0] clear_sync=0;
    reg sticky_under=0,sticky_over=0,vsync_d=0;
    reg [15:0] frame_count=0,frame_gray=0;
    always @(posedge pixel_clk) begin
        if(!pixel_resetn) begin
            clear_sync<=0;sticky_under<=0;
            vsync_d<=0;frame_count<=0;frame_gray<=0;
        end else begin
            clear_sync<={clear_sync[0],control[1]};
            if(clear_sync[1]) sticky_under<=0;
            else begin
                if(underflow) sticky_under<=1;
            end
            vsync_d<=vsync;
            if(vsync && !vsync_d) begin
                frame_count<=frame_count+1'b1;
                frame_gray<=((frame_count+1'b1)>>1)^(frame_count+1'b1);
            end
        end
    end
    (* ASYNC_REG="TRUE" *) reg [5:0] flags_meta=0,flags_sync=0;
    (* ASYNC_REG="TRUE" *) reg [15:0] gray_meta=0,gray_sync=0;
    always @(posedge axi_clk) begin
        if(!axi_resetn) begin flags_meta<=0;flags_sync<=0;gray_meta<=0;gray_sync<=0;sticky_over<=0;end
        else begin
            // The installed AMD FIFO implementation asserts overflow in
            // write/AXI domain and underflow in read/pixel domain.
            if(control[1]) sticky_over<=0;
            else if(overflow) sticky_over<=1;
            flags_meta<={sticky_over,sticky_under,overflow,underflow,video_locked,mmcm_locked};
            flags_sync<=flags_meta;
            gray_meta<=frame_gray;gray_sync<=gray_meta;
        end
    end
    function [15:0] gray_to_binary;
        input [15:0] gray;
        integer i;
        begin
            gray_to_binary[15]=gray[15];
            for(i=14;i>=0;i=i-1) gray_to_binary[i]=gray_to_binary[i+1]^gray[i];
        end
    endfunction
    assign status={gray_to_binary(gray_sync),10'b0,flags_sync};
endmodule

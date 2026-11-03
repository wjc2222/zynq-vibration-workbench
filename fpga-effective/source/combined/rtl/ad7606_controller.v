`timescale 1ns/1ps
// AD7606 (8-channel original device), parallel 16-bit, OS=000, +/-5 V.
module ad7606_controller #(
    parameter integer CLK_HZ = 50000000,
    parameter integer SAMPLE_RATE_HZ = 50000,
    parameter integer EVEN_CHANNELS = 0,
    parameter integer STARTUP_CYCLES = 2500000
)(
    input wire clk, input wire rst,
    input wire [31:0] sample_period_cycles,
    input wire [15:0] ad_data,
    input wire ad_busy, input wire ad_firstdata,
    output reg ad_reset, output reg ad_cs_n, output reg ad_rd_n,
    output reg ad_convsta,
    output wire ad_convstb, output wire ad_range,
    output wire ad_par_ser, output wire ad_stby_n,
    output reg signed [15:0] raw_ch1, raw_ch2, raw_ch3, raw_ch4,
    output reg signed [31:0] uv_ch1, uv_ch2, uv_ch3, uv_ch4,
    output reg frame_valid, output reg [31:0] frame_count,
    output wire [15:0] debug_status,
    output wire sample_trigger
);
    // Runtime period is committed only while capture is idle; controller must
    // be reset after a rate change. Timing remains at the physical50MHz clock.
    wire [31:0] PERIOD = sample_period_cycles;
    // Round up: 1 us reset, 1 us recovery, 100 ns CONVST,
    // 200 ns RD-low, 100 ns RD-high, 100 ns CS setup/hold.
    localparam integer RESET_N = (CLK_HZ + 999999) / 1000000;
    localparam integer SHORT_N = (CLK_HZ + 9999999) / 10000000;
    localparam integer READ_N = (CLK_HZ + 4999999) / 5000000;
    localparam integer TIMEOUT_N = (CLK_HZ + 9999) / 10000; // 100 us
    localparam [3:0] STARTUP=0, RESET_HIGH=1, RECOVER=2, IDLE=3,
        CONV_HIGH=4, WAIT_RISE=5, WAIT_FALL=6, CS_SETUP=7,
        READ_LOW=8, READ_HIGH=9, PUBLISH=10, FAULT=11;
    reg [3:0] state;
    reg [31:0] timer, period_count;
    reg [2:0] read_index;
    reg seen_busy, frame_bad;
    reg timeout_error, firstdata_error, overrun_error;
    reg signed [15:0] stage1, stage2, stage3, stage4;
    // True on the SAME clock edge that launches CONVSTA/B. Timestamp here,
    // not at frame_valid (which occurs after conversion and readout).
    (* ASYNC_REG = "TRUE" *) reg busy_meta, busy_sync;
    assign sample_trigger = !rst && state == IDLE && period_count == PERIOD-1 && !busy_sync;
    always @(posedge clk) begin
        if (rst) begin busy_meta<=0; busy_sync<=0; end
        else begin busy_meta<=ad_busy; busy_sync<=busy_meta; end
    end
    assign ad_convstb = ad_convsta;
    assign ad_range = 1'b0;
    assign ad_par_ser = 1'b0;
    assign ad_stby_n = 1'b1;
    assign debug_status = {timeout_error,firstdata_error,overrun_error,
        busy_sync,ad_reset,ad_cs_n,ad_rd_n,ad_convsta,state,read_index,frame_valid};
    function signed [31:0] to_uv;
        input signed [15:0] code;
        reg signed [47:0] product;
        begin
            product = code * 48'sd5000000;
            to_uv = product / 48'sd32768; // signed, truncate toward zero
        end
    endfunction
    // synthesis translate_off
    initial begin
        if (CLK_HZ != 50000000) $fatal(1,"This board/XDC requires 50 MHz");
        if (SAMPLE_RATE_HZ < 1 || SAMPLE_RATE_HZ > 100000)
            $fatal(1,"Supported sample rate: 1..100000 samples/s");
        if (CLK_HZ % SAMPLE_RATE_HZ != 0)
            $fatal(1,"Choose a sample rate that divides CLK_HZ exactly");
        if (EVEN_CHANNELS != 0 && EVEN_CHANNELS != 1) $fatal(1,"EVEN_CHANNELS must be 0 or 1");
    end
    // synthesis translate_on
    always @(posedge clk) begin
        if (rst) begin
            state<=STARTUP; timer<=0; period_count<=0; read_index<=0;
            ad_reset<=0; ad_cs_n<=1; ad_rd_n<=1; ad_convsta<=0;
            raw_ch1<=0; raw_ch2<=0; raw_ch3<=0; raw_ch4<=0;
            uv_ch1<=0; uv_ch2<=0; uv_ch3<=0; uv_ch4<=0;
            stage1<=0; stage2<=0; stage3<=0; stage4<=0;
            frame_valid<=0; frame_count<=0; seen_busy<=0; frame_bad<=0;
            timeout_error<=0; firstdata_error<=0; overrun_error<=0;
        end else begin
            frame_valid<=0;
            // Conversion-to-conversion timer; no accumulated readout delay.
            if (state >= IDLE && state < FAULT) begin
                if (period_count < PERIOD-1) period_count<=period_count+1;
                if (period_count == PERIOD-1 && state != IDLE) overrun_error<=1;
            end
            case (state)
                STARTUP: if (timer == STARTUP_CYCLES-1) begin
                    timer<=0; ad_reset<=1; state<=RESET_HIGH;
                end else timer<=timer+1;
                RESET_HIGH: if (timer == RESET_N-1) begin
                    ad_reset<=0; timer<=0; state<=RECOVER;
                end else timer<=timer+1;
                RECOVER: if (timer == RESET_N-1) begin
                    timer<=0; period_count<=PERIOD-1; state<=IDLE;
                end else timer<=timer+1;
                IDLE: if (period_count == PERIOD-1) begin
                    if (busy_sync) begin timeout_error<=1; state<=FAULT; end
                    else begin
                        ad_convsta<=1; period_count<=0; timer<=0;
                        seen_busy<=0; frame_bad<=0; state<=CONV_HIGH;
                    end
                end
                CONV_HIGH: begin
                    if (busy_sync) seen_busy<=1;
                    if (timer == SHORT_N-1) begin
                        ad_convsta<=0; timer<=0; state<=WAIT_RISE;
                    end else timer<=timer+1;
                end
                WAIT_RISE: if (seen_busy || busy_sync) begin
                    timer<=0; state<=WAIT_FALL;
                end else if (timer == TIMEOUT_N-1) begin
                    timeout_error<=1; state<=FAULT;
                end else timer<=timer+1;
                WAIT_FALL: if (!busy_sync) begin
                    ad_cs_n<=0; timer<=0; read_index<=0; state<=CS_SETUP;
                end else if (timer == TIMEOUT_N-1) begin
                    timeout_error<=1; state<=FAULT;
                end else timer<=timer+1;
                CS_SETUP: if (timer == SHORT_N-1) begin
                    ad_rd_n<=0; timer<=0; state<=READ_LOW;
                end else timer<=timer+1;
                READ_LOW: if (timer == READ_N-1) begin
                    // Capture the whole bus together after 200 ns settling.
                    // RD rises only after this sampling edge (registered output).
                    if (ad_firstdata != (read_index == 0)) begin
                        frame_bad<=1; firstdata_error<=1;
                    end
                    if (read_index[0] == (EVEN_CHANNELS != 0)) begin
                        case (read_index[2:1])
                            0: stage1<=ad_data;
                            1: stage2<=ad_data;
                            2: stage3<=ad_data;
                            3: stage4<=ad_data;
                        endcase
                    end
                    ad_rd_n<=1; timer<=0; state<=READ_HIGH;
                end else timer<=timer+1;
                READ_HIGH: if (timer == SHORT_N-1) begin
                    timer<=0;
                    if (read_index == 7) begin ad_cs_n<=1; state<=PUBLISH; end
                    else begin read_index<=read_index+1; ad_rd_n<=0; state<=READ_LOW; end
                end else timer<=timer+1;
                PUBLISH: begin
                    if (!frame_bad) begin
                        raw_ch1<=stage1; raw_ch2<=stage2; raw_ch3<=stage3; raw_ch4<=stage4;
                        uv_ch1<=to_uv(stage1); uv_ch2<=to_uv(stage2);
                        uv_ch3<=to_uv(stage3); uv_ch4<=to_uv(stage4);
                        frame_valid<=1; frame_count<=frame_count+1;
                        state<=IDLE;
                    end else state<=FAULT;
                end
                FAULT: begin
                    ad_cs_n<=1; ad_rd_n<=1; ad_convsta<=0; ad_reset<=0;
                    // Stop on invalid framing or BUSY timeout. Reprogram to retry.
                end
                default: state<=FAULT;
            endcase
        end
    end
endmodule


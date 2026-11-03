`timescale 1ns/1ps
// Legacy finite capture, or explicitly selected packetized continuous capture.
// A packet boundary never resets the original 64-bit record index/timestamp.
// An overflow/ADC error invalidates the capture. NEVER silently overwrite.
module capture_stream #(parameter integer FIFO_DEPTH=256)(
    input wire clk, rst, start, abort_capture,
    input wire [31:0] target_samples,
    input wire stream_mode, stop_stream,
    input wire [63:0] total_records,
    input wire [31:0] chunk_records,
    input wire sample_trigger, frame_valid, adc_fault,
    input wire signed [15:0] raw_x, raw_y, raw_z,
    input wire [31:0] speed_period, speed_edges,
    input wire speed_valid, speed_timeout, speed_level, speed_filtered_level,
    output reg active, source_done, stream_done, overflow, adc_error, config_error,
    output wire idle,
    output reg [31:0] captured, sent, dropped,
    output reg [63:0] captured64, sent64, dropped64, stop_records,
    output reg [63:0] first_tick, last_tick,
    output reg stop_pending,
    output reg [63:0] ticks,
    output wire [63:0] m_axis_tdata,
    output wire [7:0] m_axis_tkeep,
    output wire m_axis_tvalid, m_axis_tlast,
    input wire m_axis_tready
);
    localparam AW=$clog2(FIFO_DEPTH);
    // bit256: AXIS packet end; bit257: final record of the complete run.
    reg [257:0] memory [0:FIFO_DEPTH-1];
    reg [AW-1:0] wr_ptr, rd_ptr;
    reg [AW:0] used;
    reg [257:0] output_record;
    reg output_valid;
    reg [1:0] beat;
    reg [31:0] target;
    reg continuous;
    reg [63:0] stream_target;
    reg [31:0] chunk, chunk_position;
    reg [63:0] trigger_index, pending_index, pending_tick;
    reg [31:0] pending_period, pending_edges;
    reg [15:0] pending_flags;
    reg pending;
    assign idle = !active && !pending && !output_valid && used == 0;
    wire pop = !output_valid && used != 0;
    wire room = used < FIFO_DEPTH || pop;
    wire candidate = active && pending && frame_valid && !adc_fault;
    wire push = candidate && room;
    wire target_end = continuous ? (stream_target != 0 && captured64 == stream_target-1) : captured == target-1;
    wire chunk_end = continuous && chunk_position == chunk-1;
    wire final_record = target_end || (chunk_end && (stop_pending || stop_stream));
    wire packet_end = continuous ? (chunk_end || target_end) : target_end;
    wire [63:0] requested_stop = captured64 + {32'b0,chunk-chunk_position};
    assign m_axis_tdata = output_record[255:0] >> (beat*64);
    assign m_axis_tkeep = 8'hff;
    assign m_axis_tvalid = output_valid;
    assign m_axis_tlast = output_record[256] && beat == 3 && output_valid;
    always @(posedge clk) begin
        if (rst) begin
            ticks<=0; active<=0; source_done<=0; stream_done<=0;
            overflow<=0; adc_error<=0; config_error<=0; captured<=0; sent<=0; dropped<=0;
            captured64<=0; sent64<=0; dropped64<=0; stop_records<=0;
            first_tick<=0; last_tick<=0; stop_pending<=0;
            target<=0; trigger_index<=0; pending<=0; pending_index<=0; pending_tick<=0;
            continuous<=0; stream_target<=0; chunk<=0; chunk_position<=0;
            pending_period<=0; pending_edges<=0; pending_flags<=0;
            wr_ptr<=0; rd_ptr<=0; used<=0; output_record<=0; output_valid<=0; beat<=0;
        end else begin
            ticks<=ticks+1;
            if (abort_capture) begin
                active<=0; pending<=0; used<=0; wr_ptr<=0; rd_ptr<=0; output_valid<=0; beat<=0;
                source_done<=0; stream_done<=0;
                stop_pending<=0;
            end else if (start && !active && !pending && !output_valid && used==0 &&
                         (stream_mode ? chunk_records!=0 : target_samples!=0)) begin
                // Software must reset/arm DMA before issuing START.
                begin
                    active<=1; source_done<=0; stream_done<=0; overflow<=0; adc_error<=0; config_error<=0;
                    captured<=0; sent<=0; dropped<=0; trigger_index<=0; pending<=0;
                    captured64<=0; sent64<=0; dropped64<=0; first_tick<=0; last_tick<=0;
                    continuous<=stream_mode; stream_target<=total_records;
                    chunk<=chunk_records; chunk_position<=0; stop_pending<=0;
                    stop_records<=stream_mode ? total_records : {32'b0,target_samples};
                    target<=target_samples; wr_ptr<=0; rd_ptr<=0; used<=0; beat<=0;
                end
            end else begin
                if (start) config_error<=1;
                if (continuous && active && stop_stream && !stop_pending) begin
                    stop_pending<=1;
                    stop_records <= stream_target!=0 && stream_target<requested_stop ? stream_target : requested_stop;
                end
                case ({push,pop})
                    2'b10: used<=used+1;
                    2'b01: used<=used-1;
                    default: ;
                endcase
                if (pop) begin
                    output_record<=memory[rd_ptr]; rd_ptr<=rd_ptr+1'b1; output_valid<=1; beat<=0;
                end
                if (output_valid && m_axis_tready) begin
                    if (beat==3) begin
                        output_valid<=0; beat<=0; sent<=sent+1; sent64<=sent64+1;
                        if (output_record[257]) stream_done<=1;
                    end else beat<=beat+1'b1;
                end
                if (sample_trigger && active &&
                    (continuous ? (stream_target==0 || trigger_index<stream_target) : trigger_index<target)) begin
                    pending<=1; pending_index<=trigger_index; pending_tick<=ticks;
                    pending_period<=speed_period; pending_edges<=speed_edges;
                    // Atomic snapshot at CONVST, NOT when ADC readout ends.
                    // Keep both synchronized raw level and debounced level.
                    pending_flags<={11'b0,speed_filtered_level,speed_level,speed_timeout,speed_valid,1'b1};
                    trigger_index<=trigger_index+1;
                end
                if (candidate) begin
                    pending<=0;
                    if (!room) begin overflow<=1; dropped<=dropped+1; dropped64<=dropped64+1; active<=0; end
                    else begin
                        memory[wr_ptr]<={final_record,packet_end,pending_edges,pending_period,pending_flags,
                            raw_z,raw_y,raw_x,pending_tick,pending_index};
                        wr_ptr<=wr_ptr+1'b1; captured<=captured+1; captured64<=captured64+1;
                        if (captured64==0) first_tick<=pending_tick;
                        last_tick<=pending_tick;
                        if (packet_end) chunk_position<=0;
                        else if (continuous) chunk_position<=chunk_position+1;
                        if (final_record) begin active<=0; source_done<=1; end
                    end
                end
                if (active && adc_fault) begin adc_error<=1; active<=0; pending<=0; end
            end
        end
    end
endmodule


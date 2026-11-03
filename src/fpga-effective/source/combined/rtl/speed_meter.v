`timescale 1ns/1ps
// One conditioned 3.3 V input. Measure pulse timing, NOT RPM: pulses per
// revolution cannot be inferred from this electrical signal alone.
module speed_meter #(
    parameter integer FILTER_CYCLES=5,
    parameter integer TIMEOUT_CYCLES=100000000
)(
    input wire clk, rst, speed_in,
    output reg [31:0] period_ticks, edge_count,
    output reg period_valid, timed_out,
    output reg edge_pulse,
    output wire synchronized_level, filtered_level
);
    (* ASYNC_REG="TRUE" *) reg speed_meta, speed_sync;
    reg level, low_seen, have_edge;
    assign synchronized_level = speed_sync;
    assign filtered_level = level;
    reg [31:0] stable_count, low_count, age;
    wire qualified_change = speed_sync != level && stable_count == FILTER_CYCLES-1;
    wire rising = qualified_change && speed_sync && low_seen;
    always @(posedge clk) begin
        if (rst) begin
            speed_meta<=0; speed_sync<=0; level<=0; low_seen<=0;
            stable_count<=0; low_count<=0; age<=0; have_edge<=0;
            period_ticks<=0; edge_count<=0; period_valid<=0; timed_out<=1; edge_pulse<=0;
        end else begin
            speed_meta<=speed_in; speed_sync<=speed_meta; edge_pulse<=0;
            // Require a qualified LOW before the first edge, avoiding a fake
            // pulse when reset is released while SPEED is already high.
            if (!speed_sync) begin
                if (low_count < FILTER_CYCLES-1) low_count<=low_count+1;
                else low_seen<=1;
            end else low_count<=0;
            if (speed_sync == level) stable_count<=0;
            else if (qualified_change) begin level<=speed_sync; stable_count<=0; end
            else stable_count<=stable_count+1;
            if (have_edge) begin
                if (age < TIMEOUT_CYCLES-1) age<=age+1;
                else begin have_edge<=0; period_valid<=0; timed_out<=1; period_ticks<=0; end
            end
            if (rising) begin
                edge_count<=edge_count+1; edge_pulse<=1; age<=0;
                if (have_edge && age < TIMEOUT_CYCLES-1) begin
                    period_ticks<=age+1; period_valid<=1; timed_out<=0;
                end else begin period_ticks<=0; period_valid<=0; timed_out<=0; end
                have_edge<=1;
            end
        end
    end
endmodule

`timescale 1ns / 1ps

// Convert XDMA H2C 64-bit words from RK3588 into one 16-bit OCT sample stream.
//
// H2C word layout is little-endian:
//   tdata[15:0]   sample 0
//   tdata[31:16]  sample 1
//   tdata[47:32]  sample 2
//   tdata[63:48]  sample 3
//
// XDMA H2C tlast is not used as an OCT A-line boundary because the Linux XDMA
// driver may split one frame into arbitrary DMA chunks. The FPGA defines one
// A-line as SAMPLES_PER_LINE accepted 16-bit samples.
module ascent_h2c64_to_u16_axis #(
    parameter integer SAMPLES_PER_LINE = 2048
) (
    input  wire        aclk,
    input  wire        aresetn,
    input  wire        clear_counters,

    input  wire [63:0] s_axis_tdata,
    input  wire [7:0]  s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire [15:0] m_axis_tdata,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,
    output wire        m_axis_tuser,

    output reg  [31:0] dbg_words,
    output reg  [31:0] dbg_samples,
    output reg  [31:0] dbg_lines
);

    localparam integer SAMPLE_COUNT_W = (SAMPLES_PER_LINE > 1) ?
                                        $clog2(SAMPLES_PER_LINE) : 1;
    localparam [SAMPLE_COUNT_W-1:0] LINE_LAST_SAMPLE = SAMPLES_PER_LINE - 1;

    reg [63:0] word_reg;
    reg [7:0]  keep_reg;
    reg        word_valid;
    reg [1:0]  lane_idx;
    reg [SAMPLE_COUNT_W-1:0] line_sample_idx;

    wire lane0_valid = &keep_reg[1:0];
    wire lane1_valid = &keep_reg[3:2];
    wire lane2_valid = &keep_reg[5:4];
    wire lane3_valid = &keep_reg[7:6];
    wire any_input_sample = (&s_axis_tkeep[1:0]) |
                            (&s_axis_tkeep[3:2]) |
                            (&s_axis_tkeep[5:4]) |
                            (&s_axis_tkeep[7:6]);

    wire current_lane_valid =
        (lane_idx == 2'd0) ? lane0_valid :
        (lane_idx == 2'd1) ? lane1_valid :
        (lane_idx == 2'd2) ? lane2_valid :
                              lane3_valid;

    wire [1:0] last_valid_lane =
        lane3_valid ? 2'd3 :
        lane2_valid ? 2'd2 :
        lane1_valid ? 2'd1 :
                      2'd0;

    wire output_fire = m_axis_tvalid & m_axis_tready;
    wire output_last = (line_sample_idx == LINE_LAST_SAMPLE);
    wire word_done   = output_fire & (lane_idx == last_valid_lane);
    wire _unused_s_axis_tlast = s_axis_tlast;

    assign s_axis_tready = ~word_valid;
    assign m_axis_tvalid = word_valid & current_lane_valid;
    assign m_axis_tdata  =
        (lane_idx == 2'd0) ? word_reg[15:0] :
        (lane_idx == 2'd1) ? word_reg[31:16] :
        (lane_idx == 2'd2) ? word_reg[47:32] :
                              word_reg[63:48];
    assign m_axis_tlast = m_axis_tvalid & output_last;
    assign m_axis_tuser = m_axis_tvalid & (line_sample_idx == {SAMPLE_COUNT_W{1'b0}});

    always @(posedge aclk) begin
        if (!aresetn) begin
            word_reg    <= 64'd0;
            keep_reg    <= 8'd0;
            word_valid  <= 1'b0;
            lane_idx    <= 2'd0;
            line_sample_idx <= {SAMPLE_COUNT_W{1'b0}};
            dbg_words   <= 32'd0;
            dbg_samples <= 32'd0;
            dbg_lines   <= 32'd0;
        end else begin
            if (clear_counters) begin
                dbg_words   <= 32'd0;
                dbg_samples <= 32'd0;
                dbg_lines   <= 32'd0;
                line_sample_idx <= {SAMPLE_COUNT_W{1'b0}};
            end

            if (s_axis_tvalid && s_axis_tready) begin
                dbg_words <= dbg_words + 1'b1;

                if (any_input_sample) begin
                    word_reg   <= s_axis_tdata;
                    keep_reg   <= s_axis_tkeep;
                    word_valid <= 1'b1;
                    lane_idx   <= 2'd0;
                end else if (s_axis_tlast) begin
                    // Empty DMA packets do not carry OCT samples and therefore
                    // do not advance the A-line counter.
                end
            end

            if (output_fire) begin
                dbg_samples <= dbg_samples + 1'b1;

                if (output_last) begin
                    dbg_lines  <= dbg_lines + 1'b1;
                    line_sample_idx <= {SAMPLE_COUNT_W{1'b0}};
                end else begin
                    line_sample_idx <= line_sample_idx + 1'b1;
                end

                if (word_done) begin
                    word_valid <= 1'b0;
                    lane_idx   <= 2'd0;
                end else begin
                    lane_idx <= lane_idx + 1'b1;
                end
            end
        end
    end

endmodule

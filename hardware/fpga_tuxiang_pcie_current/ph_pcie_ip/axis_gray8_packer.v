`timescale 1ns / 1ps

module axis_u16_packer #(
    parameter integer VALID_SAMPLES = 2048,
    parameter integer FFT_SIZE      = 2048
) (
    input  wire         aclk,
    input  wire         aresetn,

    input  wire [31:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,
    input  wire         s_axis_tlast,
    input  wire         s_axis_tuser,

    output reg  [63:0]  m_axis_tdata,
    output reg  [7:0]   m_axis_tkeep,
    output reg          m_axis_tvalid,
    input  wire         m_axis_tready,
    output reg          m_axis_tlast,
    output reg          m_axis_tuser,

    output reg          core_frame_done_pulse
);

    localparam integer COUNT_W = (FFT_SIZE > 1) ? $clog2(FFT_SIZE) : 1;

    reg               first_out_pending;
    reg [COUNT_W-1:0] sample_idx;
    reg [63:0]        pack_reg;
    reg [2:0]         word_count;

    wire output_ready = (~m_axis_tvalid) | m_axis_tready;
    wire [63:0] pixel_shifted   = {{48{1'b0}}, s_axis_tdata[15:0]} << (word_count * 16);
    wire [63:0] pack_with_pixel = pack_reg | pixel_shifted;
    wire [2:0]  next_word_count = word_count + 1'b1;
    wire        count_line_end   = (sample_idx == VALID_SAMPLES - 1);
    wire        input_line_end   = s_axis_tlast | count_line_end;
    wire        emit_word        = s_axis_tvalid &
                                   ((next_word_count == 3'd4) | input_line_end);
    wire _unused_tuser = s_axis_tuser;

    assign s_axis_tready = output_ready;

    function [7:0] keep_mask;
        input [2:0] count;
        begin
            case (count)
                3'd1: keep_mask = 8'h03;
                3'd2: keep_mask = 8'h0F;
                3'd3: keep_mask = 8'h3F;
                default: keep_mask = 8'hFF;
            endcase
        end
    endfunction

    always @(posedge aclk) begin
        if (!aresetn) begin
            first_out_pending     <= 1'b1;
            sample_idx            <= {COUNT_W{1'b0}};
            pack_reg              <= 64'd0;
            word_count            <= 3'd0;
            m_axis_tdata          <= 64'd0;
            m_axis_tkeep          <= 8'd0;
            m_axis_tvalid         <= 1'b0;
            m_axis_tlast          <= 1'b0;
            m_axis_tuser          <= 1'b0;
            core_frame_done_pulse <= 1'b0;
        end else begin
            core_frame_done_pulse <= 1'b0;

            if (output_ready) begin
                if (m_axis_tvalid && m_axis_tready) begin
                    m_axis_tvalid <= 1'b0;
                    m_axis_tlast  <= 1'b0;
                    m_axis_tuser  <= 1'b0;
                end

                if (s_axis_tvalid) begin
                    if (emit_word) begin
                        m_axis_tdata  <= pack_with_pixel;
                        m_axis_tkeep  <= input_line_end ? keep_mask(next_word_count) : 8'hFF;
                        m_axis_tvalid <= 1'b1;
                        m_axis_tlast  <= input_line_end;
                        m_axis_tuser  <= first_out_pending;
                    end

                    if (input_line_end) begin
                        core_frame_done_pulse <= 1'b1;
                        sample_idx            <= {COUNT_W{1'b0}};
                        pack_reg              <= 64'd0;
                        word_count            <= 3'd0;
                        first_out_pending     <= 1'b1;
                    end else if (emit_word) begin
                        sample_idx        <= sample_idx + 1'b1;
                        pack_reg          <= 64'd0;
                        word_count        <= 3'd0;
                        first_out_pending <= 1'b0;
                    end else begin
                        sample_idx <= sample_idx + 1'b1;
                        pack_reg   <= pack_with_pixel;
                        word_count <= next_word_count;
                    end
                end
            end
        end
    end

endmodule

// Compatibility wrapper for older source lists and the historical file name.
// The current PCIe path returns 16-bit pixels, not 8-bit grayscale pixels.
module axis_gray8_packer #(
    parameter integer VALID_SAMPLES = 2048,
    parameter integer FFT_SIZE      = 2048
) (
    input  wire         aclk,
    input  wire         aresetn,

    input  wire [31:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,
    input  wire         s_axis_tlast,
    input  wire         s_axis_tuser,

    output wire [63:0]  m_axis_tdata,
    output wire [7:0]   m_axis_tkeep,
    output wire         m_axis_tvalid,
    input  wire         m_axis_tready,
    output wire         m_axis_tlast,
    output wire         m_axis_tuser,

    output wire         core_frame_done_pulse
);

    axis_u16_packer #(
        .VALID_SAMPLES(VALID_SAMPLES),
        .FFT_SIZE(FFT_SIZE)
    ) u_axis_u16_packer (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(s_axis_tdata),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .s_axis_tuser(s_axis_tuser),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast),
        .m_axis_tuser(m_axis_tuser),
        .core_frame_done_pulse(core_frame_done_pulse)
    );

endmodule

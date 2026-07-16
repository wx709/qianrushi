`timescale 1ns / 1ps

module axis_real_to_fft_frame #(
    parameter integer LINE_SAMPLES = 2048,
    parameter integer FFT_SIZE     = 2048
) (
    input  wire         aclk,
    input  wire         aresetn,

    input  wire [15:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,
    input  wire         s_axis_tlast,
    input  wire         s_axis_tuser,

    output reg  [31:0]  m_axis_tdata,
    output reg          m_axis_tvalid,
    input  wire         m_axis_tready,
    output reg          m_axis_tlast,
    output reg          m_axis_tuser,

    output reg          frame_start_pulse
);

    localparam integer COUNT_W = (FFT_SIZE > 1) ? $clog2(FFT_SIZE) : 1;

    reg [COUNT_W-1:0] emit_idx;

    wire pipe_ready = (~m_axis_tvalid) | m_axis_tready;
    wire in_data_phase = (emit_idx < LINE_SAMPLES);
    wire _unused_tlast = s_axis_tlast;
    wire _unused_tuser = s_axis_tuser;

    assign s_axis_tready = pipe_ready & in_data_phase;

    always @(posedge aclk) begin
        if (!aresetn) begin
            emit_idx          <= {COUNT_W{1'b0}};
            m_axis_tdata      <= 32'd0;
            m_axis_tvalid     <= 1'b0;
            m_axis_tlast      <= 1'b0;
            m_axis_tuser      <= 1'b0;
            frame_start_pulse <= 1'b0;
        end else begin
            frame_start_pulse <= 1'b0;

            if (pipe_ready) begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tuser  <= 1'b0;

                if (in_data_phase) begin
                    if (s_axis_tvalid) begin
                        m_axis_tdata      <= {16'd0, s_axis_tdata};
                        m_axis_tvalid     <= 1'b1;
                        m_axis_tlast  <= (emit_idx == FFT_SIZE - 1);
                        m_axis_tuser      <= (emit_idx == {COUNT_W{1'b0}});
                        frame_start_pulse <= (emit_idx == {COUNT_W{1'b0}});

                        if (emit_idx == FFT_SIZE - 1)
                            emit_idx <= {COUNT_W{1'b0}};
                        else
                            emit_idx <= emit_idx + 1'b1;
                    end
                end
            end
        end
    end

endmodule

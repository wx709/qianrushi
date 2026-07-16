`timescale 1ns / 1ps

// ============================================================================
// Module: axis_line_to_frame
//
// Converts per-line AXI4-Stream output (tlast = end of each line) to per-frame
// output (tlast = end of frame after NUM_LINES lines). tuser marks the first
// beat of the frame.
//
// Parameters:
//   NUM_LINES  - Number of lines per complete frame (default 4096)
//   DATA_W     - AXI4-Stream data width (default 64)
//
// Updated 2026-06-20: New module for frame-level PCIe streaming.
// ============================================================================
module axis_line_to_frame #(
    parameter integer NUM_LINES = 4096,
    parameter integer DATA_W    = 64
) (
    input  wire                 aclk,
    input  wire                 aresetn,

    // Per-line input stream (tlast = end of each processed line)
    input  wire [DATA_W-1:0]    s_axis_tdata,
    input  wire [7:0]           s_axis_tkeep,
    input  wire                 s_axis_tvalid,
    output wire                 s_axis_tready,
    input  wire                 s_axis_tlast,
    input  wire                 s_axis_tuser,

    // Per-frame output stream (tlast = end of frame)
    output reg  [DATA_W-1:0]    m_axis_tdata,
    output reg  [7:0]           m_axis_tkeep,
    output reg                  m_axis_tvalid,
    input  wire                 m_axis_tready,
    output reg                  m_axis_tlast,
    output reg                  m_axis_tuser
);

    localparam integer LINE_W = (NUM_LINES > 1) ? $clog2(NUM_LINES) : 1;

    reg [LINE_W-1:0] line_count;
    reg              frame_active;
    reg              first_beat_pending;

    wire in_fire  = s_axis_tvalid & s_axis_tready;
    wire out_fire = m_axis_tvalid & m_axis_tready;
    wire pipe_stall = m_axis_tvalid & ~m_axis_tready;

    assign s_axis_tready = ~pipe_stall;

    always @(posedge aclk) begin
        if (!aresetn) begin
            line_count         <= {LINE_W{1'b0}};
            frame_active       <= 1'b0;
            first_beat_pending <= 1'b1;
            m_axis_tdata       <= {DATA_W{1'b0}};
            m_axis_tkeep       <= 8'd0;
            m_axis_tvalid      <= 1'b0;
            m_axis_tlast       <= 1'b0;
            m_axis_tuser       <= 1'b0;
        end else begin
            if (!pipe_stall) begin
                // Default: clear output valid
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tuser  <= 1'b0;

                if (in_fire) begin
                    // Pass through data and tkeep
                    m_axis_tdata  <= s_axis_tdata;
                    m_axis_tkeep  <= s_axis_tkeep;
                    m_axis_tvalid <= 1'b1;

                    // tuser on first beat of the frame (line 0, first output)
                    if (first_beat_pending) begin
                        m_axis_tuser <= 1'b1;
                        first_beat_pending <= 1'b0;
                        frame_active <= 1'b1;
                    end

                    // tlast only on last beat of last line
                    if (s_axis_tlast) begin
                        if (line_count == NUM_LINES - 1) begin
                            m_axis_tlast  <= 1'b1;
                            line_count    <= {LINE_W{1'b0}};
                            frame_active  <= 1'b0;
                            first_beat_pending <= 1'b1;
                        end else begin
                            line_count <= line_count + 1'b1;
                        end
                    end
                end
            end
        end
    end

endmodule

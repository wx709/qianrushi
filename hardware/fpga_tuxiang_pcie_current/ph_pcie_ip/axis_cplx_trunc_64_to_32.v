`timescale 1ns / 1ps

// AXIS complex truncation: 64-bit -> 32-bit
// In format : s_axis_tdata = {im[31:0], re[31:0]}
// Out format: m_axis_tdata = {im[15:0], re[15:0]}
// Slice position is tunable to balance dynamic range and quantization noise.
module axis_cplx_trunc_64_to_32 #(
    parameter integer TRUNC_LSB = 12
) (
    input  wire         aclk,
    input  wire         aresetn,

    input  wire [63:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,
    input  wire         s_axis_tlast,
    input  wire         s_axis_tuser,

    output wire [31:0]  m_axis_tdata,
    output wire         m_axis_tvalid,
    input  wire         m_axis_tready,
    output wire         m_axis_tlast,
    output wire         m_axis_tuser
);

    // Pure combinational pass-through for handshake.
    assign s_axis_tready = m_axis_tready;
    assign m_axis_tvalid = s_axis_tvalid;
    assign m_axis_tlast  = s_axis_tlast;
    assign m_axis_tuser  = s_axis_tuser;

    // Re/Im truncation.
    // TRUNC_LSB=12 means [27:12] is kept from each 32-bit component.
    assign m_axis_tdata[15:0]  = s_axis_tdata[TRUNC_LSB +: 16];
    assign m_axis_tdata[31:16] = s_axis_tdata[32 + TRUNC_LSB +: 16];

endmodule

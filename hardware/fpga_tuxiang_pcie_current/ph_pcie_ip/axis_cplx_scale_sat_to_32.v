`timescale 1ns / 1ps

// Scale a wide complex AXIS sample down to signed 16-bit Re/Im components.
//
// This is used after the PL-internal FFT/Hilbert/IFFT path. The algorithm keeps
// the wider FFT precision internally, then applies a controlled arithmetic
// right shift and saturation only before the 16-bit log/output stages.
module axis_cplx_scale_sat_to_32 #(
    parameter integer IN_COMP_W  = 48,
    parameter integer VALID_COMP_W = IN_COMP_W,
    parameter integer OUT_COMP_W = 16
) (
    input  wire                     aclk,
    input  wire                     aresetn,

    input  wire [(2*IN_COMP_W)-1:0] s_axis_tdata,
    input  wire                     s_axis_tvalid,
    output wire                     s_axis_tready,
    input  wire                     s_axis_tlast,
    input  wire                     s_axis_tuser,

    output wire [(2*OUT_COMP_W)-1:0] m_axis_tdata,
    output wire                      m_axis_tvalid,
    input  wire                      m_axis_tready,
    output wire                      m_axis_tlast,
    output wire                      m_axis_tuser,

    input  wire [4:0]                cfg_shift
);

    localparam signed [IN_COMP_W-1:0] OUT_MAX_EXT =
        {{(IN_COMP_W-OUT_COMP_W){1'b0}}, 1'b0, {(OUT_COMP_W-1){1'b1}}};
    localparam signed [IN_COMP_W-1:0] OUT_MIN_EXT =
        {{(IN_COMP_W-OUT_COMP_W){1'b1}}, 1'b1, {(OUT_COMP_W-1){1'b0}}};

    wire _unused_aclk    = aclk;
    wire _unused_aresetn = aresetn;

    wire signed [IN_COMP_W-1:0] in_re =
        {{(IN_COMP_W-VALID_COMP_W){s_axis_tdata[VALID_COMP_W-1]}},
          s_axis_tdata[VALID_COMP_W-1:0]};
    wire signed [IN_COMP_W-1:0] in_im =
        {{(IN_COMP_W-VALID_COMP_W){s_axis_tdata[IN_COMP_W+VALID_COMP_W-1]}},
          s_axis_tdata[IN_COMP_W+VALID_COMP_W-1:IN_COMP_W]};

    assign s_axis_tready = m_axis_tready;
    assign m_axis_tvalid = s_axis_tvalid;
    assign m_axis_tlast  = s_axis_tlast;
    assign m_axis_tuser  = s_axis_tuser;

    function [OUT_COMP_W-1:0] scale_sat_component;
        input signed [IN_COMP_W-1:0] value;
        input [4:0] shift;
        reg signed [IN_COMP_W-1:0] shifted;
        begin
            shifted = value >>> shift;

            if (shifted > OUT_MAX_EXT)
                scale_sat_component = {1'b0, {(OUT_COMP_W-1){1'b1}}};
            else if (shifted < OUT_MIN_EXT)
                scale_sat_component = {1'b1, {(OUT_COMP_W-1){1'b0}}};
            else
                scale_sat_component = shifted[OUT_COMP_W-1:0];
        end
    endfunction

    assign m_axis_tdata[OUT_COMP_W-1:0] =
        scale_sat_component(in_re, cfg_shift);
    assign m_axis_tdata[(2*OUT_COMP_W)-1:OUT_COMP_W] =
        scale_sat_component(in_im, cfg_shift);

endmodule

`timescale 1ns / 1ps

// Linear magnitude output for the OCT Hilbert envelope path.
//
// The training/reference dataset uses a linear Hilbert envelope, not a log
// compressed image. Magnitude is approximated as:
//   max(abs(re), abs(im)) + 1/2 * min(abs(re), abs(im))
// The PROC_CTRL gain/offset fields are reused as linear output gain and bias.
module linear_mag_comp #(
    parameter integer DATA_W = 32,
    parameter integer IN_W   = 16,
    parameter integer OUT_W  = 16
) (
    input  wire                    aclk,
    input  wire                    aresetn,

    input  wire [DATA_W-1:0]       s_axis_tdata,
    input  wire                    s_axis_tvalid,
    output wire                    s_axis_tready,
    input  wire                    s_axis_tlast,
    input  wire                    s_axis_tuser,

    output wire [DATA_W-1:0]       m_axis_tdata,
    output wire                    m_axis_tvalid,
    input  wire                    m_axis_tready,
    output wire                    m_axis_tlast,
    output wire                    m_axis_tuser,

    input  wire [3:0]              cfg_out_shift,
    input  wire [7:0]              cfg_gain_q4_4,
    input  wire signed [OUT_W-1:0] cfg_offset
);

    localparam integer ABS_W   = IN_W + 1;
    localparam integer MAG_W   = ABS_W + 1;
    localparam integer PROD_W  = MAG_W + 8;
    localparam integer SUM_W   = PROD_W + OUT_W + 1;
    localparam integer SHIFT_W = SUM_W + OUT_W;

    wire signed [IN_W-1:0] in_re = s_axis_tdata[IN_W-1:0];
    wire signed [IN_W-1:0] in_im = s_axis_tdata[DATA_W-1:DATA_W-IN_W];

    function [ABS_W-1:0] abs_component;
        input signed [IN_W-1:0] value;
        reg signed [ABS_W-1:0] extended;
        begin
            extended = {value[IN_W-1], value};
            abs_component = extended[ABS_W-1] ? (~extended + 1'b1) : extended;
        end
    endfunction

    function [OUT_W-1:0] sat_u16;
        input signed [SHIFT_W-1:0] value;
        begin
            if (value[SHIFT_W-1])
                sat_u16 = {OUT_W{1'b0}};
            else if (|value[SHIFT_W-2:OUT_W])
                sat_u16 = {OUT_W{1'b1}};
            else
                sat_u16 = value[OUT_W-1:0];
        end
    endfunction

    reg                    s0_valid_r;
    reg [ABS_W-1:0]        s0_abs_re_r;
    reg [ABS_W-1:0]        s0_abs_im_r;
    reg                    s0_last_r;
    reg                    s0_user_r;
    reg [3:0]              s0_out_shift_r;
    reg [7:0]              s0_gain_q4_4_r;
    reg signed [OUT_W-1:0] s0_offset_r;

    reg                    s1_valid_r;
    reg [MAG_W-1:0]        s1_mag_r;
    reg                    s1_last_r;
    reg                    s1_user_r;
    reg [3:0]              s1_out_shift_r;
    reg [7:0]              s1_gain_q4_4_r;
    reg signed [OUT_W-1:0] s1_offset_r;

    reg                    s2_valid_r;
    reg [PROD_W-1:0]       s2_product_r;
    reg                    s2_last_r;
    reg                    s2_user_r;
    reg [3:0]              s2_out_shift_r;
    reg signed [OUT_W-1:0] s2_offset_r;

    reg                    s3_valid_r;
    reg signed [SUM_W-1:0] s3_biased_r;
    reg                    s3_last_r;
    reg                    s3_user_r;
    reg [3:0]              s3_out_shift_r;

    reg                    s4_valid_r;
    reg signed [SHIFT_W-1:0] s4_shifted_r;
    reg                    s4_last_r;
    reg                    s4_user_r;

    reg                    m_valid_r;
    reg [OUT_W-1:0]        m_data_r;
    reg                    m_last_r;
    reg                    m_user_r;

    wire pipe_ce = !m_valid_r || m_axis_tready;
    assign s_axis_tready = pipe_ce;

    wire [ABS_W-1:0] s1_mag_max =
        (s0_abs_re_r >= s0_abs_im_r) ? s0_abs_re_r : s0_abs_im_r;
    wire [ABS_W-1:0] s1_mag_min =
        (s0_abs_re_r >= s0_abs_im_r) ? s0_abs_im_r : s0_abs_re_r;
    wire [MAG_W-1:0] s1_mag_next =
        {1'b0, s1_mag_max} + {2'b00, s1_mag_min[ABS_W-1:1]};

    (* use_dsp = "yes" *) wire [PROD_W-1:0] s2_product_next =
        s1_mag_r * s1_gain_q4_4_r;

    wire [PROD_W-1:0] s3_scaled_next = s2_product_r >> 4;
    wire signed [SUM_W-1:0] s3_biased_next =
        $signed({1'b0, s3_scaled_next}) +
        $signed({{(SUM_W-OUT_W){s2_offset_r[OUT_W-1]}}, s2_offset_r});

    wire signed [SHIFT_W-1:0] s4_extended_next =
        $signed({{OUT_W{s3_biased_r[SUM_W-1]}}, s3_biased_r});
    wire signed [SHIFT_W-1:0] s4_shifted_next =
        s4_extended_next <<< s3_out_shift_r;

    always @(posedge aclk) begin
        if (!aresetn) begin
            s0_valid_r <= 1'b0;
            s1_valid_r <= 1'b0;
            s2_valid_r <= 1'b0;
            s3_valid_r <= 1'b0;
            s4_valid_r <= 1'b0;
            m_valid_r  <= 1'b0;

            s0_last_r <= 1'b0;
            s1_last_r <= 1'b0;
            s2_last_r <= 1'b0;
            s3_last_r <= 1'b0;
            s4_last_r <= 1'b0;
            m_last_r  <= 1'b0;

            s0_user_r <= 1'b0;
            s1_user_r <= 1'b0;
            s2_user_r <= 1'b0;
            s3_user_r <= 1'b0;
            s4_user_r <= 1'b0;
            m_user_r  <= 1'b0;
        end else if (pipe_ce) begin
            s0_valid_r <= s_axis_tvalid;
            s0_abs_re_r <= abs_component(in_re);
            s0_abs_im_r <= abs_component(in_im);
            s0_last_r <= s_axis_tlast;
            s0_user_r <= s_axis_tuser;
            s0_out_shift_r <= cfg_out_shift;
            s0_gain_q4_4_r <= cfg_gain_q4_4;
            s0_offset_r <= cfg_offset;

            s1_valid_r <= s0_valid_r;
            s1_mag_r <= s1_mag_next;
            s1_last_r <= s0_last_r;
            s1_user_r <= s0_user_r;
            s1_out_shift_r <= s0_out_shift_r;
            s1_gain_q4_4_r <= s0_gain_q4_4_r;
            s1_offset_r <= s0_offset_r;

            s2_valid_r <= s1_valid_r;
            s2_product_r <= s2_product_next;
            s2_last_r <= s1_last_r;
            s2_user_r <= s1_user_r;
            s2_out_shift_r <= s1_out_shift_r;
            s2_offset_r <= s1_offset_r;

            s3_valid_r <= s2_valid_r;
            s3_biased_r <= s3_biased_next;
            s3_last_r <= s2_last_r;
            s3_user_r <= s2_user_r;
            s3_out_shift_r <= s2_out_shift_r;

            s4_valid_r <= s3_valid_r;
            s4_shifted_r <= s4_shifted_next;
            s4_last_r <= s3_last_r;
            s4_user_r <= s3_user_r;

            m_valid_r <= s4_valid_r;
            m_data_r <= sat_u16(s4_shifted_r);
            m_last_r <= s4_last_r;
            m_user_r <= s4_user_r;
        end
    end

    assign m_axis_tvalid = m_valid_r;
    assign m_axis_tlast  = m_last_r;
    assign m_axis_tuser  = m_user_r;
    assign m_axis_tdata  = {{(DATA_W-OUT_W){1'b0}}, m_data_r};

endmodule

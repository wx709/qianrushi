`timescale 1ns / 1ps

// Log compression for complex input stream.
// This stage replaces CORDIC magnitude plus log compression by operating in
// the power domain:
//   y ~= 10*log10(re^2 + im^2)
// which is mathematically equivalent to 20*log10(|re + j*im|).
// Output format is unsigned fixed-point Q12.4 in the low OUT_W bits.
module log_comp #(
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

    output reg  [DATA_W-1:0]       m_axis_tdata,
    output reg                     m_axis_tvalid,
    input  wire                    m_axis_tready,
    output reg                     m_axis_tlast,
    output reg                     m_axis_tuser,

    input  wire [3:0]              cfg_out_shift,
    input  wire [7:0]              cfg_log_gain_q4_4,
    input  wire signed [OUT_W-1:0] cfg_log_offset
);

    localparam integer SQ_W  = 2 * IN_W;
    localparam integer POW_W = SQ_W + 1;
    localparam integer MSB_W = (POW_W > 1) ? $clog2(POW_W) : 1;
    localparam integer DB_W  = 16;
    localparam integer GAIN_W = 8;
    localparam integer SUM_W  = OUT_W + 2;
    localparam integer PROD_W = OUT_W + GAIN_W;

    reg signed [IN_W-1:0] s0_re;
    reg signed [IN_W-1:0] s0_im;
    reg                   s0_valid;
    reg                   s0_tlast;
    reg                   s0_tuser;

    reg [SQ_W-1:0]        s1_re_sq;
    reg [SQ_W-1:0]        s1_im_sq;
    reg                   s1_valid;
    reg                   s1_tlast;
    reg                   s1_tuser;

    reg [POW_W-1:0]       s2_power;
    reg                   s2_power_zero;
    reg                   s2_valid;
    reg                   s2_tlast;
    reg                   s2_tuser;

    reg [POW_W-1:0]       s3_power;
    reg [MSB_W-1:0]       s3_msb_idx;
    reg                   s3_power_zero;
    reg                   s3_valid;
    reg                   s3_tlast;
    reg                   s3_tuser;

    reg [DB_W-1:0]        s4_db_msb;
    reg [5:0]             s4_frac_db;
    reg                   s4_power_zero;
    reg                   s4_valid;
    reg                   s4_tlast;
    reg                   s4_tuser;

    reg [PROD_W-1:0]      s5_product;
    reg                   s5_valid;
    reg                   s5_tlast;
    reg                   s5_tuser;

    wire stage6_ready = (~m_axis_tvalid) | m_axis_tready;
    wire stage5_ready = (~s5_valid) | stage6_ready;
    wire stage4_ready = (~s4_valid) | stage5_ready;
    wire stage3_ready = (~s3_valid) | stage4_ready;
    wire stage2_ready = (~s2_valid) | stage3_ready;
    wire stage1_ready = (~s1_valid) | stage2_ready;
    wire stage0_ready = (~s0_valid) | stage1_ready;

    wire signed [IN_W-1:0] s_axis_re = s_axis_tdata[IN_W-1:0];
    wire signed [IN_W-1:0] s_axis_im = s_axis_tdata[DATA_W-1:DATA_W-IN_W];

    wire [DB_W-1:0] s4_db_sum = s4_db_msb + {{(DB_W-6){1'b0}}, s4_frac_db};
    wire signed [SUM_W-1:0] s4_offset_sum =
        $signed({2'b00, s4_db_sum[OUT_W-1:0]}) +
        $signed({{2{cfg_log_offset[OUT_W-1]}}, cfg_log_offset});
    wire [OUT_W-1:0] s4_adjusted_db =
        s4_power_zero ? {OUT_W{1'b0}} :
        s4_offset_sum[SUM_W-1] ? {OUT_W{1'b0}} :
        (|s4_offset_sum[SUM_W-2:OUT_W]) ? {OUT_W{1'b1}} :
        s4_offset_sum[OUT_W-1:0];
    wire [PROD_W-1:0] s4_gain_product = s4_adjusted_db * cfg_log_gain_q4_4;
    wire [PROD_W-1:0] s5_scaled_db = s5_product >> 4;
    wire [PROD_W+OUT_W-1:0] s5_shifted_db_ext = {{OUT_W{1'b0}}, s5_scaled_db} << cfg_out_shift;
    wire [OUT_W-1:0] s5_output_db =
        (|s5_scaled_db[PROD_W-1:OUT_W] || |s5_shifted_db_ext[PROD_W+OUT_W-1:OUT_W]) ?
        {OUT_W{1'b1}} :
        s5_shifted_db_ext[OUT_W-1:0];

    assign s_axis_tready = stage0_ready;

    function [MSB_W-1:0] find_msb_idx;
        input [POW_W-1:0] value;
        integer j;
        begin
            find_msb_idx = {MSB_W{1'b0}};
            for (j = 0; j < POW_W; j = j + 1) begin
                if (value[j])
                    find_msb_idx = j[MSB_W-1:0];
            end
        end
    endfunction

    function [3:0] frac_nib_from_power;
        input [POW_W-1:0] value;
        input [MSB_W-1:0] msb_idx;
        reg   [POW_W-1:0] norm_power;
        begin
            norm_power = value << (POW_W - 1 - msb_idx);
            frac_nib_from_power = norm_power[POW_W-2 -: 4];
        end
    endfunction

    function [5:0] frac_db_lut;
        input [3:0] frac_nib;
        begin
            case (frac_nib)
                4'd0:  frac_db_lut = 6'd0;
                4'd1:  frac_db_lut = 6'd4;
                4'd2:  frac_db_lut = 6'd8;
                4'd3:  frac_db_lut = 6'd12;
                4'd4:  frac_db_lut = 6'd16;
                4'd5:  frac_db_lut = 6'd19;
                4'd6:  frac_db_lut = 6'd21;
                4'd7:  frac_db_lut = 6'd24;
                4'd8:  frac_db_lut = 6'd26;
                4'd9:  frac_db_lut = 6'd28;
                4'd10: frac_db_lut = 6'd30;
                4'd11: frac_db_lut = 6'd32;
                4'd12: frac_db_lut = 6'd34;
                4'd13: frac_db_lut = 6'd36;
                4'd14: frac_db_lut = 6'd38;
                default: frac_db_lut = 6'd39;
            endcase
        end
    endfunction

    always @(posedge aclk) begin
        if (!aresetn) begin
            s0_re         <= {IN_W{1'b0}};
            s0_im         <= {IN_W{1'b0}};
            s0_valid      <= 1'b0;
            s0_tlast      <= 1'b0;
            s0_tuser      <= 1'b0;
            s1_re_sq      <= {SQ_W{1'b0}};
            s1_im_sq      <= {SQ_W{1'b0}};
            s1_valid      <= 1'b0;
            s1_tlast      <= 1'b0;
            s1_tuser      <= 1'b0;
            s2_power      <= {POW_W{1'b0}};
            s2_power_zero <= 1'b0;
            s2_valid      <= 1'b0;
            s2_tlast      <= 1'b0;
            s2_tuser      <= 1'b0;
            s3_power      <= {POW_W{1'b0}};
            s3_msb_idx    <= {MSB_W{1'b0}};
            s3_power_zero <= 1'b0;
            s3_valid      <= 1'b0;
            s3_tlast      <= 1'b0;
            s3_tuser      <= 1'b0;
            s4_db_msb     <= {DB_W{1'b0}};
            s4_frac_db    <= 6'd0;
            s4_power_zero <= 1'b0;
            s4_valid      <= 1'b0;
            s4_tlast      <= 1'b0;
            s4_tuser      <= 1'b0;
            s5_product    <= {PROD_W{1'b0}};
            s5_valid      <= 1'b0;
            s5_tlast      <= 1'b0;
            s5_tuser      <= 1'b0;
            m_axis_tdata  <= {DATA_W{1'b0}};
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            m_axis_tuser  <= 1'b0;
        end else begin
            if (stage0_ready) begin
                s0_valid <= s_axis_tvalid;
                if (s_axis_tvalid) begin
                    s0_re    <= s_axis_re;
                    s0_im    <= s_axis_im;
                    s0_tlast <= s_axis_tlast;
                    s0_tuser <= s_axis_tuser;
                end else begin
                    s0_tlast <= 1'b0;
                    s0_tuser <= 1'b0;
                end
            end

            if (stage1_ready) begin
                s1_valid <= s0_valid;
                if (s0_valid) begin
                    s1_re_sq <= s0_re * s0_re;
                    s1_im_sq <= s0_im * s0_im;
                    s1_tlast <= s0_tlast;
                    s1_tuser <= s0_tuser;
                end else begin
                    s1_tlast <= 1'b0;
                    s1_tuser <= 1'b0;
                end
            end

            if (stage2_ready) begin
                s2_valid <= s1_valid;
                if (s1_valid) begin
                    s2_power      <= {1'b0, s1_re_sq} + {1'b0, s1_im_sq};
                    s2_power_zero <= (s1_re_sq == {SQ_W{1'b0}}) && (s1_im_sq == {SQ_W{1'b0}});
                    s2_tlast      <= s1_tlast;
                    s2_tuser      <= s1_tuser;
                end else begin
                    s2_tlast <= 1'b0;
                    s2_tuser <= 1'b0;
                end
            end

            if (stage3_ready) begin
                s3_valid <= s2_valid;
                if (s2_valid) begin
                    s3_power      <= s2_power;
                    s3_msb_idx    <= find_msb_idx(s2_power);
                    s3_power_zero <= s2_power_zero;
                    s3_tlast      <= s2_tlast;
                    s3_tuser      <= s2_tuser;
                end else begin
                    s3_tlast <= 1'b0;
                    s3_tuser <= 1'b0;
                end
            end

            if (stage4_ready) begin
                s4_valid <= s3_valid;
                if (s3_valid) begin
                    s4_frac_db    <= s3_power_zero ? 6'd0 : frac_db_lut(frac_nib_from_power(s3_power, s3_msb_idx));
                    s4_db_msb     <= (s3_msb_idx * 16'd771) >> 4;
                    s4_power_zero <= s3_power_zero;
                    s4_tlast      <= s3_tlast;
                    s4_tuser      <= s3_tuser;
                end else begin
                    s4_tlast <= 1'b0;
                    s4_tuser <= 1'b0;
                end
            end

            if (stage5_ready) begin
                s5_valid <= s4_valid;
                if (s4_valid) begin
                    s5_product <= s4_gain_product;
                    s5_tlast   <= s4_tlast;
                    s5_tuser   <= s4_tuser;
                end else begin
                    s5_tlast <= 1'b0;
                    s5_tuser <= 1'b0;
                end
            end

            if (stage6_ready) begin
                m_axis_tvalid <= s5_valid;
                if (s5_valid) begin
                    m_axis_tdata <= {{(DATA_W-OUT_W){1'b0}}, s5_output_db};
                    m_axis_tlast <= s5_tlast;
                    m_axis_tuser <= s5_tuser;
                end else begin
                    m_axis_tlast <= 1'b0;
                    m_axis_tuser <= 1'b0;
                end
            end
        end
    end

endmodule

`timescale 1ns / 1ps

// ============================================================================
// 模块: norm_mag
// 功能:
//   1) 输入 IFFT 复数数据 (Re/Im)
//   2) 固定右移进行归一化 (默认 >>10，相当于 /1024)
//   3) 输出归一化后的复数，供 CORDIC (Cartesian->Polar) 计算幅值
//
// 输入打包:
//   s_axis_tdata[15:0]   = Re (signed)
//   s_axis_tdata[31:16]  = Im (signed)
// 输出打包:
//   m_axis_tdata[15:0]   = Re_norm (signed)
//   m_axis_tdata[31:16]  = Im_norm (signed)
// ============================================================================
module norm_mag #(
    parameter integer DATA_W   = 32,
    parameter integer IN_W     = 16,
    parameter integer NORM_SHR = 13
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

    input  wire [3:0]              cfg_norm_shift
);

    wire pipe_ready = (~m_axis_tvalid) | m_axis_tready;
    assign s_axis_tready = pipe_ready;
    wire in_fire = s_axis_tvalid & s_axis_tready;

    wire signed [IN_W-1:0] in_re = s_axis_tdata[IN_W-1:0];
    wire signed [IN_W-1:0] in_im = s_axis_tdata[DATA_W-1:DATA_W-IN_W];

    wire signed [IN_W-1:0] re_n = in_re >>> cfg_norm_shift;
    wire signed [IN_W-1:0] im_n = in_im >>> cfg_norm_shift;

    always @(posedge aclk) begin
        if (!aresetn) begin
            m_axis_tdata  <= {DATA_W{1'b0}};
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            m_axis_tuser  <= 1'b0;
        end else begin
            if (in_fire) begin
                m_axis_tdata  <= {im_n, re_n};
                m_axis_tvalid <= 1'b1;
                m_axis_tlast  <= s_axis_tlast;
                m_axis_tuser  <= s_axis_tuser;
            end else if (m_axis_tvalid && m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tuser  <= 1'b0;
            end
        end
    end

endmodule

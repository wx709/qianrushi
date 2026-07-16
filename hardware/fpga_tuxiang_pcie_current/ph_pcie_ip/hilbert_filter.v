`timescale 1ns / 1ps

// ============================================================================
// 模块: hilbert_filter
// 功能:
//   对每帧 FFT 频域数据执行希尔伯特滤波（解析信号构造）：
//   1) k=0             -> 0
//   2) 1 <= k < N/2    -> 乘 2
//   3) k >= N/2        -> 0
//
// 接口:
//   AXI-Stream 输入/输出（复数打包）
//   默认复数格式: TDATA[15:0]=Re, TDATA[31:16]=Im（有符号）
//
// 说明:
//   - 每次握手（s_axis_tvalid && s_axis_tready）处理一个频点。
//   - 计数器默认按 FFT_SIZE 周期滚动；若输入 tlast 有效也会对齐复位。
// ============================================================================
module hilbert_filter #(
    parameter integer FFT_SIZE = 2048,
    parameter integer DATA_W   = 32,
    parameter integer VALID_COMP_W = DATA_W / 2
) (
    input  wire                   aclk,
    input  wire                   aresetn,

    input  wire [DATA_W-1:0]      s_axis_tdata,
    input  wire                   s_axis_tvalid,
    output wire                   s_axis_tready,
    input  wire                   s_axis_tlast,
    input  wire                   s_axis_tuser,

    output reg  [DATA_W-1:0]      m_axis_tdata,
    output reg                    m_axis_tvalid,
    input  wire                   m_axis_tready,
    output reg                    m_axis_tlast,
    output reg                    m_axis_tuser
);

    localparam integer HALF_W = DATA_W / 2;
    localparam integer K_W    = $clog2(FFT_SIZE);

    reg [K_W-1:0] k_idx;

    wire pipe_ready = (~m_axis_tvalid) | m_axis_tready;
    assign s_axis_tready = pipe_ready;

    wire in_fire = s_axis_tvalid & s_axis_tready;

    wire signed [HALF_W-1:0] in_re =
        {{(HALF_W-VALID_COMP_W){s_axis_tdata[VALID_COMP_W-1]}},
          s_axis_tdata[VALID_COMP_W-1:0]};
    wire signed [HALF_W-1:0] in_im =
        {{(HALF_W-VALID_COMP_W){s_axis_tdata[HALF_W+VALID_COMP_W-1]}},
          s_axis_tdata[HALF_W+VALID_COMP_W-1:HALF_W]};

    reg signed [HALF_W-1:0] out_re;
    reg signed [HALF_W-1:0] out_im;

    always @(*) begin
        // 默认直通（仅用于防止组合锁存）
        out_re = in_re;
        out_im = in_im;

        if (k_idx == 0) begin
            out_re = {HALF_W{1'b0}};
            out_im = {HALF_W{1'b0}};
        end else if (k_idx < (FFT_SIZE/2)) begin
            // 正频乘 2，固定点用左移实现
            out_re = in_re <<< 1;
            out_im = in_im <<< 1;
        end else begin
            out_re = {HALF_W{1'b0}};
            out_im = {HALF_W{1'b0}};
        end
    end

    always @(posedge aclk) begin
        if (!aresetn) begin
            k_idx         <= {K_W{1'b0}};
            m_axis_tdata  <= {DATA_W{1'b0}};
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            m_axis_tuser  <= 1'b0;
        end else begin
            if (in_fire) begin
                m_axis_tdata  <= {out_im, out_re};
                m_axis_tvalid <= 1'b1;
                m_axis_tlast  <= s_axis_tlast;
                m_axis_tuser  <= s_axis_tuser;

                if (s_axis_tlast) begin
                    k_idx <= {K_W{1'b0}};
                end else if (k_idx == FFT_SIZE - 1) begin
                    k_idx <= {K_W{1'b0}};
                end else begin
                    k_idx <= k_idx + 1'b1;
                end
            end else if (m_axis_tvalid && m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
                m_axis_tlast  <= 1'b0;
                m_axis_tuser  <= 1'b0;
            end
        end
    end

endmodule

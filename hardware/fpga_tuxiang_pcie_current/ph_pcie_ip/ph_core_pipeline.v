`timescale 1ns / 1ps

// 2026-06-19 image-processing core adapted to the current PCIe/XDMA project.
//
// The board, XDMA stream protocol, BAR registers, and reset-capable FFT IPs stay
// from tuxiang_pcie. The processing chain itself keeps the 6/19 algorithm:
// real samples -> FFT -> scaled/saturated 32-bit complex -> Hilbert -> IFFT ->
// scaled/saturated 32-bit complex -> normalization -> log compression.
//
// The two wide-to-32 conversion points are deliberately not raw bit slices. They
// use arithmetic right shift plus signed saturation so overflow cannot wrap into
// a false low-amplitude/cropped-looking pixel.
module ph_core_pipeline #(
    parameter integer FFT_SIZE = 2048,
    parameter integer FFT1_COMP_W = 28,
    parameter integer FFT1_SCALE_SHIFT = 12,
    parameter integer IFFT_COMP_W = 28,
    parameter integer IFFT_SCALE_SHIFT = 12
) (
    input  wire         aclk,
    input  wire         aresetn,

    input  wire [31:0]  s_axis_tdata,
    input  wire         s_axis_tvalid,
    output wire         s_axis_tready,
    input  wire         s_axis_tlast,
    input  wire         s_axis_tuser,

    output wire         fft_event_frame_started,
    output wire         fft_event_tlast_unexpected,
    output wire         fft_event_tlast_missing,
    output wire         fft_event_status_channel_halt,
    output wire         fft_event_data_in_channel_halt,
    output wire         fft_event_data_out_channel_halt,

    output wire [31:0]  m_axis_tdata,
    output wire         m_axis_tvalid,
    input  wire         m_axis_tready,
    output wire         m_axis_tlast,
    output wire         m_axis_tuser,

    input  wire [3:0]   cfg_norm_shift,
    input  wire [3:0]   cfg_out_shift,
    input  wire [7:0]   cfg_log_gain_q4_4,
    input  wire signed [15:0] cfg_log_offset
);

    wire        fft_in_tready;

    wire [63:0] fft1_tdata;
    wire        fft1_tvalid;
    wire        fft1_tready;
    wire        fft1_tlast;

    wire [31:0] fft1_trunc_tdata;
    wire        fft1_trunc_tvalid;
    wire        fft1_trunc_tready;
    wire        fft1_trunc_tlast;
    wire        fft1_trunc_tuser;

    wire [31:0] hilbert_tdata;
    wire        hilbert_tvalid;
    wire        hilbert_tready;
    wire        hilbert_tlast;
    wire        hilbert_tuser;

    wire [63:0] ifft_tdata;
    wire        ifft_tvalid;
    wire        ifft_tready;
    wire        ifft_tlast;

    wire [31:0] ifft_trunc_tdata;
    wire        ifft_trunc_tvalid;
    wire        ifft_trunc_tready;
    wire        ifft_trunc_tlast;
    wire        ifft_trunc_tuser;

    wire [31:0] norm_tdata;
    wire        norm_tvalid;
    wire        norm_tready;
    wire        norm_tlast;
    wire        norm_tuser;

    reg         cfg_fft_tvalid;
    wire        cfg_fft_tready;
    reg         cfg_ifft_tvalid;
    wire        cfg_ifft_tready;

    wire        _unused_s_axis_tuser = s_axis_tuser;

    localparam [4:0] FFT1_SHIFT_U5 = FFT1_SCALE_SHIFT[4:0];
    localparam [4:0] IFFT_SHIFT_U5 = IFFT_SCALE_SHIFT[4:0];

    wire cfg_done = ~cfg_fft_tvalid & ~cfg_ifft_tvalid;
    wire gated_s_axis_tvalid = s_axis_tvalid & cfg_done;

    assign s_axis_tready = cfg_done & fft_in_tready;

    always @(posedge aclk) begin
        if (!aresetn) begin
            cfg_fft_tvalid  <= 1'b1;
            cfg_ifft_tvalid <= 1'b1;
        end else begin
            if (cfg_fft_tvalid && cfg_fft_tready)
                cfg_fft_tvalid <= 1'b0;
            if (cfg_ifft_tvalid && cfg_ifft_tready)
                cfg_ifft_tvalid <= 1'b0;
        end
    end

    xfft_in u_fft_fwd (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_config_tdata(8'h01),
        .s_axis_config_tvalid(cfg_fft_tvalid),
        .s_axis_config_tready(cfg_fft_tready),
        .s_axis_data_tdata(s_axis_tdata),
        .s_axis_data_tvalid(gated_s_axis_tvalid),
        .s_axis_data_tready(fft_in_tready),
        .s_axis_data_tlast(s_axis_tlast & cfg_done),
        .m_axis_data_tdata(fft1_tdata),
        .m_axis_data_tvalid(fft1_tvalid),
        .m_axis_data_tready(fft1_tready),
        .m_axis_data_tlast(fft1_tlast),
        .event_frame_started(fft_event_frame_started),
        .event_tlast_unexpected(fft_event_tlast_unexpected),
        .event_tlast_missing(fft_event_tlast_missing),
        .event_status_channel_halt(fft_event_status_channel_halt),
        .event_data_in_channel_halt(fft_event_data_in_channel_halt),
        .event_data_out_channel_halt(fft_event_data_out_channel_halt)
    );

    axis_cplx_scale_sat_to_32 #(
        .IN_COMP_W(32),
        .VALID_COMP_W(FFT1_COMP_W),
        .OUT_COMP_W(16)
    ) u_fft1_scale_sat (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(fft1_tdata),
        .s_axis_tvalid(fft1_tvalid),
        .s_axis_tready(fft1_tready),
        .s_axis_tlast(fft1_tlast),
        .s_axis_tuser(1'b0),
        .m_axis_tdata(fft1_trunc_tdata),
        .m_axis_tvalid(fft1_trunc_tvalid),
        .m_axis_tready(fft1_trunc_tready),
        .m_axis_tlast(fft1_trunc_tlast),
        .m_axis_tuser(fft1_trunc_tuser),
        .cfg_shift(FFT1_SHIFT_U5)
    );

    hilbert_filter #(
        .FFT_SIZE(FFT_SIZE),
        .DATA_W(32)
    ) u_hilbert_filter (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(fft1_trunc_tdata),
        .s_axis_tvalid(fft1_trunc_tvalid),
        .s_axis_tready(fft1_trunc_tready),
        .s_axis_tlast(fft1_trunc_tlast),
        .s_axis_tuser(fft1_trunc_tuser),
        .m_axis_tdata(hilbert_tdata),
        .m_axis_tvalid(hilbert_tvalid),
        .m_axis_tready(hilbert_tready),
        .m_axis_tlast(hilbert_tlast),
        .m_axis_tuser(hilbert_tuser)
    );

    xfft_0 u_fft_inv (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_config_tdata(8'h00),
        .s_axis_config_tvalid(cfg_ifft_tvalid),
        .s_axis_config_tready(cfg_ifft_tready),
        .s_axis_data_tdata(hilbert_tdata),
        .s_axis_data_tvalid(hilbert_tvalid),
        .s_axis_data_tready(hilbert_tready),
        .s_axis_data_tlast(hilbert_tlast),
        .m_axis_data_tdata(ifft_tdata),
        .m_axis_data_tvalid(ifft_tvalid),
        .m_axis_data_tready(ifft_tready),
        .m_axis_data_tlast(ifft_tlast),
        .event_frame_started(),
        .event_tlast_unexpected(),
        .event_tlast_missing(),
        .event_status_channel_halt(),
        .event_data_in_channel_halt(),
        .event_data_out_channel_halt()
    );

    axis_cplx_scale_sat_to_32 #(
        .IN_COMP_W(32),
        .VALID_COMP_W(IFFT_COMP_W),
        .OUT_COMP_W(16)
    ) u_ifft_scale_sat (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(ifft_tdata),
        .s_axis_tvalid(ifft_tvalid),
        .s_axis_tready(ifft_tready),
        .s_axis_tlast(ifft_tlast),
        .s_axis_tuser(1'b0),
        .m_axis_tdata(ifft_trunc_tdata),
        .m_axis_tvalid(ifft_trunc_tvalid),
        .m_axis_tready(ifft_trunc_tready),
        .m_axis_tlast(ifft_trunc_tlast),
        .m_axis_tuser(ifft_trunc_tuser),
        .cfg_shift(IFFT_SHIFT_U5)
    );

    norm_mag #(
        .DATA_W(32),
        .IN_W(16),
        .NORM_SHR(10)
    ) u_norm_mag (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(ifft_trunc_tdata),
        .s_axis_tvalid(ifft_trunc_tvalid),
        .s_axis_tready(ifft_trunc_tready),
        .s_axis_tlast(ifft_trunc_tlast),
        .s_axis_tuser(ifft_trunc_tuser),
        .m_axis_tdata(norm_tdata),
        .m_axis_tvalid(norm_tvalid),
        .m_axis_tready(norm_tready),
        .m_axis_tlast(norm_tlast),
        .m_axis_tuser(norm_tuser),
        .cfg_norm_shift(cfg_norm_shift)
    );

    log_comp #(
        .DATA_W(32),
        .IN_W(16),
        .OUT_W(16)
    ) u_log_comp (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(norm_tdata),
        .s_axis_tvalid(norm_tvalid),
        .s_axis_tready(norm_tready),
        .s_axis_tlast(norm_tlast),
        .s_axis_tuser(norm_tuser),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast),
        .m_axis_tuser(m_axis_tuser),
        .cfg_out_shift(cfg_out_shift),
        .cfg_log_gain_q4_4(cfg_log_gain_q4_4),
        .cfg_log_offset(cfg_log_offset)
    );

endmodule

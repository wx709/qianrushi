`timescale 1ns / 1ps

// OCT image processing IP for the PCIe streaming path.
//
// This top keeps the working RK3588 <-> XDMA stream protocol used by the
// tuxiang_pcie project.  The core below is now the 2026-06-19 processing chain
// adapted to this board/PCIe design:
// real samples -> FFT -> Hilbert filter -> IFFT -> configurable log-compressed output.
module ph_ip #(
    parameter integer DEPTH_POINTS = 2048,
    parameter integer FFT_SIZE     = 2048
) (
    input  wire         aclk,
    input  wire         aresetn,

    input  wire [15:0]  s_axis_tdata,
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

    output wire         busy,

    output wire         fft_event_frame_started,
    output wire         fft_event_tlast_unexpected,
    output wire         fft_event_tlast_missing,
    output wire         fft_event_status_channel_halt,
    output wire         fft_event_data_in_channel_halt,
    output wire         fft_event_data_out_channel_halt,

    output wire [31:0]  proc_tdata,
    output wire         proc_tvalid,
    output wire         proc_tlast,
    output wire         proc_tuser,

    input  wire [3:0]   cfg_norm_shift,
    input  wire [3:0]   cfg_out_shift,
    input  wire [7:0]   cfg_log_gain_q4_4,
    input  wire signed [15:0] cfg_log_offset,

    output wire [31:0]  debug_flags
);

    wire [31:0] pad_tdata;
    wire        pad_tvalid;
    wire        pad_tready;
    wire        pad_tlast;
    wire        pad_tuser;
    wire        frame_start_pulse;

    wire        proc_tready;
    wire        core_frame_done_pulse;

    reg [7:0]   inflight_frames;
    reg [31:0]  started_lines;
    reg [31:0]  completed_lines;

    axis_real_to_fft_frame #(
        .LINE_SAMPLES(DEPTH_POINTS),
        .FFT_SIZE(FFT_SIZE)
    ) u_axis_real_to_fft_frame (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(s_axis_tdata),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .s_axis_tuser(s_axis_tuser),
        .m_axis_tdata(pad_tdata),
        .m_axis_tvalid(pad_tvalid),
        .m_axis_tready(pad_tready),
        .m_axis_tlast(pad_tlast),
        .m_axis_tuser(pad_tuser),
        .frame_start_pulse(frame_start_pulse)
    );

    ph_core_pipeline #(
        .FFT_SIZE(FFT_SIZE)
    ) u_ph_core_pipeline (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(pad_tdata),
        .s_axis_tvalid(pad_tvalid),
        .s_axis_tready(pad_tready),
        .s_axis_tlast(pad_tlast),
        .s_axis_tuser(pad_tuser),
        .fft_event_frame_started(fft_event_frame_started),
        .fft_event_tlast_unexpected(fft_event_tlast_unexpected),
        .fft_event_tlast_missing(fft_event_tlast_missing),
        .fft_event_status_channel_halt(fft_event_status_channel_halt),
        .fft_event_data_in_channel_halt(fft_event_data_in_channel_halt),
        .fft_event_data_out_channel_halt(fft_event_data_out_channel_halt),
        .m_axis_tdata(proc_tdata),
        .m_axis_tvalid(proc_tvalid),
        .m_axis_tready(proc_tready),
        .m_axis_tlast(proc_tlast),
        .m_axis_tuser(proc_tuser),
        .cfg_norm_shift(cfg_norm_shift),
        .cfg_out_shift(cfg_out_shift),
        .cfg_log_gain_q4_4(cfg_log_gain_q4_4),
        .cfg_log_offset(cfg_log_offset)
    );

    axis_u16_packer #(
        .VALID_SAMPLES(DEPTH_POINTS),
        .FFT_SIZE(FFT_SIZE)
    ) u_axis_u16_packer (
        .aclk(aclk),
        .aresetn(aresetn),
        .s_axis_tdata(proc_tdata),
        .s_axis_tvalid(proc_tvalid),
        .s_axis_tready(proc_tready),
        .s_axis_tlast(proc_tlast),
        .s_axis_tuser(proc_tuser),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast),
        .m_axis_tuser(m_axis_tuser),
        .core_frame_done_pulse(core_frame_done_pulse)
    );

    always @(posedge aclk) begin
        if (!aresetn) begin
            inflight_frames <= 8'd0;
            started_lines   <= 32'd0;
            completed_lines <= 32'd0;
        end else begin
            if (frame_start_pulse)
                started_lines <= started_lines + 1'b1;
            if (core_frame_done_pulse)
                completed_lines <= completed_lines + 1'b1;

            case ({frame_start_pulse, core_frame_done_pulse})
                2'b10: begin
                    if (inflight_frames != 8'hFF)
                        inflight_frames <= inflight_frames + 1'b1;
                end
                2'b01: begin
                    if (inflight_frames != 8'd0)
                        inflight_frames <= inflight_frames - 1'b1;
                end
                default: begin
                end
            endcase
        end
    end

    assign busy = (inflight_frames != 8'd0);

    assign debug_flags = {
        8'd0,
        started_lines[3:0],
        completed_lines[3:0],
        inflight_frames[3:0],
        pad_tready,
        pad_tvalid,
        proc_tready,
        proc_tvalid,
        m_axis_tready,
        m_axis_tvalid,
        proc_tlast,
        m_axis_tlast
    };

endmodule

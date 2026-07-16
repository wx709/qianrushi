`timescale 1ns / 1ps

// PCIe image processing pipeline: RK3588 <-> FPGA
//
// Data path:
//   RK3588 XDMA H2C stream (64-bit)
//     -> ascent_h2c64_to_u16_axis (unpack to 16-bit samples)
//     -> ph_ip (FFT->Hilbert->IFFT->norm->log-compressed envelope)
//     -> axis_line_to_frame (per-line -> per-frame tlast/tuser)
//     -> XDMA C2H stream (64-bit) -> RK3588
//
// One frame = NUM_LINES lines x DEPTH_POINTS samples.
// AXI-Lite BAR registers provide control/status.
module ascent_pcie_ph_app #(
    parameter integer DEPTH_POINTS = 2048,
    parameter integer FFT_SIZE     = 2048,
    parameter integer NUM_LINES    = 4096,
    parameter [31:0]  VERSION_ID   = 32'h2026_0619
) (
    input  wire        user_clk,
    input  wire        user_resetn,
    input  wire        sys_rst_n,
    input  wire        user_lnk_up,

    // AXI4-Lite slave (from XDMA BAR)
    input  wire [31:0] s_axil_awaddr,
    input  wire        s_axil_awvalid,
    output reg         s_axil_awready,
    input  wire [31:0] s_axil_wdata,
    input  wire [3:0]  s_axil_wstrb,
    input  wire        s_axil_wvalid,
    output reg         s_axil_wready,
    output reg  [1:0]  s_axil_bresp,
    output reg         s_axil_bvalid,
    input  wire        s_axil_bready,
    input  wire [31:0] s_axil_araddr,
    input  wire        s_axil_arvalid,
    output reg         s_axil_arready,
    output reg  [31:0] s_axil_rdata,
    output reg  [1:0]  s_axil_rresp,
    output reg         s_axil_rvalid,
    input  wire        s_axil_rready,

    // XDMA H2C stream (data from RK3588)
    input  wire [63:0] m_axis_h2c_tdata,
    input  wire        m_axis_h2c_tlast,
    input  wire        m_axis_h2c_tvalid,
    output wire        m_axis_h2c_tready,
    input  wire [7:0]  m_axis_h2c_tkeep,

    // XDMA C2H stream (processed data to RK3588)
    output wire [63:0] s_axis_c2h_tdata,
    output wire        s_axis_c2h_tlast,
    output wire        s_axis_c2h_tvalid,
    input  wire        s_axis_c2h_tready,
    output wire [7:0]  s_axis_c2h_tkeep,

    // LEDs
    output wire [3:0]  leds
);

    // ============================================================
    // Register map
    // ============================================================
    localparam [7:0] REG_CTRL         = 8'h00;
    localparam [7:0] REG_STATUS       = 8'h04;
    localparam [7:0] REG_FRAME_CFG    = 8'h08;
    localparam [7:0] REG_VERSION      = 8'h0C;
    localparam [7:0] REG_H2C_WORDS    = 8'h10;
    localparam [7:0] REG_H2C_SAMPLES  = 8'h14;
    localparam [7:0] REG_H2C_LINES    = 8'h18;
    localparam [7:0] REG_C2H_WORDS    = 8'h1C;
    localparam [7:0] REG_C2H_FRAMES   = 8'h20;
    localparam [7:0] REG_BACKPRESSURE = 8'h24;
    localparam [7:0] REG_PH_EVENTS    = 8'h28;
    localparam [7:0] REG_USER_CLK_HZ  = 8'h2C;
    localparam [7:0] REG_LINE_CYCLES  = 8'h30;
    localparam [7:0] REG_LINE_COUNT   = 8'h34;
    localparam [7:0] REG_DEBUG_FLAGS  = 8'h38;
    localparam [7:0] REG_PROC_CTRL    = 8'h3C;

    localparam [15:0] DEPTH_POINTS_U16 = DEPTH_POINTS;
    localparam [15:0] FFT_SIZE_U16     = FFT_SIZE;
    localparam [15:0] NUM_LINES_U16    = NUM_LINES;
    localparam [31:0] USER_CLK_HZ      = 32'd125000000;

    // ============================================================
    // Control registers
    // ============================================================
    reg        stream_enable;
    reg        bypass_enable;
    reg        soft_reset_pulse;
    reg        clear_counters_pulse;
    reg [3:0]  proc_norm_shift;
    reg [3:0]  proc_out_shift;
    reg [7:0]  proc_log_gain_q4_4;
    reg signed [15:0] proc_log_offset;

    reg [31:0] awaddr_latched;
    reg [31:0] wdata_latched;
    reg [3:0]  wstrb_latched;
    reg        aw_seen;
    reg        w_seen;

    // ============================================================
    // Monitoring counters
    // ============================================================
    reg [25:0] heartbeat;
    reg [31:0] c2h_words;
    reg [31:0] c2h_frames;
    reg [31:0] h2c_backpressure;
    reg [31:0] c2h_backpressure;
    reg [31:0] user_cycle_count;
    reg [31:0] line_core_cycles;
    reg [31:0] line_latency_count;
    wire [31:0] h2c_words;
    wire [31:0] h2c_samples;
    wire [31:0] h2c_lines;

    // ============================================================
    // Data path signals
    // ============================================================
    wire datapath_resetn    = user_resetn & sys_rst_n & ~soft_reset_pulse;
    wire image_path_enable  = stream_enable & ~bypass_enable & datapath_resetn;
    wire bypass_path_enable = stream_enable &  bypass_enable & datapath_resetn;

    // H2C unpacker output
    wire [15:0] sample_tdata;
    wire        sample_tvalid;
    wire        sample_tready;
    wire        sample_tlast;
    wire        sample_tuser;
    wire        sample_input_tready;

    // ph_ip output
    wire [63:0] ph_tdata;
    wire [7:0]  ph_tkeep;
    wire        ph_tvalid;
    wire        ph_tready;
    wire        ph_tlast;
    wire        ph_tuser;
    wire        ph_busy;
    wire [31:0] ph_debug_flags;

    // Frame wrapper output (per-line -> per-frame)
    wire [63:0] frame_tdata;
    wire [7:0]  frame_tkeep;
    wire        frame_tvalid;
    wire        frame_tready;
    wire        frame_tlast;
    wire        frame_tuser;

    // FFT events
    wire        fft_event_frame_started;
    wire        fft_event_tlast_unexpected;
    wire        fft_event_tlast_missing;
    wire        fft_event_status_channel_halt;
    wire        fft_event_data_in_channel_halt;
    wire        fft_event_data_out_channel_halt;

    // Unused ph_ip debug outputs
    wire [31:0] proc_tdata_unused;
    wire        proc_tvalid_unused;
    wire        proc_tlast_unused;
    wire        proc_tuser_unused;

    // Event monitoring
    wire c2h_fire  = s_axis_c2h_tvalid & s_axis_c2h_tready;
    wire frame_done_pulse = frame_tvalid & frame_tready & frame_tlast;
    wire sample_line_start = sample_tvalid & sample_tready & sample_tuser & image_path_enable;
    wire ph_line_done      = ph_tvalid & ph_tready & ph_tlast & image_path_enable;

    localparam integer LAT_FIFO_DEPTH = 16;
    localparam integer LAT_FIFO_W = 4;
    localparam [LAT_FIFO_W:0] LAT_FIFO_DEPTH_COUNT = LAT_FIFO_DEPTH;
    reg [31:0] line_start_fifo [0:LAT_FIFO_DEPTH-1];
    reg [LAT_FIFO_W-1:0] line_start_wr;
    reg [LAT_FIFO_W-1:0] line_start_rd;
    reg [LAT_FIFO_W:0]   line_start_count;

    // ============================================================
    // H2C input: accept when downstream ready
    // ============================================================
    assign m_axis_h2c_tready = bypass_enable ? (bypass_path_enable & s_axis_c2h_tready) :
                                               (image_path_enable & sample_input_tready);

    // ============================================================
    // H2C 64-bit -> 16-bit sample unpacker
    // ============================================================
    ascent_h2c64_to_u16_axis #(
        .SAMPLES_PER_LINE(DEPTH_POINTS)
    ) u_h2c_unpack (
        .aclk           (user_clk),
        .aresetn        (datapath_resetn),
        .clear_counters (clear_counters_pulse),
        .s_axis_tdata   (m_axis_h2c_tdata),
        .s_axis_tkeep   (m_axis_h2c_tkeep),
        .s_axis_tvalid  (m_axis_h2c_tvalid & image_path_enable),
        .s_axis_tready  (sample_input_tready),
        .s_axis_tlast   (m_axis_h2c_tlast),
        .m_axis_tdata   (sample_tdata),
        .m_axis_tvalid  (sample_tvalid),
        .m_axis_tready  (sample_tready),
        .m_axis_tlast   (sample_tlast),
        .m_axis_tuser   (sample_tuser),
        .dbg_words      (h2c_words),
        .dbg_samples    (h2c_samples),
        .dbg_lines      (h2c_lines)
    );

    // ============================================================
    // ph_ip: streaming image processor
    // ============================================================
    ph_ip #(
        .DEPTH_POINTS (DEPTH_POINTS),
        .FFT_SIZE     (FFT_SIZE)
    ) u_ph_ip (
        .aclk                         (user_clk),
        .aresetn                      (datapath_resetn),
        .s_axis_tdata                 (sample_tdata),
        .s_axis_tvalid                (sample_tvalid & image_path_enable),
        .s_axis_tready                (sample_tready),
        .s_axis_tlast                 (sample_tlast),
        .s_axis_tuser                 (sample_tuser),
        .m_axis_tdata                 (ph_tdata),
        .m_axis_tkeep                 (ph_tkeep),
        .m_axis_tvalid                (ph_tvalid),
        .m_axis_tready                (ph_tready),
        .m_axis_tlast                 (ph_tlast),
        .m_axis_tuser                 (ph_tuser),
        .busy                         (ph_busy),
        .fft_event_frame_started      (fft_event_frame_started),
        .fft_event_tlast_unexpected   (fft_event_tlast_unexpected),
        .fft_event_tlast_missing      (fft_event_tlast_missing),
        .fft_event_status_channel_halt(fft_event_status_channel_halt),
        .fft_event_data_in_channel_halt (fft_event_data_in_channel_halt),
        .fft_event_data_out_channel_halt(fft_event_data_out_channel_halt),
        .proc_tdata                   (proc_tdata_unused),
        .proc_tvalid                  (proc_tvalid_unused),
        .proc_tlast                   (proc_tlast_unused),
        .proc_tuser                   (proc_tuser_unused),
        .cfg_norm_shift               (proc_norm_shift),
        .cfg_out_shift                (proc_out_shift),
        .cfg_log_gain_q4_4            (proc_log_gain_q4_4),
        .cfg_log_offset               (proc_log_offset),
        .debug_flags                  (ph_debug_flags)
    );

    // ============================================================
    // axis_line_to_frame: per-line tlast -> per-frame tlast
    // Counts NUM_LINES output lines, asserts tlast on last line.
    // ============================================================
    axis_line_to_frame #(
        .NUM_LINES (NUM_LINES),
        .DATA_W    (64)
    ) u_frame_wrapper (
        .aclk          (user_clk),
        .aresetn       (datapath_resetn),
        .s_axis_tdata  (ph_tdata),
        .s_axis_tkeep  (ph_tkeep),
        .s_axis_tvalid (ph_tvalid),
        .s_axis_tready (ph_tready),
        .s_axis_tlast  (ph_tlast),
        .s_axis_tuser  (ph_tuser),
        .m_axis_tdata  (frame_tdata),
        .m_axis_tkeep  (frame_tkeep),
        .m_axis_tvalid (frame_tvalid),
        .m_axis_tready (frame_tready),
        .m_axis_tlast  (frame_tlast),
        .m_axis_tuser  (frame_tuser)
    );

    assign frame_tready = image_path_enable & s_axis_c2h_tready;

    // ============================================================
    // C2H output mux: bypass or processed stream
    // ============================================================
    assign s_axis_c2h_tdata  = bypass_enable ? m_axis_h2c_tdata  : frame_tdata;
    assign s_axis_c2h_tkeep  = bypass_enable ? m_axis_h2c_tkeep  : frame_tkeep;
    assign s_axis_c2h_tlast  = bypass_enable ? m_axis_h2c_tlast  : frame_tlast;
    assign s_axis_c2h_tvalid = bypass_enable ? (m_axis_h2c_tvalid & bypass_path_enable) :
                                               (frame_tvalid & image_path_enable);

    // ============================================================
    // LED indicators
    // ============================================================
    assign leds[0] = stream_enable;
    assign leds[1] = bypass_enable;
    assign leds[2] = user_lnk_up;
    assign leds[3] = heartbeat[25];

    // ============================================================
    // Heartbeat
    // ============================================================
    always @(posedge user_clk) begin
        if (!user_resetn || !sys_rst_n)
            heartbeat <= 26'd0;
        else
            heartbeat <= heartbeat + 1'b1;
    end

    // ============================================================
    // Monitor counters
    // ============================================================
    integer lat_i;
    always @(posedge user_clk) begin
        if (!user_resetn || !sys_rst_n) begin
            c2h_words        <= 32'd0;
            c2h_frames       <= 32'd0;
            h2c_backpressure <= 32'd0;
            c2h_backpressure <= 32'd0;
            user_cycle_count <= 32'd0;
            line_core_cycles <= 32'd0;
            line_latency_count <= 32'd0;
            line_start_wr    <= {LAT_FIFO_W{1'b0}};
            line_start_rd    <= {LAT_FIFO_W{1'b0}};
            line_start_count <= {(LAT_FIFO_W+1){1'b0}};
            for (lat_i = 0; lat_i < LAT_FIFO_DEPTH; lat_i = lat_i + 1)
                line_start_fifo[lat_i] <= 32'd0;
        end else begin
            user_cycle_count <= user_cycle_count + 1'b1;

            if (clear_counters_pulse) begin
                c2h_words        <= 32'd0;
                c2h_frames       <= 32'd0;
                h2c_backpressure <= 32'd0;
                c2h_backpressure <= 32'd0;
                line_core_cycles <= 32'd0;
                line_latency_count <= 32'd0;
                line_start_wr    <= {LAT_FIFO_W{1'b0}};
                line_start_rd    <= {LAT_FIFO_W{1'b0}};
                line_start_count <= {(LAT_FIFO_W+1){1'b0}};
            end else begin
                if (c2h_fire) begin
                    c2h_words <= c2h_words + 1'b1;
                    if (s_axis_c2h_tlast)
                        c2h_frames <= c2h_frames + 1'b1;
                end
                if (m_axis_h2c_tvalid && !m_axis_h2c_tready)
                    h2c_backpressure <= h2c_backpressure + 1'b1;
                if (s_axis_c2h_tvalid && !s_axis_c2h_tready)
                    c2h_backpressure <= c2h_backpressure + 1'b1;

                case ({sample_line_start, ph_line_done})
                    2'b10: begin
                        if (line_start_count != LAT_FIFO_DEPTH_COUNT) begin
                            line_start_fifo[line_start_wr] <= user_cycle_count;
                            line_start_wr <= line_start_wr + 1'b1;
                            line_start_count <= line_start_count + 1'b1;
                        end
                    end
                    2'b01: begin
                        if (line_start_count != {(LAT_FIFO_W+1){1'b0}}) begin
                            line_core_cycles <= user_cycle_count - line_start_fifo[line_start_rd];
                            line_latency_count <= line_latency_count + 1'b1;
                            line_start_rd <= line_start_rd + 1'b1;
                            line_start_count <= line_start_count - 1'b1;
                        end
                    end
                    2'b11: begin
                        if (line_start_count != {(LAT_FIFO_W+1){1'b0}}) begin
                            line_core_cycles <= user_cycle_count - line_start_fifo[line_start_rd];
                            line_latency_count <= line_latency_count + 1'b1;
                            line_start_fifo[line_start_wr] <= user_cycle_count;
                            line_start_rd <= line_start_rd + 1'b1;
                            line_start_wr <= line_start_wr + 1'b1;
                        end else begin
                            line_start_fifo[line_start_wr] <= user_cycle_count;
                            line_start_wr <= line_start_wr + 1'b1;
                            line_start_count <= line_start_count + 1'b1;
                        end
                    end
                    default: begin
                    end
                endcase
            end
        end
    end

    // ============================================================
    // AXI-Lite write handler
    // ============================================================
    always @(posedge user_clk) begin
        if (!user_resetn || !sys_rst_n) begin
            stream_enable        <= 1'b1;
            bypass_enable        <= 1'b0;
            soft_reset_pulse     <= 1'b0;
            clear_counters_pulse <= 1'b0;
            proc_norm_shift      <= 4'd10;
            proc_out_shift       <= 4'd0;
            proc_log_gain_q4_4   <= 8'd16;
            proc_log_offset      <= 16'sd0;
            s_axil_awready       <= 1'b1;
            s_axil_wready        <= 1'b1;
            s_axil_bresp         <= 2'b00;
            s_axil_bvalid        <= 1'b0;
            awaddr_latched       <= 32'd0;
            wdata_latched        <= 32'd0;
            wstrb_latched        <= 4'd0;
            aw_seen              <= 1'b0;
            w_seen               <= 1'b0;
        end else begin
            soft_reset_pulse     <= 1'b0;
            clear_counters_pulse <= 1'b0;

            if (s_axil_awready && s_axil_awvalid) begin
                awaddr_latched <= s_axil_awaddr;
                aw_seen        <= 1'b1;
                s_axil_awready <= 1'b0;
            end
            if (s_axil_wready && s_axil_wvalid) begin
                wdata_latched  <= s_axil_wdata;
                wstrb_latched  <= s_axil_wstrb;
                w_seen         <= 1'b1;
                s_axil_wready  <= 1'b0;
            end

            if (!s_axil_bvalid && aw_seen && w_seen) begin
                if (awaddr_latched[7:0] == REG_CTRL && wstrb_latched[0]) begin
                    stream_enable <= wdata_latched[0];
                    if (wdata_latched[1])
                        soft_reset_pulse <= 1'b1;
                    bypass_enable <= wdata_latched[2];
                    if (wdata_latched[4])
                        clear_counters_pulse <= 1'b1;
                end
                if (awaddr_latched[7:0] == REG_PROC_CTRL) begin
                    if (wstrb_latched[0])
                        proc_norm_shift <= wdata_latched[3:0];
                    if (wstrb_latched[0])
                        proc_out_shift <= wdata_latched[7:4];
                    if (wstrb_latched[1])
                        proc_log_gain_q4_4 <= wdata_latched[15:8];
                    if (wstrb_latched[2])
                        proc_log_offset[7:0] <= wdata_latched[23:16];
                    if (wstrb_latched[3])
                        proc_log_offset[15:8] <= wdata_latched[31:24];
                end

                s_axil_bresp  <= 2'b00;
                s_axil_bvalid <= 1'b1;
            end

            if (s_axil_bvalid && s_axil_bready) begin
                s_axil_bvalid  <= 1'b0;
                aw_seen        <= 1'b0;
                w_seen         <= 1'b0;
                s_axil_awready <= 1'b1;
                s_axil_wready  <= 1'b1;
            end
        end
    end

    // ============================================================
    // AXI-Lite read handler
    // ============================================================
    always @(posedge user_clk) begin
        if (!user_resetn || !sys_rst_n) begin
            s_axil_arready <= 1'b1;
            s_axil_rdata   <= 32'd0;
            s_axil_rresp   <= 2'b00;
            s_axil_rvalid  <= 1'b0;
        end else begin
            if (s_axil_arready && s_axil_arvalid) begin
                s_axil_arready <= 1'b0;
                s_axil_rvalid  <= 1'b1;
                s_axil_rresp   <= 2'b00;

                case (s_axil_araddr[7:0])
                    REG_CTRL: begin
                        s_axil_rdata <= {28'd0, 1'b0, bypass_enable, 1'b0, stream_enable};
                    end
                    REG_STATUS: begin
                        s_axil_rdata <= {
                            22'd0,
                            user_lnk_up,
                            user_resetn,
                            sys_rst_n,
                            ph_busy,
                            s_axis_c2h_tvalid,
                            m_axis_h2c_tvalid,
                            bypass_enable,
                            stream_enable
                        };
                    end
                    REG_FRAME_CFG: begin
                        s_axil_rdata <= {NUM_LINES_U16, DEPTH_POINTS_U16};
                    end
                    REG_VERSION: begin
                        s_axil_rdata <= VERSION_ID;
                    end
                    REG_H2C_WORDS: begin
                        s_axil_rdata <= h2c_words;
                    end
                    REG_H2C_SAMPLES: begin
                        s_axil_rdata <= h2c_samples;
                    end
                    REG_H2C_LINES: begin
                        s_axil_rdata <= h2c_lines;
                    end
                    REG_C2H_WORDS: begin
                        s_axil_rdata <= c2h_words;
                    end
                    REG_C2H_FRAMES: begin
                        s_axil_rdata <= c2h_frames;
                    end
                    REG_BACKPRESSURE: begin
                        s_axil_rdata <= {h2c_backpressure[15:0], c2h_backpressure[15:0]};
                    end
                    REG_PH_EVENTS: begin
                        s_axil_rdata <= {
                            25'd0,
                            fft_event_data_out_channel_halt,
                            fft_event_data_in_channel_halt,
                            fft_event_status_channel_halt,
                            fft_event_tlast_missing,
                            fft_event_tlast_unexpected,
                            fft_event_frame_started,
                            ph_busy
                        };
                    end
                    REG_USER_CLK_HZ: begin
                        s_axil_rdata <= USER_CLK_HZ;
                    end
                    REG_LINE_CYCLES: begin
                        s_axil_rdata <= line_core_cycles;
                    end
                    REG_LINE_COUNT: begin
                        s_axil_rdata <= line_latency_count;
                    end
                    REG_DEBUG_FLAGS: begin
                        s_axil_rdata <= ph_debug_flags;
                    end
                    REG_PROC_CTRL: begin
                        s_axil_rdata <= {proc_log_offset, proc_log_gain_q4_4, proc_out_shift, proc_norm_shift};
                    end
                    default: begin
                        s_axil_rdata <= 32'd0;
                    end
                endcase
            end

            if (s_axil_rvalid && s_axil_rready) begin
                s_axil_rvalid  <= 1'b0;
                s_axil_arready <= 1'b1;
            end
        end
    end

    // ============================================================
    // Suppress unused-wire warnings
    // ============================================================
    wire _unused_frame = frame_tuser;
    wire _unused_proc  = ^proc_tdata_unused ^ proc_tvalid_unused ^
                         proc_tlast_unused ^ proc_tuser_unused;
    wire _unused_event = ^fft_event_frame_started ^ fft_event_tlast_unexpected ^
                         fft_event_tlast_missing ^ fft_event_status_channel_halt ^
                         fft_event_data_in_channel_halt ^ fft_event_data_out_channel_halt ^
                         frame_done_pulse;

endmodule

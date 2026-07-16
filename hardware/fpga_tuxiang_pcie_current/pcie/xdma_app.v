`timescale 1ps / 1ps

// XDMA user application wrapper.
// Connects the XDMA IP's AXI-Lite, H2C, and C2H streams to the image
// processing pipeline (ascent_pcie_ph_app). Motor/ADC/DAC ports are
// tied off since this design is image-processing only.
module xdma_app #(
    parameter TCQ                    = 1,
    parameter C_M_AXI_ID_WIDTH       = 4,
    parameter PL_LINK_CAP_MAX_LINK_WIDTH = 2,
    parameter C_DATA_WIDTH           = 64,
    parameter C_M_AXI_DATA_WIDTH     = C_DATA_WIDTH,
    parameter C_S_AXI_DATA_WIDTH     = C_DATA_WIDTH,
    parameter C_S_AXIS_DATA_WIDTH    = C_DATA_WIDTH,
    parameter C_M_AXIS_DATA_WIDTH    = C_DATA_WIDTH,
    parameter C_M_AXIS_RQ_USER_WIDTH = ((C_DATA_WIDTH == 512) ? 137 : 62),
    parameter C_S_AXIS_CQP_USER_WIDTH = ((C_DATA_WIDTH == 512) ? 183 : 88),
    parameter C_M_AXIS_RC_USER_WIDTH = ((C_DATA_WIDTH == 512) ? 161 : 75),
    parameter C_S_AXIS_CC_USER_WIDTH = ((C_DATA_WIDTH == 512) ?  81 : 33),
    parameter C_S_KEEP_WIDTH         = C_S_AXI_DATA_WIDTH / 32,
    parameter C_M_KEEP_WIDTH         = C_M_AXI_DATA_WIDTH / 32,
    parameter C_XDMA_NUM_CHNL        = 1
) (
    // AXI Lite Master Interface (from XDMA BAR)
    input  wire  [31:0] s_axil_awaddr,
    input  wire         s_axil_awvalid,
    output wire         s_axil_awready,
    input  wire  [31:0] s_axil_wdata,
    input  wire   [3:0] s_axil_wstrb,
    input  wire         s_axil_wvalid,
    output wire         s_axil_wready,
    output wire   [1:0] s_axil_bresp,
    output wire         s_axil_bvalid,
    input  wire         s_axil_bready,
    input  wire  [31:0] s_axil_araddr,
    input  wire         s_axil_arvalid,
    output wire         s_axil_arready,
    output wire  [31:0] s_axil_rdata,
    output wire   [1:0] s_axil_rresp,
    output wire         s_axil_rvalid,
    input  wire         s_axil_rready,

    // AXI streaming ports
    output wire [C_DATA_WIDTH-1:0]   s_axis_c2h_tdata_0,
    output wire                      s_axis_c2h_tlast_0,
    output wire                      s_axis_c2h_tvalid_0,
    input  wire                      s_axis_c2h_tready_0,
    output wire [C_DATA_WIDTH/8-1:0] s_axis_c2h_tkeep_0,
    input  wire [C_DATA_WIDTH-1:0]   m_axis_h2c_tdata_0,
    input  wire                      m_axis_h2c_tlast_0,
    input  wire                      m_axis_h2c_tvalid_0,
    output wire                      m_axis_h2c_tready_0,
    input  wire [C_DATA_WIDTH/8-1:0]  m_axis_h2c_tkeep_0,

    // System
    input  wire        user_resetn,
    input  wire        sys_rst_n,
    input  wire        user_clk,
    input  wire        user_lnk_up,

    output wire  [3:0] leds
);

    ascent_pcie_ph_app #(
        .DEPTH_POINTS (2048),
        .FFT_SIZE     (2048),
        .NUM_LINES    (4096),
        .VERSION_ID   (32'h2026_0619)
    ) u_app (
        .user_clk     (user_clk),
        .user_resetn  (user_resetn),
        .sys_rst_n    (sys_rst_n),
        .user_lnk_up  (user_lnk_up),

        .s_axil_awaddr (s_axil_awaddr),
        .s_axil_awvalid(s_axil_awvalid),
        .s_axil_awready(s_axil_awready),
        .s_axil_wdata  (s_axil_wdata),
        .s_axil_wstrb  (s_axil_wstrb),
        .s_axil_wvalid (s_axil_wvalid),
        .s_axil_wready (s_axil_wready),
        .s_axil_bresp  (s_axil_bresp),
        .s_axil_bvalid (s_axil_bvalid),
        .s_axil_bready (s_axil_bready),
        .s_axil_araddr (s_axil_araddr),
        .s_axil_arvalid(s_axil_arvalid),
        .s_axil_arready(s_axil_arready),
        .s_axil_rdata  (s_axil_rdata),
        .s_axil_rresp  (s_axil_rresp),
        .s_axil_rvalid (s_axil_rvalid),
        .s_axil_rready (s_axil_rready),

        .m_axis_h2c_tdata (m_axis_h2c_tdata_0),
        .m_axis_h2c_tlast (m_axis_h2c_tlast_0),
        .m_axis_h2c_tvalid(m_axis_h2c_tvalid_0),
        .m_axis_h2c_tready(m_axis_h2c_tready_0),
        .m_axis_h2c_tkeep (m_axis_h2c_tkeep_0),

        .s_axis_c2h_tdata (s_axis_c2h_tdata_0),
        .s_axis_c2h_tlast (s_axis_c2h_tlast_0),
        .s_axis_c2h_tvalid(s_axis_c2h_tvalid_0),
        .s_axis_c2h_tready(s_axis_c2h_tready_0),
        .s_axis_c2h_tkeep (s_axis_c2h_tkeep_0),

        .leds(leds)
    );

endmodule

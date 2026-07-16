`timescale 1ps / 1ps

// Top-level PCIe endpoint wrapper.
// Instantiates the XDMA IP and the image-processing application layer.
// No motor/ADC/DAC — pure image streaming (RK3588 <-> FPGA).
module xilinx_dma_pcie_ep #(
    parameter PL_LINK_CAP_MAX_LINK_WIDTH = 2,    // 1-X1; 2-X2; 4-X4
    parameter PL_SIM_FAST_LINK_TRAINING  = "FALSE",
    parameter PL_LINK_CAP_MAX_LINK_SPEED = 2,    // 1-GEN1; 2-GEN2; 4-GEN3
    parameter C_DATA_WIDTH               = 64,
    parameter EXT_PIPE_SIM               = "FALSE",
    parameter C_ROOT_PORT                = "FALSE",
    parameter C_DEVICE_NUMBER            = 0
) (
    output [(PL_LINK_CAP_MAX_LINK_WIDTH-1):0] pci_exp_txp,
    output [(PL_LINK_CAP_MAX_LINK_WIDTH-1):0] pci_exp_txn,
    input  [(PL_LINK_CAP_MAX_LINK_WIDTH-1):0] pci_exp_rxp,
    input  [(PL_LINK_CAP_MAX_LINK_WIDTH-1):0] pci_exp_rxn,

    input  sys_clk_p,
    input  sys_clk_n,
    input  sys_rst_n,

    output wire [3:0] leds
);

    localparam TCQ = 1;
    localparam C_S_AXI_ID_WIDTH  = 4;
    localparam C_M_AXI_ID_WIDTH  = 4;
    localparam C_S_AXI_DATA_WIDTH = C_DATA_WIDTH;
    localparam C_M_AXI_DATA_WIDTH = C_DATA_WIDTH;
    localparam C_S_AXI_ADDR_WIDTH = 64;
    localparam C_M_AXI_ADDR_WIDTH = 64;
    localparam C_NUM_USR_IRQ = 1;

    wire user_lnk_up;
    wire user_clk;
    wire user_resetn;

    // Ref clock buffer
    wire sys_clk;
    wire sys_rst_n_c;
    IBUFDS_GTE2 refclk_ibuf (.O(sys_clk), .ODIV2(), .I(sys_clk_p), .CEB(1'b0), .IB(sys_clk_n));
    IBUF      sys_reset_n_ibuf (.O(sys_rst_n_c), .I(sys_rst_n));

    // XDMA AXI-Lite (master -> slave in app)
    wire [31:0] m_axil_awaddr;
    wire [2:0]  m_axil_awprot;
    wire        m_axil_awvalid, m_axil_awready;
    wire [31:0] m_axil_wdata;
    wire [3:0]  m_axil_wstrb;
    wire        m_axil_wvalid, m_axil_wready;
    wire        m_axil_bvalid, m_axil_bready;
    wire [1:0]  m_axil_bresp;
    wire [31:0] m_axil_araddr;
    wire [2:0]  m_axil_arprot;
    wire        m_axil_arvalid, m_axil_arready;
    wire [31:0] m_axil_rdata;
    wire [1:0]  m_axil_rresp;
    wire        m_axil_rvalid, m_axil_rready;

    // XDMA H2C / C2H streams
    wire [C_DATA_WIDTH-1:0]   m_axis_h2c_tdata_0;
    wire                      m_axis_h2c_tlast_0;
    wire                      m_axis_h2c_tvalid_0;
    wire                      m_axis_h2c_tready_0;
    wire [C_DATA_WIDTH/8-1:0] m_axis_h2c_tkeep_0;
    wire [C_DATA_WIDTH-1:0]   s_axis_c2h_tdata_0;
    wire                      s_axis_c2h_tlast_0;
    wire                      s_axis_c2h_tvalid_0;
    wire                      s_axis_c2h_tready_0;
    wire [C_DATA_WIDTH/8-1:0] s_axis_c2h_tkeep_0;

    wire        usr_irq_req = 1'b0;
    wire        usr_irq_ack;
    wire        msi_enable;
    wire [2:0]  msi_vector_width;

    // ================================================================
    // XDMA IP Core
    // ================================================================
    ascent_xdma_stream_0 ascent_xdma_stream_0_i (
        .sys_rst_n       (sys_rst_n_c),
        .sys_clk         (sys_clk),

        .pci_exp_txn     (pci_exp_txn),
        .pci_exp_txp     (pci_exp_txp),
        .pci_exp_rxn     (pci_exp_rxn),
        .pci_exp_rxp     (pci_exp_rxp),

        .s_axis_c2h_tdata_0 (s_axis_c2h_tdata_0),
        .s_axis_c2h_tlast_0 (s_axis_c2h_tlast_0),
        .s_axis_c2h_tvalid_0(s_axis_c2h_tvalid_0),
        .s_axis_c2h_tready_0(s_axis_c2h_tready_0),
        .s_axis_c2h_tkeep_0 (s_axis_c2h_tkeep_0),
        .m_axis_h2c_tdata_0 (m_axis_h2c_tdata_0),
        .m_axis_h2c_tlast_0 (m_axis_h2c_tlast_0),
        .m_axis_h2c_tvalid_0(m_axis_h2c_tvalid_0),
        .m_axis_h2c_tready_0(m_axis_h2c_tready_0),
        .m_axis_h2c_tkeep_0 (m_axis_h2c_tkeep_0),

        .m_axil_awaddr    (m_axil_awaddr),
        .m_axil_awprot    (m_axil_awprot),
        .m_axil_awvalid   (m_axil_awvalid),
        .m_axil_awready   (m_axil_awready),
        .m_axil_wdata     (m_axil_wdata),
        .m_axil_wstrb     (m_axil_wstrb),
        .m_axil_wvalid    (m_axil_wvalid),
        .m_axil_wready    (m_axil_wready),
        .m_axil_bvalid    (m_axil_bvalid),
        .m_axil_bresp     (m_axil_bresp),
        .m_axil_bready    (m_axil_bready),
        .m_axil_araddr    (m_axil_araddr),
        .m_axil_arprot    (m_axil_arprot),
        .m_axil_arvalid   (m_axil_arvalid),
        .m_axil_arready   (m_axil_arready),
        .m_axil_rdata     (m_axil_rdata),
        .m_axil_rresp     (m_axil_rresp),
        .m_axil_rvalid    (m_axil_rvalid),
        .m_axil_rready    (m_axil_rready),

        .usr_irq_req      (usr_irq_req),
        .usr_irq_ack      (usr_irq_ack),
        .msi_enable       (msi_enable),
        .msi_vector_width (msi_vector_width),

        .cfg_mgmt_addr         (19'b0),
        .cfg_mgmt_write        (1'b0),
        .cfg_mgmt_write_data   (32'b0),
        .cfg_mgmt_byte_enable  (4'b0),
        .cfg_mgmt_read         (1'b0),
        .cfg_mgmt_read_data    (),
        .cfg_mgmt_read_write_done (),
        .cfg_mgmt_type1_cfg_reg_access (1'b0),

        .axi_aclk        (user_clk),
        .axi_aresetn     (user_resetn),
        .user_lnk_up     (user_lnk_up)
    );

    // ================================================================
    // User application: image processing pipeline
    // ================================================================
    xdma_app #(
        .C_M_AXI_ID_WIDTH(C_M_AXI_ID_WIDTH)
    ) xdma_app_i (
        .s_axil_awaddr (m_axil_awaddr[31:0]),
        .s_axil_awvalid(m_axil_awvalid),
        .s_axil_awready(m_axil_awready),
        .s_axil_wdata  (m_axil_wdata[31:0]),
        .s_axil_wstrb  (m_axil_wstrb[3:0]),
        .s_axil_wvalid (m_axil_wvalid),
        .s_axil_wready (m_axil_wready),
        .s_axil_bresp  (m_axil_bresp),
        .s_axil_bvalid (m_axil_bvalid),
        .s_axil_bready (m_axil_bready),
        .s_axil_araddr (m_axil_araddr[31:0]),
        .s_axil_arvalid(m_axil_arvalid),
        .s_axil_arready(m_axil_arready),
        .s_axil_rdata  (m_axil_rdata),
        .s_axil_rresp  (m_axil_rresp),
        .s_axil_rvalid (m_axil_rvalid),
        .s_axil_rready (m_axil_rready),

        .s_axis_c2h_tdata_0 (s_axis_c2h_tdata_0),
        .s_axis_c2h_tlast_0 (s_axis_c2h_tlast_0),
        .s_axis_c2h_tvalid_0(s_axis_c2h_tvalid_0),
        .s_axis_c2h_tready_0(s_axis_c2h_tready_0),
        .s_axis_c2h_tkeep_0 (s_axis_c2h_tkeep_0),
        .m_axis_h2c_tdata_0 (m_axis_h2c_tdata_0),
        .m_axis_h2c_tlast_0 (m_axis_h2c_tlast_0),
        .m_axis_h2c_tvalid_0(m_axis_h2c_tvalid_0),
        .m_axis_h2c_tready_0(m_axis_h2c_tready_0),
        .m_axis_h2c_tkeep_0 (m_axis_h2c_tkeep_0),

        .user_clk    (user_clk),
        .user_resetn (user_resetn),
        .sys_rst_n   (sys_rst_n_c),
        .user_lnk_up (user_lnk_up),

        .leds(leds)
    );

endmodule

// XDMA IP black-box declaration for top-level synthesis.
// Vivado stitches the generated ascent_xdma_stream_0 checkpoint during implementation.
(* black_box, syn_black_box *)
module ascent_xdma_stream_0 (
    input  wire        sys_clk,
    input  wire        sys_rst_n,
    output wire        user_lnk_up,

    output wire [1:0]  pci_exp_txp,
    output wire [1:0]  pci_exp_txn,
    input  wire [1:0]  pci_exp_rxp,
    input  wire [1:0]  pci_exp_rxn,

    output wire        axi_aclk,
    output wire        axi_aresetn,

    input  wire [0:0]  usr_irq_req,
    output wire [0:0]  usr_irq_ack,
    output wire        msi_enable,
    output wire [2:0]  msi_vector_width,

    output wire [31:0] m_axil_awaddr,
    output wire [2:0]  m_axil_awprot,
    output wire        m_axil_awvalid,
    input  wire        m_axil_awready,
    output wire [31:0] m_axil_wdata,
    output wire [3:0]  m_axil_wstrb,
    output wire        m_axil_wvalid,
    input  wire        m_axil_wready,
    input  wire        m_axil_bvalid,
    input  wire [1:0]  m_axil_bresp,
    output wire        m_axil_bready,
    output wire [31:0] m_axil_araddr,
    output wire [2:0]  m_axil_arprot,
    output wire        m_axil_arvalid,
    input  wire        m_axil_arready,
    input  wire [31:0] m_axil_rdata,
    input  wire [1:0]  m_axil_rresp,
    input  wire        m_axil_rvalid,
    output wire        m_axil_rready,

    input  wire [18:0] cfg_mgmt_addr,
    input  wire        cfg_mgmt_write,
    input  wire [31:0] cfg_mgmt_write_data,
    input  wire [3:0]  cfg_mgmt_byte_enable,
    input  wire        cfg_mgmt_read,
    output wire [31:0] cfg_mgmt_read_data,
    output wire        cfg_mgmt_read_write_done,
    input  wire        cfg_mgmt_type1_cfg_reg_access,

    input  wire [63:0] s_axis_c2h_tdata_0,
    input  wire        s_axis_c2h_tlast_0,
    input  wire        s_axis_c2h_tvalid_0,
    output wire        s_axis_c2h_tready_0,
    input  wire [7:0]  s_axis_c2h_tkeep_0,

    output wire [63:0] m_axis_h2c_tdata_0,
    output wire        m_axis_h2c_tlast_0,
    output wire        m_axis_h2c_tvalid_0,
    input  wire        m_axis_h2c_tready_0,
    output wire [7:0]  m_axis_h2c_tkeep_0
);

endmodule

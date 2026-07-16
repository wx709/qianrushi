open_project [file join [file dirname [info script]] tuxiang_pcie.xpr]

# Add new wrapper files
set src_root [file normalize [file join [file dirname [info script]] ..]]
set pcie_dir [file join $src_root pcie]

foreach f {xdma_app.v xilinx_dma_pcie_ep.sv} {
    set full [file join $pcie_dir $f]
    if {[file exists $full]} {
        if {[llength [get_files -quiet $full]] == 0} {
            add_files -norecurse $full
            puts "ADD: $f"
        }
    }
}

# Set the real top module
set_property top xilinx_dma_pcie_ep [get_filesets sources_1]
update_compile_order -fileset sources_1

puts "\nTop: [get_property TOP [get_filesets sources_1]]"
puts "Sources: [llength [get_files -of_objects [get_filesets sources_1]]]"

close_project

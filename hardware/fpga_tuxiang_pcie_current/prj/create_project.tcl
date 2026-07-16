
set project_name tuxiang_pcie
set project_dir [file normalize [file dirname [info script]]]
set src_root [file normalize [file join $project_dir ..]]

create_project $project_name $project_dir -part xc7a35tfgg484-2 -force
set_property target_language Verilog [current_project]

# --- Add source files ---
set ph_dir [file join $src_root ph_pcie_ip]
set pcie_dir [file join $src_root pcie]

# All .v from ph_pcie_ip
foreach f [glob -nocomplain [file join $ph_dir *.v]] {
    add_files -norecurse $f
    puts "  ADD: [file tail $f]"
}

# FFT IPs from ph_pcie_ip
foreach ip {xfft_in xfft_0 xfft_0_w29} {
    set xci [file join $ph_dir $ip $ip.xci]
    if {[file exists $xci]} {
        add_files -norecurse $xci
        puts "  ADD: $ip.xci"
    }
}

# All .v/.sv from pcie
foreach f [concat \
    [glob -nocomplain [file join $pcie_dir *.v]] \
    [glob -nocomplain [file join $pcie_dir *.sv]]] {
    add_files -norecurse $f
    puts "  ADD: [file tail $f]"
}

# XDMA IP from pcie
set xdma_xci [file join $pcie_dir ascent_xdma_stream_0 ascent_xdma_stream_0.xci]
if {[file exists $xdma_xci]} {
    add_files -norecurse $xdma_xci
    puts "  ADD: ascent_xdma_stream_0.xci"
}

# --- Add constraint ---
set xdc [file join $src_root constraints ascent_pro_xdma_pcie.xdc]
if {[file exists $xdc]} {
    add_files -fileset constrs_1 -norecurse $xdc
    set_property USED_IN {synthesis implementation} [get_files $xdc]
    set_property PROCESSING_ORDER LATE [get_files $xdc]
    puts "  ADD: ascent_pro_xdma_pcie.xdc"
}

# --- Set top ---
set_property top xilinx_dma_pcie_ep [get_filesets sources_1]

# --- Generate IPs ---
puts "\n--- Upgrading & generating IPs ---"
foreach ip_name {xfft_in xfft_0 xfft_0_w29 ascent_xdma_stream_0} {
    set ip_obj [get_ips -quiet $ip_name]
    if {[llength $ip_obj] > 0} {
        upgrade_ip $ip_obj -quiet
        reset_target all $ip_obj -quiet
        generate_target all $ip_obj -force
        puts "  IP $ip_name done"
    }
}

update_compile_order -fileset sources_1

puts "\n=== Project created ==="
puts "  Path: $project_dir/$project_name.xpr"
puts "  Part: xc7a35tfgg484-2"
puts "  Top:  xilinx_dma_pcie_ep"
puts "  Sources: [llength [get_files -of_objects [get_filesets sources_1]]] files"

close_project

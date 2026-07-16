set script_dir [file normalize [file dirname [info script]]]
set project_path [file join $script_dir tuxiang_pcie.xpr]

open_project $project_path
set_property top xilinx_dma_pcie_ep [get_filesets sources_1]

set stale_xdma_stub [file normalize [file join $script_dir .. pcie ascent_xdma_stream_0_stub.v]]
set stale_file_obj [get_files -quiet $stale_xdma_stub]
if {[llength $stale_file_obj] > 0} {
    remove_files $stale_file_obj
    puts "REMOVED_STALE_FILE=$stale_xdma_stub"
}

foreach ip_name {xfft_in xfft_0 xfft_0_w29 ascent_xdma_stream_0} {
    set ip_obj [get_ips -quiet $ip_name]
    if {[llength $ip_obj] > 0} {
        upgrade_ip $ip_obj -quiet
        reset_target all $ip_obj -quiet
        generate_target all $ip_obj -force
        puts "IP_REGENERATED=$ip_name"
    }
}

update_compile_order -fileset sources_1

foreach ip_run_name {xfft_0_w29_synth_1 ascent_xdma_stream_0_synth_1} {
    set ip_run [get_runs -quiet $ip_run_name]
    if {[llength $ip_run] > 0} {
        reset_run $ip_run
        launch_runs $ip_run -jobs 4
        wait_on_run $ip_run
        set ip_status [get_property STATUS $ip_run]
        puts "IP_SYNTH_STATUS($ip_run_name)=$ip_status"
        if {([string first "Complete" $ip_status] < 0) &&
            ([string first "cached IP results" $ip_status] < 0)} {
            puts "ERROR: IP synthesis did not complete: $ip_run_name"
            exit 1
        }
    } else {
        puts "IP_SYNTH_STATUS($ip_run_name)=SKIPPED_NO_RUN"
    }
}

reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
set synth_status [get_property STATUS [get_runs synth_1]]
puts "SYNTH_STATUS=$synth_status"
if {[string first "Complete" $synth_status] < 0} {
    puts "ERROR: synthesis did not complete"
    exit 1
}

reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
set impl_status [get_property STATUS [get_runs impl_1]]
puts "IMPL_STATUS=$impl_status"
if {[string first "Complete" $impl_status] < 0} {
    puts "ERROR: implementation/bitstream did not complete"
    exit 1
}

open_run impl_1
report_timing_summary -file [file join $script_dir tuxiang_pcie.runs impl_1 xilinx_dma_pcie_ep_timing_summary_routed.rpt] -delay_type min_max -report_unconstrained -check_timing_verbose -max_paths 10 -input_pins
report_drc -file [file join $script_dir tuxiang_pcie.runs impl_1 xilinx_dma_pcie_ep_drc_routed.rpt]
report_route_status -file [file join $script_dir tuxiang_pcie.runs impl_1 xilinx_dma_pcie_ep_route_status.rpt]

puts "BITSTREAM=[file join $script_dir tuxiang_pcie.runs impl_1 xilinx_dma_pcie_ep.bit]"
close_project

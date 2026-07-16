open_project [file join [file dirname [info script]] tuxiang_pcie.xpr]

puts "=== Top: [get_property TOP [get_filesets sources_1]] ==="
puts "=== Part: [get_property PART [current_project]] ==="

puts "\n=== IP Status ==="
foreach ip [get_ips -quiet] {
    set name [get_property NAME $ip]
    set ipdef [get_property IPDEF $ip]
    set is_locked [get_property IS_LOCKED $ip]
    puts [format "  %-30s type=%-25s locked=%s" $name $ipdef $is_locked]
}

puts "\n=== FFT IP Parameters ==="
foreach ip [get_ips -quiet -filter {IPDEF =~ "*xfft*"}] {
    set name [get_property NAME $ip]
    set nfft [get_property CONFIG.transform_length $ip]
    set aresetn [get_property CONFIG.aresetn $ip]
    set arch [get_property CONFIG.implementation_options $ip]
    puts [format "  %s: N=%s  aresetn=%s  arch=%s" $name $nfft $aresetn $arch]
}

puts "\n=== Module Hierarchy (top 3 levels) ==="
set top [get_property TOP [get_filesets sources_1]]
set top_cells [get_cells -quiet -hier -filter "PRIMITIVE_SUBGROUP == $top"]
# Try direct hierarchy
set refs [get_cells -quiet -hierarchical -filter "NAME =~ *"]
if {[llength $refs] == 0} {
    puts "  (需要先 run synthesis 才能看 hierarchy)"
}

puts "\n=== Sources Check ==="
foreach f [get_files -quiet -of_objects [get_filesets sources_1] -filter {FILE_TYPE == "Verilog" || FILE_TYPE == "SystemVerilog"}] {
    set name [file tail $f]
    set lib [get_property LIBRARY $f]
    set used [get_property USED_IN_SYNTHESIS $f]
    puts [format "  %-40s lib=%s synth=%s" $name $lib $used]
}

puts "\n=== Constraint Check ==="
foreach f [get_files -quiet -of_objects [get_filesets constrs_1]] {
    puts "  [file tail $f]"
    puts "    USED_IN: [get_property USED_IN $f]"
    puts "    PROCESSING_ORDER: [get_property PROCESSING_ORDER $f]"
}

close_project

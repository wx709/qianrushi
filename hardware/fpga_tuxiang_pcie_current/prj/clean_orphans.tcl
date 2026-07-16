open_project [file join [file dirname [info script]] tuxiang_pcie.xpr]
set f [get_files -quiet -filter {NAME =~ "*ph_ip_pcie*"}]
if {[llength $f] > 0} {
    remove_files $f -quiet
    puts "REMOVED: ph_ip_pcie.v"
}
update_compile_order -fileset sources_1
close_project

open_project [file join [file dirname [info script]] tuxiang_pcie.xpr]
reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
set status [get_property STATUS [get_runs synth_1]]
puts "SYNTH_STATUS=$status"
close_project

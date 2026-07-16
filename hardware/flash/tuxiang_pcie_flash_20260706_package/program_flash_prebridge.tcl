set mcsfile {D:/FPGA20251016/qianrushi/_codex_generated_temp/tuxiang_pcie_flash_20260706_package/xilinx_dma_pcie_ep_20260706_package.mcs}
set bridge_bit {D:/FPGA20251016/2025.2/Vivado/data/xicom/cfgmem/bitfile/spi_xc7a35t_pullnone.bit}
set cfgmem_name {mt25ql128-spi-x1_x2_x4}
set hw_url {localhost:3121}
set target_pattern {*Digilent/210299767327*}
set device_name {xc7a35t_0}

puts "=== program_flash_prebridge 20260706 package ==="
puts "mcsfile=$mcsfile"
puts "bridge_bit=$bridge_bit"
puts "cfgmem_part=$cfgmem_name"

foreach f [list $mcsfile $bridge_bit] {
    if {![file exists $f]} {
        error "required file not found: $f"
    }
}

open_hw_manager
connect_hw_server -url $hw_url
refresh_hw_server

set targets [get_hw_targets -quiet $target_pattern]
if {[llength $targets] == 0} { set targets [get_hw_targets -quiet *] }
if {[llength $targets] == 0} { error "no hardware targets found" }

current_hw_target [lindex $targets 0]
catch {set_property PARAM.FREQUENCY 125000 [current_hw_target]}
puts "target=[current_hw_target]"
catch {puts "jtag_frequency=[get_property PARAM.FREQUENCY [current_hw_target]]"}
open_hw_target

set devs [get_hw_devices -quiet $device_name]
if {[llength $devs] == 0} { set devs [get_hw_devices -quiet *] }
if {[llength $devs] == 0} { error "hardware device not found" }
set dev [lindex $devs 0]
current_hw_device $dev
puts "device=$dev"
puts "part=[get_property PART $dev]"
puts "idcode=[get_property IDCODE $dev]"

puts "programming_bridge_bit"
set_property PROGRAM.FILE $bridge_bit $dev
program_hw_devices $dev
after 3000
refresh_hw_device -update_hw_probes false $dev
catch {puts "bridge_done_pin=[get_property REGISTER.IR.BIT5_DONE $dev]"}
catch {puts "bridge_config_status=[get_property REGISTER.CONFIG_STATUS $dev]"}

set cfgmem_part [lindex [get_cfgmem_parts $cfgmem_name] 0]
puts "using_cfgmem_part=$cfgmem_part"
create_hw_cfgmem -hw_device $dev $cfgmem_part
set cfgmem [current_hw_cfgmem]
puts "cfgmem=$cfgmem"
catch {puts "device_bridge_bit=[get_property PROGRAM.HW_CFGMEM_BITFILE $dev]"}

set_property PROGRAM.FILES [list $mcsfile] $cfgmem
set_property PROGRAM.ADDRESS_RANGE {use_file} $cfgmem
set_property PROGRAM.BLANK_CHECK 1 $cfgmem
set_property PROGRAM.ERASE 1 $cfgmem
set_property PROGRAM.CFG_PROGRAM 1 $cfgmem
set_property PROGRAM.VERIFY 1 $cfgmem

puts "starting_program_hw_cfgmem"
program_hw_cfgmem $cfgmem
puts "program_hw_cfgmem_completed"

puts "booting_hw_device"
boot_hw_device $dev
after 3000
refresh_hw_device -update_hw_probes false $dev
catch {puts "done_pin=[get_property REGISTER.IR.BIT5_DONE $dev]"}
catch {puts "boot_status=[get_property REGISTER.BOOT_STATUS $dev]"}
catch {puts "config_status=[get_property REGISTER.CONFIG_STATUS $dev]"}

close_hw_manager
puts "=== done ==="

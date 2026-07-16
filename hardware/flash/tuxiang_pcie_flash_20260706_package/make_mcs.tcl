set bitfile {D:/FPGA20251016/tuxiang_pcie/prj/tuxiang_pcie.runs/impl_1/xilinx_dma_pcie_ep.bit}
set outdir {D:/FPGA20251016/qianrushi/_codex_generated_temp/tuxiang_pcie_flash_20260706_package}
set mcsfile "$outdir/xilinx_dma_pcie_ep_20260706_package.mcs"

puts "=== make_mcs 20260706 package ==="
puts "bitfile=$bitfile"
puts "mcsfile=$mcsfile"
puts "format=MCS interface=SPIx4 size=16MB"

if {![file exists $bitfile]} {
    error "bitfile does not exist: $bitfile"
}

file mkdir $outdir
write_cfgmem -force -format mcs -interface SPIx4 -size 16 -loadbit "up 0x0 $bitfile" -file $mcsfile

puts "generated_mcs=$mcsfile"
puts "=== done ==="

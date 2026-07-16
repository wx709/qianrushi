## Wildfire Ascent Pro / Artix-7 PCIe endpoint constraints.
## Image-processing-only design (no motor).

set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.GENERAL.COMPRESS true [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 50 [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property CONFIG_MODE SPIx4 [current_design]

## PCIe reference clock
set_property PACKAGE_PIN F10 [get_ports sys_clk_p]

## PCIe lanes (x2)
set_property PACKAGE_PIN D9  [get_ports {pci_exp_rxp[0]}]
set_property PACKAGE_PIN B10 [get_ports {pci_exp_rxp[1]}]

## System reset
set_property PACKAGE_PIN F21 [get_ports sys_rst_n]
set_property IOSTANDARD LVCMOS33 [get_ports sys_rst_n]
set_property PULLUP true [get_ports sys_rst_n]

## LEDs
set_property IOSTANDARD LVCMOS33 [get_ports {leds[3]}]
set_property IOSTANDARD LVCMOS33 [get_ports {leds[2]}]
set_property IOSTANDARD LVCMOS33 [get_ports {leds[1]}]
set_property IOSTANDARD LVCMOS33 [get_ports {leds[0]}]
set_property PACKAGE_PIN M21 [get_ports {leds[3]}]
set_property PACKAGE_PIN L21 [get_ports {leds[2]}]
set_property PACKAGE_PIN K21 [get_ports {leds[1]}]
set_property PACKAGE_PIN K22 [get_ports {leds[0]}]

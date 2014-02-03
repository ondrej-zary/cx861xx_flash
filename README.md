cx861xx_flash - Conexant CX861xx USB Boot Flash Utility
Copyright (c) 2012 Ondrej Zary - http://www.rainbow-software.org
================================================================

This utility can access external flash ROM on boards/devices with Conexant CX861xx
(CX86111/CX86113) Network Processors, such as Flarion Desktop Modem.

Requirements:
-------------
Linux, GCC, Make, Libusb-1.0
Probably works correctly only on little endian machines (such as x86).

Compilation:
------------
$ make


Notes:
------
Only Intel 28F320C3B flash is supported for now.

USB flashing is slow. The first working version ran for more than two hours.
Now after three optimizations, flashing a typical image takes about 20 minutes
 - depends on how many empty (FFFF) words are present.

The processor must be in USB Boot mode for the utility to work.


USB Boot mode on Flarion Desktop Modem:
---------------------------------------
There are two versions of Flarion Desktop Modem. Remove the bottom grey
plastic. If you see pads with "RS323" and "USB BOOT" marking, it's v2. If
not (only shields), it's v1 and you have to completely disassemble it.

v1: Completely disassemble the device, remove the PCB and turn it over to
access the bottom side. With antenna connector positioned top left and connectors
on the right side, lift the shielding cover in the upper right corner.
You should see gold pads in the following shape:

        USB boot jumper
        vvvv
uP      |     |       GND
           |       TX  |  RX
_GND                |     |
                     RS232

Use a piece of wire to conect two left pads and apply power, keeping pads
shorted. You can then remove the short.

v2: Remove bottom plastic and use a piece of wire to connect holes marked
"USB BOOT".

Conect USB cable to PC. You should something like this in lsusb:
Bus 002 Device 002: ID 0572:cafc Conexant Systems (Rockwell), Inc. CX861xx ROM Boot Loader

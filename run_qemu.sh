#!/bin/bash

$PWD/flash.sh $PWD/build/lwm2m-project.bin > /dev/null 2>&1
qemu-system-xtensa -nographic -M esp32 -m 4 -drive file=flash.bin,if=mtd,format=raw -nic user,model=open_eth,hostfwd=tcp::80-:80

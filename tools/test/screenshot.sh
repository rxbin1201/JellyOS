#!/bin/bash
# Boot build/esp in QEMU without a window, run monitor commands, save a screenshot.
#
# usage: tools/test/screenshot.sh SECONDS [COMMAND_FILE] [OUTPUT.png]
#
# COMMAND_FILE has one QEMU monitor (HMP) command per line, e.g. "sendkey a";
# "sleep N" pauses. The serial console goes to build/screenshot-serial.log.
cd "$(dirname "$0")/../.."
secs=$1
script=$2
output=${3:-build/screenshot.png}
rm -f build/screenshot.ppm build/monitor.sock
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_screenshot.fd
timeout $((secs + 120)) qemu-system-x86_64 -machine q35 -m 512M -no-reboot -net none \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=build/OVMF_VARS_screenshot.fd \
    -drive format=raw,file=fat:rw:build/esp \
    -device virtio-keyboard-pci -device virtio-tablet-pci \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -serial file:build/screenshot-serial.log -display none \
    -monitor unix:build/monitor.sock,server,nowait &
qemu=$!
sleep "$secs"
if [ -n "$script" ]; then
    while IFS= read -r command; do
        case "$command" in
            sleep\ *) sleep "${command#sleep }" ;;
            ""|\#*) ;;
            *) python3 tools/test/qemu_monitor.py build/monitor.sock "$command" ;;
        esac
    done < "$script"
fi
python3 tools/test/qemu_monitor.py build/monitor.sock "screendump build/screenshot.ppm"
sleep 1
python3 tools/test/qemu_monitor.py build/monitor.sock quit
wait $qemu 2>/dev/null
python3 tools/test/ppm2png.py build/screenshot.ppm "$output"

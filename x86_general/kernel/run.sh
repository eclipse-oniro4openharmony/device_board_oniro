qemu-system-x86_64 -machine q35 -smp 6 -m 4096M -boot c -nographic -vga none -device virtio-gpu-pci,xres=360,yres=720,max_outputs=1,addr=08.0 -display sdl,gl=off -rtc base=utc,clock=host -device es1370 -initrd ramdisk.img -kernel bzImage \
-drive if=none,file=updater.img,format=raw,id=updater,index=0 \
-device virtio-blk-pci,drive=updater \
-drive if=none,file=system.img,format=raw,id=system,index=1 \
-device virtio-blk-pci,drive=system \
-drive if=none,file=vendor.img,format=raw,id=vendor,index=2 \
-device virtio-blk-pci,drive=vendor \
-drive if=none,file=userdata.img,format=raw,id=userdata,index=3 \
-device virtio-blk-pci,drive=userdata \
-append "ip=dhcp loglevel=4 console=ttyS0,115200 init=init root=/dev/ram0 rw  ohos.boot.hardware=x86_general ohos.required_mount.system=/dev/block/vdb@/usr@ext4@ro,barrier=1@wait,required ohos.required_mount.vendor=/dev/block/vdc@/vendor@ext4@ro,barrier=1@wait,required ohos.required_mount.misc=/dev/block/vda@/misc@none@none=@wait,required" -enable-kvm -cpu host -netdev user,id=net0,hostfwd=tcp::55555-:55555 -device virtio-net-pci,netdev=net0

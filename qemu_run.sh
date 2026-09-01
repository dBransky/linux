#!/bin/bash

DEBUG_MODE=0

# Check for the -g flag
while getopts "g" opt; do
  case $opt in
    g)
      DEBUG_MODE=1
      ;;
    *)
      ;;
  esac
done

# Build the QEMU command
sudo chmod 0777 /dev/kvm
QEMU_CMD="qemu-system-x86_64 \
        -m ${QEMU_MEM:-130M} \
        -smp 1 \
        -kernel ./arch/x86/boot/bzImage \
        -append \"console=ttyS0 root=/dev/sda earlyprintk=serial net.ifnames=0 nokaslr debug ${QEMU_APPEND_EXTRA:-}\" \
        -drive file=/scratch/bullseye.img,format=raw \
        ${QEMU_EXTRA_DISK:+-drive file=${QEMU_EXTRA_DISK},format=raw} \
        -virtfs local,path=/scratch/swap_tests,mount_tag=tests,security_model=none \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:10021-:22 \
        -device virtio-net-pci,netdev=net0 \
        -enable-kvm \
        -nographic \
        -pidfile vm.pid"

# Add debugging options if -g is passed
if [ $DEBUG_MODE -eq 1 ]; then
  echo "Debug mode enabled"
  QEMU_CMD="$QEMU_CMD  -s -S"
fi

# Run the QEMU command and log output
eval $QEMU_CMD 2>&1 | tee vm.log
# for i in {1..100}; do dd if=/dev/zero of=/scratch/vma_swaps/swapfile_$i.swap bs=1G count=1 status=progress; done     
# ./minimal_bench/a.out -s 200 -b 536870912 100 -i 100 -r 1 -w 1 
# for i in {1..200}; do
#   sudo swapon /scratch/vma_swaps/swapfile_$i.swap
# done

# for i in {72..200}; do
#   echo "/scratch/vma_swaps/swapfile_$i.swap none swap sw 0 0" >> /etc/fstab
# done
# for i in {1..100}; do
#   rm /scratch/vma_swaps/swapfile_$i.swap
# done
  #  echo "/scratch/vma_swaps/swapfile_$i.swap none swap sw 0 0" >> /etc/fstab
# for f in /scratch/vma_swaps/swapfile_*.swap; do
# done
# for i in {1..200}; do
#   sudo echo "/scratch/vma_swaps/swapfile_$i.swap none swap sw 0 0" >> /etc/fstab
# done
# ./minimal_bench/a.out -s 200 -b 500000000 -i 1 -r 1 -w 1 -f /scratch/vma_swaps/swapfile_300.swap
# sudo dd if=/dev/zero of=256K.file bs=256K count=1 conv=fsync
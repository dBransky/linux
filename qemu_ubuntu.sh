#!/bin/bash
# Boot a real Ubuntu 24.04 cloud guest with this tree's nswap bzImage.
# Full systemd (sshd, snap, rsyslog, ...) — not the syzkaller emergency image.
#
# SSH: ssh -i ~/.ssh/id_ed25519 -p 10022 -o StrictHostKeyChecking=no \
#          -o UserKnownHostsFile=/dev/null root@127.0.0.1
#      (ubuntu@ after cloud-init; same key)
# Serial log: ubuntu-vm.log
#
# Reset disk: rm -f /scratch/nswap-ubuntu-vm/overlay.qcow2 && ./qemu_ubuntu.sh
#   (recreates overlay from the pristine cloud image)
#
# This kernel's early root lookup does not support LABEL=/UUID= without
# initramfs (see block/early-lookup.c). Boot with root=/dev/vda1.
# CONFIG_ISO9660_FS is off, so cloud-init seed is a VFAT cidata disk.

set -euo pipefail
cd "$(dirname "$0")"
sudo chmod 0777 /dev/kvm

IMGDIR=/scratch/nswap-ubuntu-vm
BASE=$IMGDIR/ubuntu-24.04-server-cloudimg-amd64.img
OVERLAY=$IMGDIR/overlay.qcow2
CIDATA=$IMGDIR/cidata.img
NSWAPIMG=$IMGDIR/nswap.img
PUBKEY=${PUBKEY:-$HOME/.ssh/id_ed25519.pub}
KERNEL=./arch/x86/boot/bzImage
LOG=./ubuntu-vm.log
PIDFILE=./ubuntu-vm.pid
NBD=${NBD:-/dev/nbd0}

MEM=${MEM:-8G}
SMP=${SMP:-4}

if [[ ! -f $BASE ]]; then
	echo "missing $BASE" >&2
	exit 1
fi
if [[ ! -f $KERNEL ]]; then
	echo "missing $KERNEL — build with ./build.sh" >&2
	exit 1
fi
if [[ ! -f $PUBKEY ]]; then
	echo "missing $PUBKEY" >&2
	exit 1
fi

make_cidata() {
	local tmp
	tmp=$(mktemp -d)
	cp "$IMGDIR/cidata/user-data" "$IMGDIR/cidata/meta-data" "$tmp/"
	cat >"$tmp/network-config" <<'EOF'
version: 2
ethernets:
  eth0:
    dhcp4: true
EOF
	rm -f "$CIDATA"
	dd if=/dev/zero of="$CIDATA" bs=1M count=2 status=none
	mkfs.vfat -n CIDATA "$CIDATA" >/dev/null
	mcopy -i "$CIDATA" "$tmp/user-data" "$tmp/meta-data" "$tmp/network-config" ::
	rm -rf "$tmp"
}

nbd_disconnect() {
	sudo qemu-nbd -d "$NBD" >/dev/null 2>&1 || true
	sleep 0.3
}

grow_and_inject() {
	sudo modprobe nbd max_part=16
	nbd_disconnect
	sudo qemu-nbd -c "$NBD" "$OVERLAY"
	sleep 0.5
	sudo partprobe "$NBD" >/dev/null 2>&1 || true
	# Relocate backup GPT after qemu-img resize, then grow p1 (last partition).
	sudo sgdisk -e "$NBD" >/dev/null
	sudo growpart "$NBD" 1
	sudo e2fsck -y -f "${NBD}p1" || [[ $? -le 1 ]]
	sudo resize2fs "${NBD}p1"

	local mnt tmppub
	mnt=$(mktemp -d)
	tmppub=$(mktemp)
	cp "$PUBKEY" "$tmppub"
	sudo mount "${NBD}p1" "$mnt"
	sudo mkdir -p "$mnt/root/.ssh" "$mnt/nswap" "$mnt/mnt"
	sudo cp "$tmppub" "$mnt/root/.ssh/authorized_keys"
	rm -f "$tmppub"
	sudo chmod 0700 "$mnt/root/.ssh"
	sudo chmod 0600 "$mnt/root/.ssh/authorized_keys"
	sudo chmod 1777 "$mnt/nswap"
	sudo chown -R 0:0 "$mnt/root/.ssh"
	# Host keys so sshd can start before cloud-init.
	sudo ssh-keygen -A -f "$mnt" >/dev/null
	sudo tee "$mnt/etc/netplan/99-nswap.yaml" >/dev/null <<'EOF'
network:
  version: 2
  ethernets:
    eth0:
      dhcp4: true
EOF
	if ! grep -q 'tests /mnt 9p' "$mnt/etc/fstab"; then
		echo 'tests /mnt 9p trans=virtio,version=9p2000.L,msize=262144,_netdev,nofail 0 0' \
			| sudo tee -a "$mnt/etc/fstab" >/dev/null
	fi
	if ! grep -q '/dev/vdc /nswap' "$mnt/etc/fstab"; then
		echo '/dev/vdc /nswap ext4 defaults,nofail 0 2' \
			| sudo tee -a "$mnt/etc/fstab" >/dev/null
	fi
	# Unused when booting -kernel; missing NLS/vfat options otherwise drop Ubuntu into emergency.
	sudo sed -i 's/\(LABEL=UEFI[[:space:]]\+\/boot\/efi[[:space:]]\+vfat[[:space:]]\+[^[:space:]]*\)/\1,nofail/' "$mnt/etc/fstab"
	sudo tee "$mnt/etc/nswap-ubuntu-prepared" >/dev/null <<<"ok"
	sudo umount "$mnt"
	rmdir "$mnt"
	nbd_disconnect
}

if [[ ! -f $CIDATA ]]; then
	make_cidata
fi

# Full systemd guests map every anonymous VMA as named-swap. Cursor's V8
# cages are 8–10GiB each; 16G /nswap hits ENOSPC immediately.
NSWAP_MB=${NSWAP_MB:-65536}
OVERLAY_SIZE=${OVERLAY_SIZE:-64G}
chmod_nswap_img() {
	local tmpmnt
	tmpmnt=$(mktemp -d)
	sudo mount -o loop "$NSWAPIMG" "$tmpmnt"
	sudo chmod 1777 "$tmpmnt"
	sudo umount "$tmpmnt"
	rmdir "$tmpmnt"
}
if [[ ! -f $NSWAPIMG ]]; then
	dd if=/dev/zero of="$NSWAPIMG" bs=1M count="$NSWAP_MB" status=none
	mkfs.ext4 -F -L nswap "$NSWAPIMG" >/dev/null
	chmod_nswap_img
else
	have=$(stat -c%s "$NSWAPIMG")
	want=$((NSWAP_MB * 1024 * 1024))
	if (( have < want )); then
		truncate -s "${NSWAP_MB}M" "$NSWAPIMG"
		e2fsck -fy "$NSWAPIMG" >/dev/null || true
		resize2fs "$NSWAPIMG" >/dev/null
		chmod_nswap_img
	fi
fi

grow_overlay_fs() {
	sudo modprobe nbd max_part=16
	nbd_disconnect
	sudo qemu-nbd -c "$NBD" "$OVERLAY"
	sleep 0.5
	sudo partprobe "$NBD" >/dev/null 2>&1 || true
	sudo sgdisk -e "$NBD" >/dev/null
	sudo growpart "$NBD" 1
	sudo e2fsck -y -f "${NBD}p1" || [[ $? -le 1 ]]
	sudo resize2fs "${NBD}p1"
	nbd_disconnect
}

if [[ ! -f $OVERLAY ]]; then
	qemu-img create -f qcow2 -F qcow2 -b "$BASE" "$OVERLAY"
	qemu-img resize "$OVERLAY" "$OVERLAY_SIZE"
	grow_and_inject
else
	cur=$(qemu-img info --output=json "$OVERLAY" | python3 -c 'import json,sys; print(json.load(sys.stdin)["virtual-size"])')
	want=$(numfmt --from=iec "$OVERLAY_SIZE")
	if (( cur < want )); then
		echo "growing overlay $cur -> $OVERLAY_SIZE"
		qemu-img resize "$OVERLAY" "$OVERLAY_SIZE"
		grow_overlay_fs
	fi
fi

if [[ -f $PIDFILE ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
	echo "already running pid $(cat "$PIDFILE")" >&2
	exit 1
fi

rm -f "$PIDFILE"
: > "$LOG"

QEMU=(
	qemu-system-x86_64
	-m "$MEM" -smp "$SMP"
	-cpu host
	-kernel "$KERNEL"
	-append "root=/dev/vda1 rootfstype=ext4 rw console=ttyS0,115200 earlyprintk=serial net.ifnames=0 nokaslr ${NSWAP_CMDLINE:-named_swap.root=/nswap named_swap.device=/dev/vdc}"
	-drive file="$OVERLAY",if=virtio,format=qcow2,discard=unmap
	-drive file="$CIDATA",if=virtio,format=raw,readonly=on
	-drive file="$NSWAPIMG",if=virtio,format=raw
	-virtfs local,path=/scratch/swap_tests,mount_tag=tests,security_model=none
	-virtfs local,path=/csl/daniel.br/.cursor-server,mount_tag=cursor,security_model=none
	-netdev user,id=net0,hostfwd=tcp:127.0.0.1:10022-:22,hostfwd=tcp:127.0.0.1:37111-:37111
	-device virtio-net-pci,netdev=net0
	-enable-kvm
	-display none
	-serial file:"$LOG"
	-pidfile "$PIDFILE"
)

if [[ "${FOREGROUND:-0}" == 1 ]]; then
	exec "${QEMU[@]}" -nographic -serial mon:stdio
fi

"${QEMU[@]}" -daemonize
echo "Ubuntu nswap VM started pid $(cat "$PIDFILE")"
echo "serial: $LOG"
echo "ssh:    ssh -i $HOME/.ssh/id_ed25519 -p 10022 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1"

#!/bin/sh
# Buildroot post-build for the disk rootfs (buildroot-disk.defconfig).
set -e
T=$1
# mount points for /sbin/overlay-init
mkdir -p "$T/overlay" "$T/rom"
cat > "$T/sbin/overlay-init" <<'S'
#!/bin/sh
# PID 1 on the read-only squashfs root: put a tmpfs overlay on top of it,
# move into the overlay and start BusyBox init there. Changes last until
# reboot; the squashfs is at /rom.
mount -t tmpfs -o mode=0755 tmpfs /overlay
mkdir /overlay/upper /overlay/work /overlay/root
mount -t overlay -o lowerdir=/,upperdir=/overlay/upper,workdir=/overlay/work overlay /overlay/root
mount --move /dev /overlay/root/dev
cd /overlay/root
pivot_root . rom
exec chroot . /sbin/init
S
chmod 755 "$T/sbin/overlay-init"
# network: DHCP on the virtio NIC (Wi-Fi on the board) without holding up boot
cat > "$T/etc/network/interfaces" <<'N'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
  udhcpc_opts -b -t 3 -T 2
N
# UTF-8 for tmux and friends (musl has C.UTF-8 built in)
grep -q LANG "$T/etc/profile" || echo 'export LANG=C.UTF-8' >> "$T/etc/profile"
# swap on the board's flash, when the firmware offers it (pico-shell.sh -w):
# the virtio disk whose serial is rvemu-flash-swap; its last page is the
# device's statistics, not swap
cat > "$T/etc/init.d/S02swap" <<'S'
#!/bin/sh
case "$1" in
start)
  for d in /sys/block/vd*; do
    [ "$(cat "$d/serial" 2>/dev/null)" = rvemu-flash-swap ] || continue
    mkswap "/dev/${d##*/}" $(( $(cat "$d/size") / 2 - 4 )) >/dev/null && swapon "/dev/${d##*/}"
  done
  ;;
stop)
  swapoff -a
  ;;
esac
S
chmod 755 "$T/etc/init.d/S02swap"
# the flash swap device's counters and flash timings (its last page)
cat > "$T/usr/bin/flashswap-stats" <<'S'
#!/bin/sh
for d in /sys/block/vd*; do
  [ "$(cat "$d/serial" 2>/dev/null)" = rvemu-flash-swap ] || continue
  dd if="/dev/${d##*/}" bs=4096 skip=$(( $(cat "$d/size") / 8 - 1 )) count=1 iflag=direct 2>/dev/null | tr -d '\0'
done
S
chmod 755 "$T/usr/bin/flashswap-stats"

#!/bin/sh
# Buildroot post-build for the disk rootfs (buildroot-disk.defconfig).
set -e
T=$1
# mount points for /sbin/overlay-init
mkdir -p "$T/overlay" "$T/rom"
cat > "$T/sbin/overlay-init" <<'S'
#!/bin/sh
# PID 1 on the read-only squashfs root: put a tmpfs overlay on top of it,
# move into the overlay and start BusyBox init there. Changes outside /home
# and /root last until reboot; the squashfs is at /rom.
mount -t tmpfs -o mode=0755 tmpfs /overlay
mkdir /overlay/upper /overlay/work /overlay/root
mount -t overlay -o lowerdir=/,upperdir=/overlay/upper,workdir=/overlay/work overlay /overlay/root
mount --move /dev /overlay/root/dev
cd /overlay/root
# /home: jffs2 on the board's flash region (the rvemu,flash MTD device),
# kept across reboots; /root lives in it as /home/root. jffs2 formats blank
# or foreign flash itself (it erases what it can't parse, in the background).
mkdir -p home
if mount -t jffs2 -o noatime mtd0 home; then
  [ -d home/root ] || { mkdir -m 700 home/root && cp -a /root/. home/root/; }
  mount --bind home/root root
else
  echo "overlay-init: no flash storage; /home is in RAM"
fi
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

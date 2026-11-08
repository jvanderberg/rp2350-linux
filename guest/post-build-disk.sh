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
# flash itself, and erases blocks it can't parse, but it won't mount flash
# that holds no jffs2 node at all (garbage): format that through the driver.
mkdir -p home
ok=0
if mount -t jffs2 -o noatime mtd0 home; then
  ok=1
else
  mount -t sysfs sysfs /sys
  F=/sys/bus/platform/devices/f8000400.flash/format
  if [ -e $F ]; then
    echo "overlay-init: no jffs2 filesystem in the /home flash; formatting it"
    echo 1 > $F && mount -t jffs2 -o noatime mtd0 home && ok=1
  fi
  umount /sys
fi
if [ $ok = 1 ]; then
  [ -d home/root ] || { mkdir -m 700 home/root && cp -a /root/. home/root/; }
  mount --bind home/root root
else
  echo "overlay-init: no flash storage; /home is in RAM"
fi
pivot_root . rom
exec chroot . /sbin/init
S
chmod 755 "$T/sbin/overlay-init"
# network: DHCP on the virtio NIC (Wi-Fi on the board) without holding up
# boot: udhcpc starts in the background at once (with `inet dhcp`, ifup
# waits for its first discover attempts, ~6 s of idle boot)
cat > "$T/etc/network/interfaces" <<'N'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet manual
  up ip link set eth0 up
  up udhcpc -b -R -i eth0 -p /var/run/udhcpc.eth0.pid </dev/null >/dev/null 2>&1 &
  down kill $(cat /var/run/udhcpc.eth0.pid) 2>/dev/null; ip link set eth0 down
N
# boot: every init script is a fork+exec, so only the needed ones stay. No
# syslogd/klogd (dmesg has the kernel log), no crond, no seedrng (the device
# tree's rng-seed seeds the CRNG), no modules.
rm -f "$T/etc/init.d/S01seedrng" "$T/etc/init.d/S01syslogd" "$T/etc/init.d/S02klogd" \
  "$T/etc/init.d/S11modules" "$T/etc/init.d/S50crond"
# inittab: the four `ln -sf /proc/self/fd... /dev/...` sysinit lines become
# one `cp -a` of the same symlinks, made here (three fewer fork+execs at boot);
# every other line stays where it is
mkdir -p "$T/lib/devlinks"
ln -sfn /proc/self/fd "$T/lib/devlinks/fd"
for i in 0:stdin 1:stdout 2:stderr; do ln -sfn "/proc/self/fd/${i%%:*}" "$T/lib/devlinks/${i#*:}"; done
if [ "$(grep -c '^null::sysinit:/bin/ln -sf /proc/self/fd' "$T/etc/inittab")" = 4 ]; then
  awk '/^null::sysinit:\/bin\/ln -sf \/proc\/self\/fd/ { if (!d) print "null::sysinit:/bin/cp -a /lib/devlinks/fd /lib/devlinks/stdin /lib/devlinks/stdout /lib/devlinks/stderr /dev/"; d = 1; next } { print }' \
    "$T/etc/inittab" > "$T/etc/inittab.new" && mv "$T/etc/inittab.new" "$T/etc/inittab"
fi
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

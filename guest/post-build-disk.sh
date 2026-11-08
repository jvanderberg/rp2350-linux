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
# boot: every init script is a fork+exec of dynamic BusyBox (~1 M guest
# instructions each). No syslogd/klogd (dmesg has the kernel log), no crond,
# no seedrng (the device tree's rng-seed seeds the CRNG), no modules.
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

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
  # a deeper atomic reserve than the default's (~360 KiB for 7.5 MiB):
  # virtio-net refills its receive ring with GFP_ATOMIC, and under swap
  # pressure kswapd otherwise lets free memory fall to where those fail
  echo 1024 > /proc/sys/vm/min_free_kbytes
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
# the SD card: the firmware's SD device (the virtio disk whose serial is
# rvemu-sd), its first FAT partition at /mnt/sd and any swap partition on
# it, in the background
cat > "$T/etc/init.d/S03sd" <<'S'
#!/bin/sh
# The SD card. The firmware looks for a card about once a second, and its
# disk's size is 0 while there is none. When a card goes in, its partitions
# are read, every partition with a swap signature (mkswap) becomes swap at
# priority 10 (ahead of the flash swap), and the first partition, or the
# whole card if it has no partition table, is mounted as FAT at /mnt/sd.
# When it comes out, both go. A card unmounted by hand stays unmounted until
# it comes out.
PID=/var/run/sd.pid
sd_disk() {
  for d in /sys/block/vd*; do
    [ "$(cat "$d/serial" 2>/dev/null)" = rvemu-sd ] && { echo "${d##*/}"; return; }
  done
}
is_swap() {
  [ "$(dd if="$1" bs=1 skip=4086 count=10 2>/dev/null)" = SWAPSPACE2 ]
}
mounted() {
  while read -r _ m _; do [ "$m" = /mnt/sd ] && return 0; done < /proc/mounts
  return 1
}
card_in() {
  partprobe "/dev/$1" 2>/dev/null
  for p in /sys/block/"$1"/"$1"*; do
    [ -e "$p" ] || continue
    is_swap "/dev/${p##*/}" && swapon -p 10 "/dev/${p##*/}"
  done
  p=/dev/${1}1
  [ -b "$p" ] || p=/dev/$1
  is_swap "$p" || mount -t vfat -o noatime "$p" /mnt/sd
}
card_out() {
  while read -r f _; do
    case $f in /dev/"$1"*) swapoff "$f";; esac
  done < /proc/swaps
  mounted && umount -l /mnt/sd
  partprobe "/dev/$1" 2>/dev/null
}
follow() {
  d=$(sd_disk)
  [ -n "$d" ] || exit 0
  in=0
  while :; do
    read -r size < "/sys/block/$d/size"
    if [ "$size" != 0 ] && [ $in = 0 ]; then
      in=1
      card_in "$d"
    elif [ "$size" = 0 ] && [ $in = 1 ]; then
      in=0
      card_out "$d"
    fi
    sleep 2
  done
}
case "$1" in
start)
  [ -f $PID ] && kill -0 "$(cat $PID)" 2>/dev/null && exit 0
  mkdir -p /mnt/sd
  follow </dev/null >/dev/null 2>&1 &
  echo $! > $PID
  ;;
stop)
  [ -f $PID ] && kill "$(cat $PID)" 2>/dev/null
  rm -f $PID
  d=$(sd_disk)
  [ -n "$d" ] && card_out "$d"
  ;;
restart)
  "$0" stop
  "$0" start
  ;;
esac
S
chmod 755 "$T/etc/init.d/S03sd"
# Wi-Fi keepalive: an ARP request to the gateway every 20 s. On a working
# link the reply (unicast) comes back; the firmware rejoins the network when
# the guest sends and nothing unicast comes back for 30 s (wifi.rs,
# TRAFFIC_STALL_S), which catches a link the radio reports up but that
# passes nothing. ARP, not ping: every IPv4 gateway answers it.
cat > "$T/etc/init.d/S41wifi-keepalive" <<'S'
#!/bin/sh
PID=/var/run/wifi-keepalive.pid
gateway() {
  while read -r i d g _; do
    [ "$i" = eth0 ] && [ "$d" = 00000000 ] || continue
    g=$((0x$g))
    echo "$((g & 255)).$((g >> 8 & 255)).$((g >> 16 & 255)).$((g >> 24 & 255))"
    return
  done < /proc/net/route
}
follow() {
  while :; do
    sleep 20
    gw=$(gateway)
    [ -n "$gw" ] && arping -q -f -c 1 -w 2 -I eth0 "$gw"
  done
}
case "$1" in
start)
  [ -f $PID ] && kill -0 "$(cat $PID)" 2>/dev/null && exit 0
  follow </dev/null >/dev/null 2>&1 &
  echo $! > $PID
  ;;
stop)
  [ -f $PID ] && kill "$(cat $PID)" 2>/dev/null
  rm -f $PID
  ;;
restart)
  "$0" stop
  "$0" start
  ;;
esac
S
chmod 755 "$T/etc/init.d/S41wifi-keepalive"
# the flash swap device's counters and flash timings (its last page)
cat > "$T/usr/bin/flashswap-stats" <<'S'
#!/bin/sh
for d in /sys/block/vd*; do
  [ "$(cat "$d/serial" 2>/dev/null)" = rvemu-flash-swap ] || continue
  dd if="/dev/${d##*/}" bs=4096 skip=$(( $(cat "$d/size") / 8 - 1 )) count=1 iflag=direct 2>/dev/null | tr -d '\0'
done
S
chmod 755 "$T/usr/bin/flashswap-stats"
# SSH (Dropbear, S50dropbear): host keys in /home/dropbear, so they last
# across reboots; dropbear -R makes them at the first connection
ln -sfn /home/dropbear "$T/etc/dropbear"
mkdir -p "$T/etc/default"
cat > "$T/etc/default/dropbear" <<'D'
# read by /etc/init.d/S50dropbear
mkdir -p -m 700 /home/dropbear
D
# claude-remote: run Claude Code on another host against files on this guest.
# The MCP server is a no_std Rust binary built on the host by run-build.sh.
install -m 755 "$(dirname "$0")/claude-remote/claude-remote" "$T/usr/bin/claude-remote"
install -m 755 "$(dirname "$0")/claude-remote/target/riscv32im-unknown-none-elf/release/claude-guest-mcp" \
  "$T/usr/bin/claude-guest-mcp"

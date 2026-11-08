# shellcheck shell=bash
# Shared by pico-linux.sh / pico-shell.sh (source it from rvemu-rs/): get the
# RP2350 into USB BOOTSEL and load the guest blob + firmware with picotool.

P=${PROBE_RS:-$HOME/git/toolchains/probe-rs-master/bin/probe-rs}
# One board, several checkouts: the board lock (../rp2350/tools/board-lock)
# and the record of what is in its flash live outside every checkout.
LOCK_HOME=${RP2350_LOCK_HOME:-$HOME/.cache/rp2350-board}
BOARD_LOCK=../rp2350/tools/board-lock
mkdir -p "$LOCK_HOME/stamps"
BLOB_STAMP=$LOCK_HOME/stamps/blob.sha256
DISK_STAMP=$LOCK_HOME/stamps/disk.sha256

# need_board: stop unless this caller holds the board lock (BOARD_LOCK_OWNER)
need_board() { "$BOARD_LOCK" check "${BOARD_LOCK_OWNER:-nobody}" || exit 1; }

in_bootsel() { picotool info >/dev/null 2>&1; }
wait_bootsel() { for _ in $(seq 1 "$1"); do in_bootsel && return 0; sleep 1; done; return 1; }

# rvemu-pico-shell's console port (USB serial number "rvemu")
shell_port() { ls /dev/cu.usbmodemrvemu* 2>/dev/null | head -1; }

request_bootsel() {
  # rvemu-pico-shell: the 1200-baud touch on its console port
  local port; port=$(shell_port)
  if [ -n "$port" ] && lsof "$port" >/dev/null 2>&1; then
    # the firmware acts on the touch only once the port is closed
    echo "pico: $port is open in another program ($(lsof -t "$port" | head -1 | xargs ps -o comm= -p)); close it (screen: Ctrl-A k) and run again"
    exit 1
  fi
  if [ -n "$port" ]; then
    stty -f "$port" 1200 2>/dev/null || true
    wait_bootsel 5 && return 0
  fi
  # any rp2350-board firmware: watchdog SCRATCH0 = BOOTSEL_MAGIC, then reset
  "$P" write --chip RP235x_riscv --protocol swd b32 0x400d800c 0xb0075e1c >/dev/null 2>&1 || true
  "$P" reset --chip RP235x_riscv --protocol swd >/dev/null 2>&1 || true
  wait_bootsel 5
}

# ensure_bootsel ELF: failing a request, flash ELF over SWD once (slow) and ask again
ensure_bootsel() {
  need_board
  in_bootsel && return
  request_bootsel && return
  echo "pico: no BOOTSEL device; flashing $1 over SWD once"
  "$P" download --chip RP235x_riscv --protocol swd --speed 10000 "$1"
  request_bootsel || { echo "pico: target never appeared in BOOTSEL (USB data connection?)"; exit 1; }
}

# Flash map (16 MB): firmware, /home (jffs2, written only by the firmware) at
# 0x10100000, swap at 0x10200000, Wi-Fi settings in the 4 KB at 0x103FF000,
# guest blob at 0x10400000 (2 MB), root filesystem at 0x10600000, kernel text
# in the last bytes before 0x11000000 (guest_blob.rs).
WIFI_ADDR=0x103ff000
OLD_WIFI_ADDR=0x10ff0000   # before the kernel text moved to the end of flash

# load_at FILE ADDR STAMP WHAT: write FILE to flash at ADDR unless STAMP says it is there
load_at() {
  need_board
  local sum; sum="$(shasum -a 256 "$1" | cut -d' ' -f1) @$2"
  if [ "${FORCE_BLOB:-0}" = 1 ] || [ "$(cat "$3" 2>/dev/null || true)" != "$sum" ]; then
    echo "pico: loading $4 ($(wc -c < "$1" | tr -d ' ') bytes) over USB"
    rm -f "$3"
    picotool load -v -t bin "$1" -o "$2"
    echo "$sum" > "$3"
  fi
}

# load_blob BLOB: the guest blob (kernel after its text, initramfs, DTB) at 0x10400000
load_blob() { load_at "$1" 0x10400000 "$BLOB_STAMP" "guest blob"; }

# load_disk IMAGE: the root filesystem (squashfs) at 0x10600000
load_disk() { load_at "$1" 0x10600000 "$DISK_STAMP" "root filesystem"; }

# load_text TEXT: the kernel text, ending at 0x11000000
load_text() {
  local n; n=$(wc -c < "$1" | tr -d ' ')
  load_at "$1" "$(printf '0x%x' $((0x11000000 - n)))" "$TEXT_STAMP" "kernel text"
}
TEXT_STAMP=$LOCK_HOME/stamps/text.sha256

# keep_wifi: once, copy Wi-Fi settings stored at the old address to the new
# one (picotool wants both range bounds in hex; a failed read stops the load)
keep_wifi() {
  local t; t=$(mktemp -d)
  picotool save -r $WIFI_ADDR "$(printf 0x%x $((WIFI_ADDR + 4096)))" "$t/new.bin" -t bin >/dev/null
  if [ "$(head -c 4 "$t/new.bin" 2>/dev/null)" != WIFI ]; then
    picotool save -r $OLD_WIFI_ADDR "$(printf 0x%x $((OLD_WIFI_ADDR + 4096)))" "$t/old.bin" -t bin >/dev/null
    if [ "$(head -c 4 "$t/old.bin" 2>/dev/null)" = WIFI ]; then
      echo "pico: moving the stored Wi-Fi network to $WIFI_ADDR"
      picotool load -t bin "$t/old.bin" -o $WIFI_ADDR >/dev/null
    fi
  fi
  rm -rf "$t"
}

# text_len IMAGE: bytes of the Image that are pure text (from its System.map):
# _etext - _start, rounded down to a page (that page also holds .init)
text_len() {
  local m; m="$(dirname "$1")/System.map"
  python3 -c "import sys
s=e=0
for l in open(sys.argv[1]):
    a,_,n=l.split()[:3]
    if n=='_start': s=int(a,16)
    if n=='_etext': e=int(a,16)
print((e-s)//4096*4096)" "$m"
}

# pack_guest IMAGE INITRD RAM_MB BLOB_OUT TEXT_OUT: device tree as the Mac host
# makes it, then the blob and the kernel text. INITRD "-" means none: root on
# the disk (pass its image as DISK=). NET=1 adds the network device node,
# NOR=1 the flash device node (/home in the storage region 0x10100000).
# IO=1 the pin devices (GPIO, I2C, SPI, PWM, ADC).
pack_guest() {
  local dts dtb initrd=$2 src tl; dts=$(mktemp); dtb=$(mktemp)
  tl=$(text_len "$1")
  if [ "$initrd" = - ]; then src=(-d "$DISK"); initrd=/dev/null; else src=(-i "$initrd"); fi
  # NET=1: the firmware has the Wi-Fi virtio-net device
  [ "${NET:-0}" = 1 ] && src+=(-N)
  # NOR=1: the firmware has the /home flash device (-D never opens the file)
  [ "${NOR:-0}" = 1 ] && src+=(-F /dev/null)
  # SWAP=1: the swap device node (rvemu-pico-shell's flash swap, /dev/vdb)
  [ "${SWAP:-0}" = 1 ] && src+=(-W)
  # IO=1: the firmware drives the board's pins (rvemu-pico/src/pins.rs)
  [ "${IO:-0}" = 1 ] && src+=(-G)
  ./target/release/rvemu -D -m "$3" -X "$tl" "${src[@]}" > "$dts"
  dtc -q -I dts -O dtb -o "$dtb" "$dts"
  python3 ../rp2350/tools/pack-guest.py "$1" "$initrd" "$dtb" "$3" "$tl" "$4" "$5"
  rm -f "$dts" "$dtb"
}

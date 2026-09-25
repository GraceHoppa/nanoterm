#!/data/data/com.termux/files/usr/bin/bash
# One-shot Termux setup: everything needed to compile and upload Arduino
# sketches from the phone. Safe to re-run; finished steps are skipped.
#
#   1. Termux packages (compiler, libusb, termux-api, proot-distro)
#   2. a Debian container with arduino-cli and the AVR board core
#   3. nanoterm, nt, and the arduino-cli wrapper into $PREFIX/bin
#
# Run from a clone of the repo:  ./termux/setup.sh
set -euo pipefail

DISTRO="${NANOTERM_DISTRO:-debian}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

say()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33m[!] %s\033[0m\n' "$*" >&2; }
in_distro() { proot-distro login "$DISTRO" -- bash -c "$1"; }

[ -n "${PREFIX:-}" ] && [ -d "$PREFIX" ] || { echo "run this inside Termux" >&2; exit 1; }

say "Termux packages"
pkg install -y git clang make pkg-config libusb termux-api proot-distro

# termux-usb talks to the separate Termux:API *app*; without it every call
# just hangs. termux-usb -l itself would hang, so probe with a timeout.
if ! timeout 15 termux-usb -l >/dev/null 2>&1; then
    warn "termux-usb isn't answering: install the Termux:API app from the same"
    warn "place you got Termux (F-Droid), open it once, then re-run this."
fi

say "Debian container ($DISTRO)"
if proot-distro login "$DISTRO" -- true 2>/dev/null; then
    echo "already installed"
else
    proot-distro install "$DISTRO"
fi

say "arduino-cli inside the container"
# Single quotes on purpose: this runs in the container, $(...) expands there.
# shellcheck disable=SC2016
in_distro '
    set -e
    if [ -x /root/bin/arduino-cli ]; then
        echo "already installed: $(/root/bin/arduino-cli version)"
    else
        apt-get update -qq
        apt-get install -y -qq curl ca-certificates >/dev/null
        mkdir -p /root/bin
        curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/root/bin sh
    fi
    if /root/bin/arduino-cli core list | grep -q "^arduino:avr "; then
        echo "arduino:avr core already installed"
    else
        /root/bin/arduino-cli core update-index
        /root/bin/arduino-cli core install arduino:avr
    fi
'

say "nanoterm, nt, arduino-cli wrapper"
make -C "$ROOT" install-termux

say "done"
cat <<'EOF'

Plug the board in (USB-C cable or OTG adapter), then from a sketch folder:

  arduino-cli compile -b arduino:avr:nano:cpu=atmega328 --output-dir build .
  nt -u build/<sketch>.ino.hex -m     # upload, then watch Serial output

EOF

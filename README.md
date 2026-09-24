# nanoterm

A tiny serial terminal for CH340-based Arduino clones (Nano, Uno, …) that
talks to the USB chip directly through libusb. That makes it work on an
Android phone in **Termux, without root** — no kernel driver needed — as
well as on Linux.

- 9600 baud, 8N1
- `-t` timestamps every received line
- `-l FILE` logs everything received and sent (sent lines marked `>`)
- Type a line + Enter to send it (a `\n` is appended); Ctrl+C quits

## Android (Termux)

Install **Termux** and **Termux:API** — both from F-Droid (mixing sources
breaks `termux-usb`). Then:

```sh
pkg install git clang make pkg-config libusb termux-api
git clone <this repo> && cd nanoterm
make install-termux
```

Plug the board in (USB-C to USB-C or an OTG adapter), then:

```sh
nt          # finds the board, asks for USB permission, connects
nt -t -l session.log
```

On GrapheneOS, new USB devices are blocked while the phone is locked —
unlock before plugging in, and check *Settings → USB-C port* if nothing
shows up.

## Linux

```sh
make && sudo make install
sudo cp udev/99-ch340.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
nanoterm -t
```

The udev rule gives the `uucp` group (Arch; use `dialout` on Debian/Ubuntu)
raw access to the CH340. nanoterm detaches the `ch341` kernel driver while
it runs, so `/dev/ttyUSB0` disappears until it exits — quit it before
uploading a new sketch.

## Notes

- `termux-usb -e` captures the program's stdout until it exits, so
  nanoterm writes everything live to stderr.
- `termux-usb -e` accepts only a single command word, so `nt` passes
  options to nanoterm through `NANOTERM_TIMESTAMPS=1` / `NANOTERM_LOG=FILE`.
- Only 9600 baud for now.

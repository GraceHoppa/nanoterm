Test fixtures, built from the sketches in `examples/`:

- `*.ino.hex`, `*.ino.with_bootloader.hex`: `arduino-cli compile -b arduino:avr:nano:cpu=atmega328 --output-dir build`
- `*.ino.bin`: the same program extracted straight from the ELF, independent
  of the .hex: `avr-objcopy -O binary -R .eeprom build/X.ino.elf X.ino.bin`

`test_ihex` checks that parsing each .hex reproduces its .bin byte for byte.

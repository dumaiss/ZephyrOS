# ZTRCheck

ZTRCheck is the KonaOS ZTR validator. libsnd's `snd_ztr_open` trusts the
content CRC a producer stamps into the header (docs/ZTR-Format.md `16h`);
the full structural validation lives here. `ztrcheck` runs every check of
the format over a `kos_reader_t` on the open file — header fields, section
layout, the CP/M record tail, the content CRC when the field is nonzero,
the string pool, the instrument directory, the macro blocks, every pattern
stream, and the note divisors — and prints either one OK summary or the
first defect with its check and file offset.

## Build

From this directory:

```sh
make
```

The Makefile extracts and links against
`ZephyrOS/build/kos-sdk-0.2.0.tar.gz`. The executable is
`build/user/ztrcheck.exe`; `build/host/ztrcheck` is the host twin the test
suites drive without the simulator. `make test` runs the host-twin checks
and validates the packed executable.

## Run

```text
ztrcheck night-market.ztr
```

A valid song prints its title, author, source, rate, speed, rows, orders,
patterns, instruments, macro bytes, logical size and CRC state
(`crc=none`, or `crc=ok XXXX` for a stamped file). A defective file prints
`FAIL <check> at <offset>: <detail>`; a stamped file whose covered bytes
changed prints `FAIL crc at 0016: MISMATCH stored=XXXX computed=YYYY`.
Exit status is 0 for a valid file, 4 for a defective one, 2 when the file
cannot be read.

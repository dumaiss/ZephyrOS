# KonaOS

KonaOS is the native operating system of the Zephyr-80, a Z80 machine with
banked SRAM, a V9958 video card, an I/O controller for USB keyboards and an
SD card. It is a trusted, single-tasking system: a small nucleus in one
bank, resident drivers in a second, and programs in the rest, with one
namespace (`/fs`, `/system`, `/dev`), typed handles, extended memory and
far code for programs that need more than 56 KiB.

## Where to start

| you want to | read |
|---|---|
| **write a program** | [`docs/sdk.md`](docs/sdk.md): the SDK, the build rule, the API, far code and data, the standalone SDK. Then [`docs/abi.md`](docs/abi.md) and [`docs/filesystem.md`](docs/filesystem.md) |
| **write or change a driver** | [`docs/devices.md`](docs/devices.md): the drivers and their protocols. Then [`docs/module-format.md`](docs/module-format.md) for the image and the loader |
| **work on the OS itself** | [`docs/architecture.md`](docs/architecture.md): the contract. Then [`docs/memory.md`](docs/memory.md), [`docs/boot.md`](docs/boot.md), [`docs/vfs.md`](docs/vfs.md), [`docs/exe-format.md`](docs/exe-format.md), [`docs/romfs-format.md`](docs/romfs-format.md) |
| **use the machine** | [`docs/monitor.md`](docs/monitor.md): the monitor and the production exerciser |

## Requirements

- SDCC 4.5 (`sdcc`, `sdasz80`, `sdldz80`) with the Z80 target
- GNU Make
- Python 3 (standard library only)

## Building

```sh
make                  # the production image: build/konaos.bin (and build/zephyr80.bin)
make check            # clean rebuild, reproducibility, layout checks, host tests,
                      # the production replay in the simulator
make sdk              # the standalone SDK: build/kos-sdk-<version>/ and its tarball
make SYSTEM=<name>    # any image described by systems/<name>.json
make clean            # remove build*/
```

Acceptance images, each with its simulator replay:

| target | image |
|---|---|
| `make subsystem-selftest` | the syscall gate, interrupts, the ROMFS reader, processes and the monitor |
| `make nucleus-selftest` | the fixed core and the upcall shim |
| `make ipc-selftest` | the IPC substrate: copy paths, nesting, driver IRQs |
| `make ns-selftest` | the namespace, the standard handles and publications |
| `make fs-selftest` | files on `/fs` and `/system`, nested exec, with and without a card |
| `make xmem-selftest` | extended memory, far code and banked calls |
| `make console-serial-selftest` | the serial-console build, and the missing-driver boot failure |
| `make helloworld-image` | the SDK demo image |

Focused host checks: `make mod-test`, `make romfs-test`, `make exe-test`,
`make monitor-test`, `make ioc-test`, `make hid-test`, `make v9958-test`.

## Running on a board

Program `build/konaos.bin` (512 KiB) into the ROM, connect a 115,200 8N1
terminal to SIO channel B, and reset: the banner, the drivers and the
monitor prompt appear. `X /system/hwtest.exe` at the prompt runs the
production exerciser.

To check a board automatically, build the image, flash it, start the tool
and press RESET:

```sh
make smoke-hardware SERIAL_PORT=/dev/ttyUSB0          # production boot
make smoke-hardware-hwtest SERIAL_PORT=/dev/ttyUSB0   # the exerciser
make hardware-images                                  # every acceptance image
```

Each `smoke-hardware-*` target prints the image and SHA-256 it expects
(`docs/boot.md` section 8).

### Measuring the read path

`make phase-image` builds a measurement image (`KOS_IOC_PHASES=1`) whose
IOC driver records the clock at every step of a `/fs` block read, plus
the card program that fetches and reports it.  The measurement is
side-effect free and no shipped target defines the option:

```sh
make phase-image                     # build-phase/konaos.bin + build/user/readphase.exe
# flash build-phase/konaos.bin, copy readphase.exe to the card as READPH.EXE
readph BIG.BIN 256                   # a report per pass: min/mean/max per phase
```

The report's rows are the phases of one read (the transport, the cache,
the staging copy, the caller copy) in 25.6 us counts, with the timer
driver's own reading of the same read as a cross-check; `docs/validation/`
has the replay that fixes the semantics.

## Tree

```text
boot/         reset and the ROM-to-RAM copy
kernel/       the nucleus
drivers/      resident drivers, and the driver kit (drivers/include/)
formats/      the on-media formats: ROMFS, EXE, MOD
abi/kos/      the public native ABI
platform/     the machine description (constants only)
systems/      one description per image
config/       residency and SDK configuration
linker/       the link script
mk/kos.mk     the program build rule
user/         the SDK, the monitor, the demo
tools/        host packers, validators, the smoke tool
tests/        host tests, simulator replays, target test code
docs/         the documentation above
```

## License

See [`LICENSE`](LICENSE).

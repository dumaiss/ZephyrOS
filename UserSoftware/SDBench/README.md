# SDBench for KonaOS

This is a C port of the read-only parts of
`Code/HOST/Utilities/src/ioc_sdbench.asm`. It uses the KonaOS 0.1.0
application SDK supplied by ZephyrOS. Both normal FILE-API benchmarking and
the original controller `/SHARED` benchmark are supported.

The program reads one file in six forward passes, using logical request sizes
of 128, 256, 512, 1024, 2048 and 4096 bytes. For every size it reports:

- bytes, elapsed I/O time and throughput;
- minimum, mean and maximum request latency;
- the original SDBench latency histogram buckets; and
- a checksum consistency check across all six passes.

Timing uses the `/dev/timer` SDK interface (25.6 us counts). SDBench calibrates
the time between two adjacent `timer_now()` samples and subtracts that cost
from each request. Buffer checksumming occurs after the timed interval.

With `/S`, the program binds KonaOS's published `/dev/ioc` object and uses the
controller's read-only `/SHARED` file commands. It performs three passes using
genuine 128, 256 and 512-byte IOC bulk transactions. `/P` brackets each `/S`
pass with the controller's physical-SD profile page and reports physical read
calls and ticks, cache misses, retries, reinits, and the physical/measured time
ratio. `/P` requires the diagnostic/profile-enabled controller firmware, as it
does in the CP/M original.

## Build

From this directory:

```sh
make
```

The first build unpacks
`../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz` beneath the ignored local
`build/` directory. The resulting application is:

```text
build/user/sdbench.exe
```

`KOS_SDK_ARCHIVE` and `KOS_SDK_ROOT` can be overridden for a different SDK
location or an already unpacked SDK.

## Run

Copy `sdbench.exe` onto a KonaOS filesystem and give it a non-empty file of at
most 4 MiB. A large file is preferable because it reduces cache bias.

```text
sdbench /fs/BENCH.BIN
sdbench -n /fs/BENCH.BIN
sdbench BENCH.BIN /s
sdbench BENCH.BIN /s /p
```

KonaOS passes command-tail words starting at `argv[0]`; the port parses its
arguments accordingly. `-n` (also `/N` or `--no-timer`) performs the reads and
checksum verification without collecting timing data.

In normal mode each timed sample is one public SDK `read()` call. The `/fs`
provider can split larger requests into 512-byte controller operations
internally, so the 1024-4096-byte rows are explicitly logical application
requests rather than single SD-card transactions.

`/S` takes a bare FAT 8.3 filename, not a KonaOS path. The file is opened below
the controller's `/SHARED` directory. Option letters are case-insensitive;
`-s`, `--shared`, `-p`, and `--profile` are also accepted.

## Differences from IOC_SDBENCH.COM

The CP/M BDOS-record mode and destructive raw-card diagnostic remain absent.
The KonaOS port is read-only and never asks for confirmation because it never
writes either the target file or raw media. `/S` and `/P` use KonaOS object IPC
to `/dev/ioc`; they do not access IOC hardware ports directly.

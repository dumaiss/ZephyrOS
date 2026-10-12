# `user/helloworld/` — demos and examples

This directory holds the KonaOS *demo* programs: small, complete SDK
programs that are written against `user/sdk/` alone and are **not** part of
the production `/system` set (architecture 32.14 keeps the shipped system
image minimal).

Every demo is one `exe_program` line in [Makefile](./Makefile):

```make
$(eval $(call exe_program,hello,user/helloworld,hello.c,STACK=256,STAGE=1))
$(eval $(call exe_program,fardemo,user/helloworld,fardemo.c,\
	FAR=fardemo_far.c,STACK=256,STAGE=1))
```

* `hello.c` — prints `Hello, KonaOS!`, then one line per command-tail
  argument, and exits 0.  It reads no filesystem and calls no diagnostic
  syscall, so it is also the smallest possible proof that the SDK toolchain
  produces a loadable EXE.
* `fardemo.c` + `fardemo_far.c` — the far-code demo (work package 44b).
  `fardemo_far.c` is compiled with `--codeseg FAR_CODE` through `FAR=` and
  linked at `8000h`, so it becomes the program's `FAR_CODE` segment;
  `fardemo.c` maps it with `segmap`, calls it through `farcall`
  and prints the token, the far line, the returned latch/value and `PASS`.
  The far half prints from a literal inside the aperture, calls back into
  primary code and issues its own console syscall, so the demo covers the
  whole far-call path.

## Building

```sh
make build/user/hello.exe     # pack the demo in the production tree
make build/user/fardemo.exe   # pack the far-code demo
make helloworld-image         # separate build dir, boots it end to end
```

The manifest [manifest.json](./manifest.json) is the production recovery
manifest plus `hello.exe` and `fardemo.exe`, and `make helloworld-image`
builds in `build-helloworld/` with `KOS_BOOT_PROGRAM=/system/hello.exe`.
Booting the demo therefore needs **no kernel change**: the program is
stored in the recovery store, the launcher starts it like any other EXE,
and the console shows its output followed by the launcher's exit report.

Booting `fardemo.exe` instead is one override (the `helloworld-image`
recipe hardcodes `/system/hello.exe`):

```sh
make BUILD=build-fardemo KOS_HELLOWORLD_IMAGE=1 \
	KOS_BOOT_PROGRAM=/system/fardemo.exe \
	build-fardemo/konaos-helloworld.bin
```

`tests/test_sdk.py` boots that image in the simulator and checks the
transcript, so a broken demo fails the normal `make check` run.
`fardemo.exe` is built and packer-validated by the same image target (it
is a normal `exe_program` line); the far-code path itself - map, plain
call, aperture buffer, exec and refusal - is replayed end to end by
`tests/test_xmem_selftest_sim.py`, which boots the xmem-selftest image
with its far-code program.

## Writing a new demo

Copy `user/sdk/template/hello.c`, add it to this directory, add a
`exe_program` line, and (only if you want to boot it) add its `.exe` to
this manifest.  See [docs/sdk.md](../../docs/sdk.md) for the SDK layout,
`libkos`, `klibc` and the `exe_program` options.

# `user/helloworld/` — the bootable SDK demo image

This directory owns the **demo image**, not the programs: `make
helloworld-image` builds `build-helloworld/konaos-helloworld.bin` from
`systems/helloworld.json`, whose `boot` is `/system/hello.exe` and whose
store adds `hello.exe` and `fardemo.exe` to the production file set.
Nothing differs from production: no kernel edit, no new syscall, no new
module.

The programs are the SDK's own examples (`user/sdk/examples/`), so the
image boots exactly what the standalone SDK ships; a broken example fails
this image too.  Each is one `exe_program` line here:

```make
$(eval $(call exe_program,hello,user/sdk/examples,hello.c,STACK=256,STAGE=1))
$(eval $(call exe_program,fardemo,user/sdk/examples,fardemo.c,\
	FAR1=fardemo_far.c,STACK=256,STAGE=1))
```

`STAGE=1` copies the packed program into `$(MODULES)` so the selected
recovery manifest can name it.

* `hello.c` — prints `Hello, KonaOS!`, then one line per command-tail
  argument, and exits 0.  It reads no filesystem and calls no diagnostic
  syscall, so it is also the smallest possible proof that the SDK
  toolchain produces a loadable EXE.
* `fardemo.c` + `fardemo_far.c` — the far-code demo (work packages
  44b/49b).  `fardemo_far.c` is compiled into the `_FAR1` area and linked
  at `8000h` through `FAR1=`, so it is segment 1; the far entry is
  `__banked`, so the primary half calls it as plain C and the SDK's
  trampoline maps the segment and restores the previous latch.  The demo
  prints the token, the far line, the returned latch/value and `PASS`.

## Building

```sh
make build/user/hello.exe     # pack the demo in the production tree
make build/user/fardemo.exe   # pack the far-code demo
make helloworld-image         # separate build dir, boots hello.exe
```

Booting the demo needs **no kernel change**: the program is stored in the
recovery store, the launcher starts it like any other EXE, and the console
shows its output followed by the launcher's exit report.  Because the
launcher re-execs on every clean exit, the transcript is the repeating
pair

```text
Hello, KonaOS!
monitor: exit status 0x0000 active 0 free-banks 6
```

`tests/test_sdk.py` boots that image in the simulator and checks the
transcript, so a broken demo fails the normal `make check` run.
`fardemo.exe` is built and packer-validated by the same image target; to
boot it instead, give the image a system file whose `boot` names
`/system/fardemo.exe` (see `docs/sdk.md` section 10).

## Writing a new demo

Copy `user/sdk/template/` next to a Makefile, or add a `.c` file to
`user/sdk/examples/` with an `exe_program` line in its `Makefile` (so the
packaged SDK builds it) and, if the demo image should boot or store it,
one here plus its `.exe` in `systems/helloworld.json`.  See
[docs/sdk.md](../../docs/sdk.md) for the SDK layout, `libkos`, `klibc`
and the `exe_program` options.

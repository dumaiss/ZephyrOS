# `user/helloworld/` — demos and examples

This directory holds the ZephyrOS *demo* programs: small, complete SDK
programs that are written against `user/sdk/` alone and are **not** part of
the production `/SYSTEM` set (architecture 32.14 keeps the shipped system
image minimal).

Every demo is one `zex_program` line in [Makefile](./Makefile):

```make
$(eval $(call zex_program,hello,user/helloworld,hello.c,STACK=256,STAGE=1))
```

* `hello.c` — prints `Hello, ZephyrOS!`, then one line per command-tail
  argument, and exits 0.  It reads no filesystem and calls no diagnostic
  syscall, so it is also the smallest possible proof that the SDK toolchain
  produces a loadable ZEX.

## Building

```sh
make build/user/hello.zex     # pack the demo in the production tree
make helloworld-image         # separate build dir, boots it end to end
```

The manifest [manifest.json](./manifest.json) is the production recovery
manifest plus `hello.zex`, and `make helloworld-image` builds in
`build-helloworld/` with `ZOS_BOOT_PROGRAM=/SYSTEM/hello.zex`.  Booting the
demo therefore needs **no kernel change**: the program is stored in the
recovery store, the launcher starts it like any other ZEX, and the console
shows its output followed by the launcher's exit report.

`tests/test_sdk.py` boots that image in the simulator and checks the
transcript, so a broken demo fails the normal `make check` run.

## Writing a new demo

Copy `user/sdk/template/hello.c`, add it to this directory, add a
`zex_program` line, and (only if you want to boot it) add its `.zex` to
this manifest.  See [docs/sdk.md](../../docs/sdk.md) for the SDK layout,
`libzos`, `zlibc` and the `zex_program` options.

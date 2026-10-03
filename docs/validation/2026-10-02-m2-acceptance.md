# M2 Interrupt Executive Acceptance — 2026-10-02

## Host validation

The M2 implementation builds a normal ROM and a distinct
`build-irq-selftest/zephyros-irq-selftest.bin` acceptance ROM. Host checks
cover the fixed IM2 page/guard, both IRQ stack reservations, linked kernel
handler boundary, invalid layout fixtures, and replay of the real common
entry/return bytes from mode `10` (banks 0 and 6), mode `11`, and vector
`FFh`. The replay preserves main and alternate registers, PC, SP, exact
port-00h latch, bank identity, and enabled interrupt state.

Observed host results:

```text
make clean                         PASS
make                               PASS
make test                          PASS  87 host tests
make check                         PASS  reproducible rebuild + validation
make syscall-selftest              PASS
syscall linked-byte simulation     PASS  banks 0/6 + enabled-IFF restore
make irq-selftest                  PASS
final full unittest discovery      PASS  87 tests (incl. full IRQ replay)
```

The full replay executes the real `zos_run_irq_selftest` C driver with one
injected CTC0 interrupt per EI window: registry semantics, three deliveries
per bank (one through a HALT window with bit-exact `AF` verification), the
syscall-gate IFF restore probe, and the mode-11 unclaimed path, ending in
`result = ZOS_IST_OK`.  The packer additionally proves
`word_at(FDFFh) == address_of(irq_ff_unexpected)` in every image.

Binary hashes (post-validation rebuild):

| Artifact | SHA-256 |
| --- | --- |
| `build/zephyros.bin` (normal) | `67991e1614bfecad7a11873eaecdf694673d57073a039570f6085e5d63bff72f` |
| `build-selftest/zephyros-selftest.bin` | `e2320243a05aae125a0d4b914306f171f8cbab58ac1f6b82702de9c97942b7f2` |
| `build-irq-selftest/zephyros-irq-selftest.bin` | `699c204852b6d6cbb3d63be1e0b550ae3807727d4ca4b71dc260c9f22b4df1b6` |

The normal image is exactly 131072 bytes and `build/zephyr80.bin` is
byte-identical.  The kernel prints the banner after a ~1 s serial-connect
delay, then runs the syscall and IRQ self-tests.

## Physical Zephyr-80 validation

**PASS — observed on physical Zephyr-80 hardware, 2026-10-02.**

The `build-irq-selftest/zephyros-irq-selftest.bin` image (sha256
`699c204852b6d6cbb3d63be1e0b550ae3807727d4ca4b71dc260c9f22b4df1b6`) was
programmed into the board and captured through SIO0/B at 115200 8N1:

```sh
python3 tools/serial_smoke.py --mode irq --port /dev/ttyUSB0
```

```text
serial_smoke: listening on /dev/ttyUSB0 at 115200 8N1 (mode=irq, timeout=15s)
serial_smoke: captured 111 bytes
serial_smoke: transcript follows
ZephyrOS 0.0
platform: Zephyr-80
kernel entry: OK
serial: OK
HALT
syscall selftest: OK
irq selftest: OK
serial_smoke: OK (the banner plus 'irq selftest: OK')
```

The transcript proves on physical hardware: `I = FDh` and IM2 are active;
CTC0 at port `40h` (vector `00h`, control `A7h`, constant `00h`) repeatedly
dispatches the registered bank-7 handler from mode `11` and from mode `10`
over banks 0 and 6; the interrupted mode, bank, SP, PC, and full main and
alternate register state (including bit-exact `AF` through the HALT
window) are restored; the syscall gate preserves enabled-interrupt state;
the unclaimed source takes the quiesce path; CTC0 tears down cleanly with
`03h`; and the early console is unaffected before and after the test.

All M2 acceptance criteria in `docs/architecture.md` section 19 are met;
**M2 is accepted** (host checks plus physical-hardware observation).

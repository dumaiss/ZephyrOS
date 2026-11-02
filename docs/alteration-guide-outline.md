# KonaOS Alteration Guide

**Working outline · OS developers · draft**

The Programmer’s Guide is for a program that runs on KonaOS. This guide is for
the person changing KonaOS: the nucleus, a resident driver, a loader, a build
tool or a system image. It starts with the boundaries an alteration must keep,
then follows the path from source change to evidence on a board or emulator.

The guide should use the Programmer’s Guide’s voice: direct instructions,
short explanations beside the rule they explain, and concrete failure cases.
When a limit or contract matters, state its value and where it is enforced.
This is an outline; the architecture documents remain the authoritative
reference for exact layouts, structures and algorithms.

## Part I · Before you change the system

Orient the reader to the system, the source tree and the contracts a change can
cross. Read this part before moving code or changing an interface.

### 1. What you are changing

- KonaOS as ROM-loaded system code, a fixed nucleus, bank-6 resident drivers,
  ROMFS content and native programs.
- The single-tasking process model and the distinction between kernel,
  interrupt executive, driver and application responsibilities.
- Where each subsystem lives in the source tree; ownership boundaries and the
  toolchain used to build it.
- Which document defines which contract: architecture, boot, memory, ABI,
  devices, VFS, filesystem and the binary formats.

### 2. Start with the boundary

- Trace a request from a program through the syscall gate, namespace, endpoint
  and driver, then back through the reply path.
- Identify whether the proposed change affects source compatibility, the
  native ABI, on-disk or in-ROM formats, the boot manifest, or only an
  implementation detail.
- Keep application policy in applications, reusable clients in the SDK, device
  policy in drivers and machine-wide policy in the nucleus.
- Treat reserved fields, error results, ownership and cleanup as part of the
  contract.

### 3. The machine KonaOS has

- Decoder revision 13, the banking latch, operating modes and the roles of
  banks 0–6.
- The 0000h–DFFFh banked region, E000h–FFFFh common memory, ROM mapping and the
  far aperture.
- The CTC and port map; the early serial console and devices that survive a
  warm reboot.
- The memory map and linker areas: what can move, what must stay common, and
  which changes need new ledger evidence.

## Part II · The path from reset to a running system

Follow the system in execution order. Each chapter should point to the exact
source and build artifact a developer can inspect.

### 4. Reset and boot

- ROM reset, the ROM-to-RAM copy and the mapping at kernel entry.
- C runtime setup, early serial output and the boot sequence.
- The boot launcher, image selection and failure fallback.
- What a warm reboot resets in software, and what each driver must reinitialize
  because hardware was not reset.
- Build outputs and the board and emulator checks for a boot change.

### 5. The nucleus

- Fixed core and cold half; responsibilities the nucleus owns.
- Page zero, the RST 30h syscall gate and its saved state.
- The interrupt executive: IM2 setup, vector table, stacks, transition
  reservations, handler restrictions and return path.
- ROM access and the ROMFS reader while bank mappings change.
- Process entry, exit, synchronous child execution and fault handling.
- Error paths: validate before switching state, release partial allocations,
  and return with the caller’s mapping and interrupt state restored.

### 6. Memory, banking and shared state

- Primary banks, driver bank 6, common memory and extended memory.
- Fixed-core routines and the memory ledgers; how code placement affects
  budgets and call reachability.
- Banking primitives and the one-window rule for the far aperture.
- Syscall and IRQ transition state: which bytes and stacks are reserved, and
  why they cannot be casually reused.
- Adding code or data: update the right linker areas and ledgers, then inspect
  the map and generated reports.

## Part III · Interfaces and components

Change a public contract only with its callers, providers, format readers and
tooling in view.

### 7. The native ABI

- The syscall entry and return convention, register preservation and interrupt
  behavior.
- Argument-block and data-buffer ranges; validation before dereference.
- Adding or changing a syscall: number allocation, argument layout, errors,
  implementation, ABI declarations, SDK wrapper and documentation.
- Objects, handles, protocols and the two result channels.
- Process, event, extended-memory and diagnostic interfaces.
- Compatibility rules: keep old numbers and layouts stable; use a versioned
  structure or explicit ABI revision when a contract must change.

### 8. Resident drivers

- A driver is a packed MOD image with endpoints, dependencies, placement and
  publications.
- The bank-6 tiers, the driver image and the bootstrap API available during
  initialization.
- Declaring dependencies and composing endpoints without hidden order
  assumptions.
- Protocol frames, descriptors, buffer-copy helpers, reentrancy and bounded
  work.
- Driver initialization, failure unwind, unload ownership and publication in
  `/dev`.
- Port ownership and exclusive takeovers: restore device state on release and
  initialize fully after warm reboot.
- Interrupt handlers: acknowledge promptly, use only the restricted API and
  defer work that belongs in process context.

### 9. Namespace, filesystems and ROMFS

- The root router, provider roots, canonical paths, CWD and fixed VFS workspace.
- Handle ownership, provider callbacks and cleanup on close, process exit and
  failed operations.
- FILE and filesystem-namespace protocols; metadata and filesystem limits.
- IOC-FAT as one provider, not a VFS assumption; how another provider fits.
- ROMFS physical placement, directory entries, manifest and reader integration.
- Format changes: update packers, validators, readers and fixtures together;
  retain a versioned path for existing images.

### 10. Executables and driver images

- KEX2 geometry, segment tables, link contract and load sequence.
- MOD container layout, dependency graph, endpoint records and driver loading.
- Host packers as the first line of validation; loader checks remain necessary.
- Changing a format: document byte layout and bounds, define rejection behavior,
  update producer and consumer, and add compatibility evidence.
- Keep generated files generated; identify the source manifest and reproducible
  command for each image.

### 11. Build the system you intend to ship

- One system file per image; the boot manifest selects its contents.
- Host validation, packer reports, memory and placement ledgers.
- Production and acceptance images: what differs and when each is appropriate.
- SDK and user program builds as consumers of the ABI, not alternate owners of
  kernel behavior.
- Make targets and the output to inspect when a build fails.

## Part IV · Prove the alteration

Tests are evidence for a specific contract. The guide should teach developers
to choose the narrowest useful check and then verify the whole path affected.

### 12. Test at the right layer

- Host tests for parsers, packers, format bounds, generated manifests and
  placement rules.
- Unit or simulation tests for nucleus routines, protocols and driver state
  machines.
- Integration tests for boot, syscall, VFS, process and device paths.
- Emulator acceptance images for behavior that needs a complete system.
- Board checks for real timing, ports, interrupts and hardware state.
- For each test: state what it proves, what it cannot prove and how to run it.

### 13. Follow a failure

- Read the first failing check; reproduce with the smallest relevant target.
- Use the map, ledger, packed-image report, boot serial output and monitor
  diagnostics to narrow the fault.
- Separate build rejection, boot failure, syscall failure, protocol refusal and
  device failure.
- Check saved latch, SP, registers, interrupt state and cleanup at every
  crossing.
- Capture enough evidence to make the failure repeatable; do not infer success
  from a build that merely completed.

### 14. Review and land a system change

- Review the contract and every producer, consumer and tool touched by it.
- Check reserved fields, bounds, error codes, ownership, unwind and reboot
  behavior.
- Compare memory and placement ledgers before and after the change.
- Record the tests and hardware checks that passed, plus any limits they leave
  unverified.
- Update the architecture reference when implementation facts change; update
  this guide when the developer workflow or boundary rules change.

## Appendices · Reference at hand

- Source tree map by subsystem.
- Build, pack, test, emulator and board command quick reference.
- Memory map and bank-role diagram.
- Crossings at a glance: syscall, interrupt, IPC, ROM read and driver call.
- Error and failure ownership table.
- Change checklist: ABI, format, boot manifest, memory budget, tests and docs.
- Glossary of KonaOS terms and abbreviations.

## Source map for the eventual chapters

| Guide chapter | Primary reference |
|---|---|
| 1–3 | `docs/architecture.md` |
| 4 | `docs/boot.md`, `docs/architecture.md` |
| 5–6 | `docs/architecture.md`, `docs/memory.md`, `docs/abi.md` |
| 7 | `docs/abi.md`, `docs/sdk.md` |
| 8 | `docs/devices.md`, `docs/module-format.md` |
| 9 | `docs/vfs.md`, `docs/filesystem.md`, `docs/romfs-format.md` |
| 10 | `docs/exe-format.md`, `docs/module-format.md`, `docs/romfs-format.md` |
| 11–14 | Build targets, test suites, `docs/boot.md` and the subsystem references |

The Programmer’s Guide remains the application guide. The Alteration Guide
should explain where an OS change belongs and how to prove it; exact syscall,
protocol, structure and file-format definitions stay in their reference
documents.

# tests/reader - the shared read-at reader (package 99)

- `readertest.c` - the card program: one fixture through all four
  adapters (handle, stdio, KRES, memory), matching checksums and
  random probes including past the end, the sources still alive after
  the readers are dropped, and a framed per-call timing report.
- `readermap.c` - the link proof: the memory adapter alone, so the map
  carries nothing of the other adapters.
- `host/host_reader.c` - the host test of the memory adapter and
  `kos_reader_read_full` against a fake reader (short chunks, a
  transient error, an early end).
- `fixtures/card.dat` - the 1,024-byte card fixture (xorshift pattern,
  sha256 pinned by tests/test_reader.py).
- `fixtures/reader.kpr` - a sealed KRES container holding `card.dat`
  byte for byte, built with `make kres-host` and the host kres tool
  (`kres create/import/seal`); tests/test_reader.py re-extracts and
  compares it.

tests/test_reader.py runs readertest from the shell and checks the
prints, the FS2 open count (one open per source, none by an adapter)
and both link maps.

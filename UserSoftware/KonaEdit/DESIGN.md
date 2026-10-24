# KonaEdit design

## Document store

All document positions, lengths, line numbers and visual columns use
`ke_pos_t` (`uint32_t`).  The store owns eight 8-KiB public XMEM objects.  A
64-entry order table maps logical 1-KiB pages to their physical object/page
slot, while a 64-entry length table records occupancy.  One 1-KiB near-memory
cache holds the current page and is written back only when dirty.  Insertions
split full pages and propagate their suffix into the next logical page;
deletions remove empty pages and keep the order table compact.  No line index
or line buffer imposes a line-length limit.

Text walks read the store a page span at a time (`ke_store_span`: one page
lookup, then a byte loop inside the cached page), never byte by byte.

## View

The view (`ke_view.c`) is the visible document lines in RAM: each body row's
line start (a position past the end marks a blank row), the line after the
bottom row, and the top row's line number.  The cursor's line is the top line
plus its row, so nothing counts lines except a rebuild after a jump or an
action; Page Up/Down move the view by exactly the lines moved.  A row is built
from page spans with byte-sized columns (a horizontal scroll first walks to
the left column) and written with one `tui_put_text` per attribute run; a
selection, being contiguous, puts at most one selected span on a row.  TAB
advances to the next multiple of eight.  Positions stay 32-bit; screen
coordinates are bytes.

## Clipboard and history

The clipboard uses two public 16-KiB mappable XMEM allocations and accepts up
to 32 KiB.  It is independent of document page order.  History uses two
32-KiB objects as one 64-KiB arena.  Each of the three record descriptors
contains a 32-bit position, removed length, inserted length and arena offset;
the arena retains the old then new byte sequences needed for undo and redo.
When a fourth record or arena pressure requires space, the oldest complete
record is dropped and remaining payload is compacted.  Starting a new edit
after undo discards redo records.

Consecutive single-byte typing is accumulated in a 20-byte near-memory stage.
The stage is committed to the history arena only when full or before another
history operation, so normal typing performs roughly one history XMEM write
per twenty characters while remaining one undo record.  Backspace and Delete
retain their direction-aware grouped records.

The startup allocation is twelve XMEM objects. The support store is created
first: `ke_history_init` allocates two 16-KiB clipboard objects followed by two
32-KiB history objects. The eight 8-KiB document objects are allocated after
them. This order is intentional: both four-slot history runs are secured before
the document's independent allocations can fragment the free space. It also
gives transactional Open a deterministic allocation/free sequence in which the
support store can be rebuilt after the old document is released. No near-memory
document fallback exists.

## Load and save state machines

Load streams 512-byte filesystem reads through a CR normalizer into a fresh
XMEM store. A CP/M `1Ah` byte is logical EOF; later record padding and debris
are not examined. NUL before that marker, overflow, I/O or XMEM failure
disposes the fresh store and leaves the live document unchanged. Because the
allocator is synchronous,
Open temporarily releases history/clipboard, constructs the fresh document
beside the old one, commits the store swap only after validation, then rebuilds
the support stores in the released banks.

Save creates an exclusive `KETMPn.$$$` sibling, streams all document bytes,
syncs and closes it.  If the target exists it is renamed to a unique recovery
name; the temporary is then renamed to the target.  Failure to install rolls
the recovery name back.  A temporary or recovery file that cannot be removed
is reported to the user.

## Primary-memory budget

The packed executable report is the authoritative budget. The current image
contains 45,993 bytes plus 9,275 bytes of BSS, ends at `D8E4h`, and declares a
1,024-byte stack at `DC00h-E000h`, leaving 796 bytes between the linked image
and stack. Large application-owned areas are the shared 1,296-byte modal save
buffer, 1,024-byte document page cache, the shared 512-byte load/save transfer
buffer, and the TUI library's logical screen.
The XMEM payload budget is 64 KiB document + 32 KiB clipboard + 64 KiB history.

## TUI update policy and exposed gaps

Caret-only motion changes the status row (only when its line, column or flags
change) and the cursor.  An edit inside a line shifts the row starts below it
and redraws that row; only changed cells reach the console.  A newline or a
join moves the rows on one side of the cursor by `tui_scroll_rows` and draws
the two rows involved: the console restores every row outside the moved
region with a VDP copy, so the side that leaves fewer rows outside is moved
(near the bottom the rows above move and the top line steps by one).  A
cursor one line past either edge scrolls the whole body region and draws the
exposed row; anything further rebuilds the view.  A moving selection redraws
the rows between the cursor's old and new rows; clearing one redraws all rows.
Popup selection rewrites the same glyphs with changed semantic styles so the
TUI emits INVERT records, not TEXT glyphs. File-list selection similarly
repaints two rows, filename entry repaints only its input field, and container
suffixes use positioned output.  Overlay close uses `tui_restore` for its
rectangle.  A console without semantic scroll gets the affected rows redrawn.
KonaEdit also has to provide popup-menu policy,
compound dialogs and a paged directory view itself because the widget layer
has no popup/form runner or virtual list provider.  Those are SDK findings,
not private-API workarounds.

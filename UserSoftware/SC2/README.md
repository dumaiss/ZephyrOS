# SC2Kos for KonaOS

SC2Kos is a native C spreadsheet for KonaOS, based on the supplied
SORCIM SuperCalc2 CP/M 2.2 binaries (`sc2.com` and `sc2.ovl`). It preserves
the original application's text-grid workflow, sparse A1-style worksheet,
immediate recalculation and slash-command vocabulary while using only the
public KonaOS 0.1.0 application SDK.

The interactive renderer is incremental: ordinary cursor movement updates
the old and new cells plus the changing status fields instead of repainting
the terminal. Full-screen draws are batched into a few large console writes.

## Build

From this directory:

```sh
make
```

The first build unpacks
`../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz` into the ignored local
`build/` directory. The packed application is:

```text
build/user/sc2kos.exe
```

The port has no dependency on the CP/M binaries at run time.

Run `make demo` to regenerate and validate the worked 12-month planning model
at `examples/planning_model.ksc`. It contains 644 populated cells with linked
operating, cash-flow, loan, growth, break-even and discounted-cash-flow
calculations.

## Run

Copy `sc2kos.exe` to a KonaOS filesystem and start it from the shell:

```text
sc2kos
sc2kos /fs/BUDGET.KSC
```

Arrow keys move the active cell. Typing replaces a cell, Enter edits its
current contents, `?` opens help, and `/` opens the command menu. Prefix text
that looks numeric with an apostrophe to force a label.

Examples:

```text
125.50
=A1*1.13
=SUM(B1:B12)
=AVERAGE(C2:C20)
```

Arithmetic is signed fixed-point with three internal decimal places. Formulae
support relative and `$`-absolute cell references, comparisons (`=`, `<>`,
`<`, `<=`, `>`, `>=`), parentheses, and `+`, `-`, `*`, `/`. The function set
includes:

- Ranges: `SUM`, `AVERAGE`, `MIN`, `MAX`, `COUNT`
- Numeric: `ABS`, `INT`, `SIGN`, `SQRT`, `ROUND`, `MOD`, `POWER`
- Logical: `IF`, `AND`, `OR`, `NOT`
- Dates: `DATE`, `YEAR`, `MONTH`, `DAY`
- Financial: `PMT`, `FV`

Dates are Gregorian serial values from 1900 through 2099. Circular references,
invalid dates, invalid arguments, and division by zero display `#ERROR`.

Implemented slash commands are Blank, Copy, Delete, Edit, Format, Go-to,
Insert, Load, Move, Output-to-CSV, Protect, Quit, range Replicate, Save,
Unprotect, and Zap. Copy and Replicate adjust relative references while
preserving `$`-absolute rows and columns. The worksheet is sparse, spans A1
through BK254, and holds up to 768 populated cells with 63 input characters
per cell and a shared 12 KiB expression/label pool.

## File formats and compatibility

`/S` and `/L` use the compact native `KSC1` format, which preserves formulas,
format precision, and protection flags. `/O` exports calculated values as CSV.

The supplied CP/M program's machine-specific terminal configuration, direct
BDOS calls, overlay loader, printer setup, execute scripts, split windows,
and proprietary `.CAL` serialization are not copied into the native port. In
particular, SC2Kos does not claim binary compatibility with old `.CAL` files.
This keeps the application within the documented KonaOS SDK and avoids
shipping or embedding the copyrighted CP/M program or overlay.

## Verification

`make test` runs the host-side model and persistence suite. It covers formula
evaluation, logical/date/financial functions, circular-reference errors,
relative and absolute-reference replication, protected edits, CSV output,
and a save/load round trip at the 768-cell capacity. `make` also validates the
packed KonaOS executable with the SDK packer.

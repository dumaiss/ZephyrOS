# PadStat

PadStat shows the IOC's USB/HID enumeration state. It is the KonaOS
counterpart of the CP/M `PADSTAT.COM`, and adds the controller and
keyboard fields from HID_STATUS page 0.

It reads HID_STATUS page 0 (controller, keyboard) and page 6 (devices,
last HID interface, the two F310 slots) through `/dev/ioc`. Both pages are
passive: the controller answers from state its USB callbacks keep, runs
no USB transaction and acknowledges nothing. Both are present in the
normal firmware build.

## Build

From this directory:

```sh
make
```

The Makefile extracts and links against
`ZephyrOS/build/kos-sdk-0.2.0.tar.gz`. The executable is
`build/user/padstat.exe`.

## Run

```text
padstat
padstat /w
```

The plain form prints one report:

- controller state and MAX3421E revision;
- configured devices, mount and unmount counts;
- the last device's address and VID:PID;
- HID mounts, and the last HID interface's address and boot protocol
  (`keyboard`, `mouse`, `none`, or `never` before any HID mount);
- the keyboard's address, link speed (`full` or `low`) and report count;
- each F310 slot: address, instance, first-arm result, report count, last
  report length and Coleco latch byte;
- a verdict line.

`/w` watches. It polls both pages every 250 ms and prints one line
whenever a shown field changes. Report counts alone don't trigger a line,
so plugging, unplugging and pad presses leave a trail but held keys don't.
Any key stops it.

```text
dev 01 m 02 u 00 last 02 046d:c216 hid 02 none kbd 01 full p0 02/0041 p1 00/0000
```

Fields, in order: devices, mounts, unmounts, last device address and
VID:PID, last HID address and protocol, keyboard address and speed, then
each pad's address and report count.

`/p n` dumps HID_STATUS page `n` (0-6) as raw bytes and names the
fields of page 1 (host controller registers and the enumeration
milestones: attach, device descriptor, configuration descriptor,
enumeration state and failures, last HRSL) and page 4 (the behind-hub
enumeration entry and endpoint-table use) and page 5 (the HID driver's
set-config chain: SET_IDLE, SET_PROTOCOL, report descriptor, mount).
Pages 1-5 exist only in the
diagnostic firmware (`make IOC_PROFILE=diagnostic`); the normal build
answers them with page 0, and PadStat says so.

Exit status is 0 on success, 1 for a usage error, and 2 when `/dev/ioc`,
`/dev/timer` or the controller command fails.

## Low-speed devices

Every USB port sits behind the board's hub, so a low-speed device (most
mice, many keyboards) needs the MAX3421E's PRE-packet mode. A mouse that
enumerates shows `protocol 02 (mouse)` and the verdict
`a boot mouse enumerated ... through the hub`. A low-speed keyboard shows
`low speed` with a rising report count once it types.

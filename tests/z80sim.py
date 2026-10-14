#!/usr/bin/env python3
"""Minimal Z80 simulator to replay the KonaOS selftest flows.

Models the banking latch (MEM_DECODER rev 12):
  mode 00: 0000-FFFF reads from the selected ROM page; writes target the
           selected SRAM bank (reset copier and ROM-access stub);
  mode 08: flat view of the selected SRAM bank over the whole 64 KiB;
           no ROM and no forced bank (MEM_MODE_FLAT);
  mode 10: 0000-DFFF = bankN, E000+ = bank0 (application);
  mode 11: 0000-1FFF = bankN, 2000-DFFF = bank7, E000+ = bank0 (OS).

Boot pages 0/1 come from the provided images; the recovery pages 2-7
are supplied separately (rom_pages) when a replay exercises ROMFS boot.

Usage: sim.py <selftest-image> [start-pc] [trace]
"""

import sys
import collections

SZ = 0x10000
FILL = 0xFF


class IocCommandPeer:
    """Register-level model of the deployed MCU IOC command-lane peer.

    Models exactly what the KonaOS IOC driver can observe through
    SIO1/B (data port 32h, control port 33h):

      - the WR0 command / register-pointer protocol, including channel
        reset, Reset External/Status, error reset and EOM reset;
      - RR0 status: RX-available, TX-empty, /DCDB COMMAND_READY and the
        /CTSB HID doorbell;
      - /RTSB as the held "unacknowledged request" level (WR5 bit 1),
        with preload-vs-underrun detection on the capture window;
      - request parsing and CRC-16-CCITT replies for LINK_SYNC, PING and
        the HID input fetch (the deployed MCU also emits one trailing
        FFh pipeline byte);
      - the HID input queue: /CTSB is held while queued bytes exist,
        CMD_HID_INPUT dequeues up to the wanted count and answers
        `queued`/`dropped` plus data, and one-shot malformed replies can
        be injected;
      - fault injection: arbitrary leading (full-duplex) bytes, delayed
        readiness, delayed replies, stalled clocks, CRC/length/type/
        sequence faults, and a held HID doorbell.

    Observability for the host tests: channel_resets, rts_asserts/
    rts_releases, hunt_writes, frame_start/preload_present, drain reads,
    parsed requests, delivered replies.
    """

    CMD_LINK_SYNC = 0x0C
    CMD_PING = 0x01
    CMD_XFER_STATUS = 0x06
    CMD_FS_OPEN = 0x22
    CMD_FS_READ = 0x23
    CMD_FS_WRITE = 0x24
    CMD_FS_CLOSE = 0x25
    CMD_FS_DELETE = 0x27
    CMD_HID_INPUT = 0x0E
    RSP_FS_OPEN = 0xA2
    RSP_FS_READ = 0xA3
    RSP_FS_WRITE = 0xA4
    RSP_FS_CLOSE = 0xA5
    RSP_FS_DELETE = 0xA7
    RSP_HID_INPUT = 0x8E
    RSP_XFER_STATUS = 0x86
    DIR_MCU_TO_Z80 = 0
    DIR_Z80_TO_MCU = 1
    ST_OK = 0x00
    ST_BULK_FAIL = 0x20
    ST_BULK_NO_SYNC = 0x22
    ST_FS_NO_FILE = 0x34
    ST_FS_BAD_NAME = 0x35
    ST_FS_NO_HANDLE = 0x36
    ST_FS_RANGE = 0x37
    ST_FS_ERROR = 0x38
    ST_UNKNOWN_CMD = 0x02   # IOC_STATUS_UNKNOWN_CMD (unknown-class reply)

    # --- FS2 filesystem driver ----------------------
    CMD_FS2_CAPS = 0x30
    CMD_FS2_GENERATION = 0x31
    CMD_FS2_RESET = 0x32
    CMD_FS2_ROOT = 0x33
    CMD_FS2_PUSH = 0x34
    CMD_FS2_OPEN_RO = 0x35
    CMD_FS2_READ = 0x36
    CMD_FS2_CLOSE = 0x37
    CMD_FS2_OPENDIR = 0x38
    CMD_FS2_READDIR = 0x39
    CMD_FS2_CLOSEDIR = 0x3A
    CMD_FS2_STAT = 0x3B
    CMD_FS2_OPEN_RW = 0x3D
    CMD_FS2_WRITE = 0x3E
    CMD_FS2_SYNC = 0x3F
    CMD_FS2_TRUNCATE = 0x40
    CMD_FS2_UNLINK = 0x41
    CMD_FS2_RENAME = 0x42
    CMD_FS2_MKDIR = 0x43
    CMD_FS2_RMDIR = 0x44
    RSP_FS2_READ = 0xB6
    RSP_FS2_RESET = 0xB2
    RSP_FS2_WRITE = 0xBE
    ST_FS2_OK = 0x00
    ST_FS2_NOT_FOUND = 0x40
    ST_FS2_END = 0x41
    ST_FS2_EXISTS = 0x42
    ST_FS2_BAD_NAME = 0x43
    ST_FS2_READ_ONLY = 0x44
    ST_FS2_NO_SPACE = 0x45
    ST_FS2_NOT_DIR = 0x46
    ST_FS2_IS_DIR = 0x47
    ST_FS2_NO_HANDLE = 0x48
    ST_FS2_STALE = 0x49
    ST_FS2_RANGE = 0x4A
    ST_FS2_NO_MEDIA = 0x4B
    ST_FS2_UNKNOWN_WRITE = 0x4D
    ST_FS2_IO = 0x4E
    FS2_ATTR_DIR = 0x10

    def __init__(self, leading=(0xFF,), reply_delay=8, ready_delay=0,
                 never_ready=False, stall=False, fault=None,
                 link_faults=None, doorbell=False, level=0x4A,
                 echo_len=16, ping_len=26, reply_tail=b"\xff",
                 fs_files=None, read_fault=None, write_fault=None,
                 dcd_delay=2, cts_delay=2, commit_delay=1,
                 pipeline_residue=2, read_trailer=b"\x00\x00",
                 stall_bytes=None, fs_open_fault=None,
                 fs_delete_reopen_fault=None,
                 arbitrary_ready=None, custom_replies=None,
                 hid_bytes=b"", hid_faults=None,
                 fs2_media=True, fs2_read_only=False, fs2_files=None,
                 fs2_caps_fault=None, fs2_read_fault=None,
                 fs2_write_fault=None, fs2_generation=None,
                 fs2_stale_path=None):
        self.ptr = 0
        self.wr = {}
        self.rx = collections.deque()
        self.tx_window = bytearray()
        self.capture = bytearray()
        self.rts = False
        self.tx_enabled = False
        self.doorbell = doorbell
        self.level = level
        self.echo_len = echo_len
        self.ping_len = ping_len
        self.reply_tail = reply_tail
        self.leading = tuple(leading)
        self.reply_delay = reply_delay
        self.reply_countdown = 0
        self.reply_pending = None
        self.reply_queued = False
        self.stall = stall
        self.fault = fault
        self.link_faults = list(link_faults) if link_faults else []
        self.ready_delay = ready_delay
        self.never_ready = never_ready
        # --- SIO1/A bulk-lane model ---------------------------------
        self.a_ptr = 0
        self.a_wr = {}
        self.a_rx = collections.deque()  # bytes available to host reads
        self.a_tx_window = bytearray()   # host -> MCU capture
        self.a_rts = False
        self.a_tx_enabled = False
        self.dcd = False                 # /DCDA: read admission level
        self.cts = False                 # /CTSA: phase-active level
        self.tx_underrun = False         # RR0 D6 for the write path
        self.admit_countdown = 0
        self.cts_off_countdown = 0
        self.armed_send = None   # (id, rsp_type, seq, payload)
        self.armed_recv = None   # (id, cmd_type, seq, name, offset, len)
        self.a_delivered = False
        self.read_fault = read_fault
        self.write_fault = write_fault
        self.done_fault = None
        self.dcd_delay = dcd_delay
        self.cts_delay = cts_delay
        self.commit_delay = commit_delay
        self.pipeline_residue = pipeline_residue
        self.read_trailer = bytes(read_trailer)
        self.stall_bytes = stall_bytes
        self.a_channel_resets = 0
        self.a_hunt_writes = 0
        self.a_rts_asserts = 0
        self.a_rts_releases = 0
        self.a_wr3_writes = []
        self.bulk_requests = []  # parsed write captures
        self.bulk_sends = []     # wire bytes pushed to the host
        self.a_drain_reads = 0
        self.a_data_reads = 0
        self.last_xfer_id = 0
        self.last_status = 0
        self.last_bulk_capture = b""
        self.xfer_id = 0
        # --- /SHARED file model -------------------------------------
        self.fs_files = {k: bytearray(v)
                         for k, v in (fs_files or {}).items()}
        self.fs_open_name = None
        self.fs_handle = 0
        self.fs_next_handle = 1
        self.fs_writing = False
        # --- READY-admission test shaping ---------------------------
        # fs_open_fault: None | "unknown" (RSP_UNKNOWN_COMMAND plus the
        # unknown-command status) | "unknown_status" (unknown class with a
        # filesystem status) | status int | ("class", reply_class).
        # arbitrary_ready: {request type: (direction, length[, status])}
        # arms a READY for any bulk-arming class under test.
        # custom_replies: {request type: (reply class, payload)}.
        self.fs_open_fault = fs_open_fault
        self.fs_delete_reopen_fault = fs_delete_reopen_fault
        self.arbitrary_ready = arbitrary_ready
        self.custom_replies = dict(custom_replies or {})
        # --- FS2 filesystem driver model ------------
        # A small root-relative FAT-like namespace served with the real
        # FS2 command/reply and READY/DONE bulk semantics.  `fs2_counts`
        # records every FS2 command class so tests can prove the
        # provider's cache rules from the wire traffic.
        self.fs2_media = fs2_media
        self.fs2_read_only = fs2_read_only
        self.fs2_caps_fault = fs2_caps_fault
        self.fs2_read_fault = fs2_read_fault
        self.fs2_write_fault = fs2_write_fault
        self.fs2_generation = 1 if fs2_generation is None else fs2_generation
        # A path (tuple or '/'-string) whose reads/writes report STALE, to
        # exercise the provider's staleness mapping without a real media
        # change.
        self.fs2_stale_path = ((tuple(p.upper() for p in fs2_stale_path
                                      .split("/") if p))
                               if isinstance(fs2_stale_path, str)
                               else fs2_stale_path)
        self.fs2_counts = collections.Counter()
        self.fs2_resolver = []
        self.fs2_dirs = {()}
        self.fs2_files = {}
        # Two file slots and one directory slot (CAPS v1).
        self.fs2_slots = [None, None]   # [path, writable, cookie]
        self.fs2_cookies = [1, 1]
        self.fs2_dir = None             # [path, cookie] or None
        self.fs2_dir_cookie = 1
        for name, content in (fs2_files or {}).items():
            self.fs2_add(name, content)
        # --- HID input queue model -----------------------------------
        # The deployed firmware translates USB boot-keyboard reports into
        # terminal bytes, holds them in its own queue and asserts /CTSB
        # while it is non-empty.  CMD_HID_INPUT dequeues what it returns,
        # so the reply carries `queued`/`dropped` then up to the wanted
        # count of bytes.  hid_faults injects malformed replies one at a
        # time ("unknown", "wrong_type", "status", "short",
        # "over_count").
        self.hid_queue = collections.deque(hid_bytes)
        self.hid_faults = list(hid_faults) if hid_faults else []
        self.hid_requests = []   # requested maximum per CMD_HID_INPUT
        self.hid_dropped = 0
        self.hid_delivered = 0
        # observability
        self.channel_resets = 0
        self.rts_asserts = 0
        self.rts_releases = 0
        self.hunt_writes = 0
        self.wr3_writes = []
        self.preload_present = False
        self.frame_start = None
        self.drain_data_reads = 0
        self.data_reads = 0
        self.requests = []      # (type, seq, status, data)
        self.replies = []       # delivered wire bytes
        self.captures = []      # (frame_start, captured request bytes)

    # -- driver-visible port behaviour ---------------------------------
    def ctrl_read(self):
        self._tick()
        value = 0x04            # TX buffer empty
        if self.rx:
            value |= 0x01       # RX character available
        if self._ready():
            value |= 0x08       # /DCDB: COMMAND_READY
        if self.doorbell or self.hid_queue:
            value |= 0x20       # /CTSB: HID doorbell (held while queued)
        return value

    def data_read(self):
        if not self.reply_queued:
            self.drain_data_reads += 1
        if self.rx:
            self.data_reads += 1
            return self.rx.popleft()
        return 0xFF

    def data_write(self, value):
        self.tx_window.append(value & 0xFF)

    def ctrl_write(self, value):
        value &= 0xFF
        if self.ptr:
            reg = self.ptr
            self.ptr = 0
            self._write_reg(reg, value)
            return
        low = value & 0x07
        if low:
            self.ptr = low
            return
        if (value & 0xF8) == 0x18:      # WR0 channel reset
            self.channel_resets += 1
            self.wr.clear()
            self.rx.clear()
            self.rts = False
            self.tx_enabled = False
            self.reply_pending = None
        # WR0 10h (ext/status reset), 30h (error reset), C0h (EOM reset)
        # and 00h carry no additional state here.

    # -- internal ------------------------------------------------------
    def _ready(self):
        if self.never_ready:
            return False
        if self.rts or self.reply_pending is not None:
            return False
        if self.ready_delay > 0:
            self.ready_delay -= 1
            return False
        return True

    def _tick(self):
        if self.reply_pending is not None:
            if self.reply_countdown > 0:
                self.reply_countdown -= 1
            if self.reply_countdown == 0:
                self.rx.extend(self.reply_pending)
                self.replies.append(bytes(self.reply_pending))
                self.reply_pending = None
                self.reply_queued = True

    def _write_reg(self, reg, value):
        self.wr[reg] = value
        if reg == 3:
            self.wr3_writes.append(value)
            if value & 0x10:            # Enter Hunt Phase
                self.hunt_writes += 1
        elif reg == 5:
            rts = (value & 0x02) != 0
            self.tx_enabled = (value & 0x08) != 0
            if rts and not self.rts:
                self._rts_assert()
            elif self.rts and not rts:
                self._rts_release()

    def _rts_assert(self):
        self.rts = True
        self.rts_asserts += 1
        # A transmitter that is empty when RTS is granted emits underrun
        # fill first; the preload discipline is what prevents it.
        self.preload_present = len(self.tx_window) > 0
        self.capture = bytearray()
        if not self.preload_present:
            self.capture.append(0xFF)
        self.reply_queued = False
        self.reply_pending = None
        # Full-duplex idle bytes the host receiver sees while it sends.
        self.rx.extend(self.leading)

    def _rts_release(self):
        self.rts = False
        self.rts_releases += 1
        self.capture.extend(self.tx_window)
        self.tx_window.clear()
        self._schedule_reply(bytes(self.capture))

    def _schedule_reply(self, capture):
        start = capture.find(b"\xa5\x5a")
        self.frame_start = start
        self.captures.append((start, bytes(capture)))
        if start < 0 or start + 5 > len(capture):
            return
        length = capture[start + 2] | (capture[start + 3] << 8)
        if length < 3 or length > 29 or start + 6 + length > len(capture):
            return
        rtype = capture[start + 4]
        seq = capture[start + 5]
        status = capture[start + 6]
        data = capture[start + 7:start + 7 + (length - 3)]
        self.requests.append((rtype, seq, status, bytes(data)))
        if self.stall:
            return
        self.reply_pending = (self._apply_fault(
            self._handle_request(rtype, seq, data), rtype) +
            self.reply_tail)
        self.reply_countdown = self.reply_delay

    def _handle_request(self, rtype, seq, data):
        """Reply to one command, arming the bulk lane for READY replies."""
        if self.custom_replies and rtype in self.custom_replies:
            cls, payload = self.custom_replies[rtype]
            return self._wrap(cls, seq, self.ST_OK, bytes(payload))
        if self.arbitrary_ready and rtype in self.arbitrary_ready:
            entry = self.arbitrary_ready[rtype]
            status = entry[2] if len(entry) > 2 else self.ST_OK
            return self._ready_reply(rtype, seq, entry[0], entry[1], status)
        if rtype == self.CMD_HID_INPUT:
            return self._build_hid_reply(seq, data)
        if self.CMD_FS2_CAPS <= rtype <= self.CMD_FS2_RMDIR:
            return self._build_fs2_reply(rtype, seq, data)
        if rtype in (self.CMD_FS_OPEN, self.CMD_FS_READ, self.CMD_FS_WRITE,
                     self.CMD_FS_CLOSE, self.CMD_FS_DELETE,
                     self.CMD_XFER_STATUS):
            return self._build_fs_reply(rtype, seq, data)
        return self._build_reply(rtype, seq, data)

    def _hid_take(self):
        """One byte from the MCU queue, or a synthetic fill byte when a
        fault injected more than the queue holds."""
        if self.hid_queue:
            self.hid_delivered += 1
            return self.hid_queue.popleft()
        return 0xEA

    def _build_hid_reply(self, seq, data):
        """CMD_HID_INPUT: `queued`, `dropped`, then queued terminal bytes
        (dequeued as they are sent), capped to the wanted count and the
        protocol's 24-byte DATA maximum.  hid_faults poisons exactly one
        reply: unknown class/status, a wrong response class, a nonzero
        controller status, a short (< 2 DATA) reply, or a reply carrying
        more bytes than requested."""
        wanted = data[0] if data else 0
        self.hid_requests.append(wanted)
        fault = self.hid_faults.pop(0) if self.hid_faults else None
        if fault == "unknown":
            return self._wrap(0xFE, seq, self.ST_UNKNOWN_CMD)
        if fault == "wrong_type":
            return self._wrap(0xFE, seq, self.ST_OK)
        if fault == "short":
            return self._wrap(self.RSP_HID_INPUT, seq, self.ST_OK, b"\x00")
        if fault == "status":
            return self._wrap(self.RSP_HID_INPUT, seq, 0x07,
                              bytes([len(self.hid_queue), self.hid_dropped]))
        if fault == "over_count":
            count = min(wanted + 1, 24)
            payload = bytes(self._hid_take() for _ in range(count))
            return self._wrap(self.RSP_HID_INPUT, seq, self.ST_OK,
                              bytes([len(self.hid_queue), self.hid_dropped]) +
                              payload)
        count = min(len(self.hid_queue), wanted, 24)
        payload = bytes(self._hid_take() for _ in range(count))
        return self._wrap(self.RSP_HID_INPUT, seq, self.ST_OK,
                          bytes([len(self.hid_queue), self.hid_dropped]) +
                          payload)

    def _ready_reply(self, rtype, seq, direction, length, status=None):
        """A READY (or its failed twin) for one bulk-arming class."""
        if status is None:
            status = self.ST_OK
        ident = self._next_xfer_id()
        ready = (bytes([ident, direction, length & 0xFF,
                        (length >> 8) & 0xFF]) + bytes(4))
        if status == self.ST_OK:
            if direction == self.DIR_MCU_TO_Z80:
                payload = bytes(((i * 7) & 0xFF) for i in range(length))
                self.armed_send = (ident, (rtype | 0x80) & 0xFF, seq,
                                   payload)
            else:
                self.armed_recv = (ident, rtype, seq, None, 0, length)
        return self._wrap((rtype | 0x80) & 0xFF, seq, status, ready)

    def _build_reply(self, rtype, seq, data):
        if rtype == self.CMD_LINK_SYNC:
            rsp = 0x8C
            out = b""
        elif rtype == self.CMD_PING:
            rsp = 0x81
            echo = bytearray(data[:self.echo_len])
            echo.extend(b"\x00" * (self.echo_len - len(echo)))
            out = bytearray(self.ping_len)
            out[0:self.echo_len] = echo
            out[16] = self.level
            out[17] = 0x40              # IOC_PING_LINK_SYNCED
            out = bytes(out)
        else:
            rsp = 0xFE
            out = b""
        payload = bytes([rsp, seq, 0x00]) + out
        length = len(payload)
        body = bytes([length & 0xFF, (length >> 8) & 0xFF]) + payload
        crc = ioc_crc16(body)
        return (b"\xa5\x5a" + body +
                bytes([(crc >> 8) & 0xFF, crc & 0xFF]))

    def _apply_fault(self, reply, rtype):
        reply = bytearray(reply)
        if rtype == self.CMD_LINK_SYNC and self.link_faults:
            fault = self.link_faults.pop(0)
        else:
            fault = self.fault
        if fault == "crc":
            reply[-1] ^= 0xFF
        elif fault == "len":
            reply[2] = 0x1E             # LEN beyond the 26-byte maximum
        elif fault == "type":
            # A structurally valid frame carrying the wrong response
            # class: the CRC must be recomputed or the fault would be
            # rejected as a checksum error instead.
            reply[4] = 0x02
            length = reply[2] | (reply[3] << 8)
            crc = ioc_crc16(reply[2:2 + 2 + length])
            reply[-2] = (crc >> 8) & 0xFF
            reply[-1] = crc & 0xFF
        elif fault == "seq":
            reply[5] = (reply[5] + 0x11) & 0xFF
            length = reply[2] | (reply[3] << 8)
            crc = ioc_crc16(reply[2:2 + 2 + length])
            reply[-2] = (crc >> 8) & 0xFF
            reply[-1] = crc & 0xFF
        return bytes(reply)

    # -- /SHARED filesystem subset (mirrors fs_share.c) ----------------
    def _wrap(self, cls, seq, status, payload=b""):
        body = bytes([cls, seq, status]) + bytes(payload)
        head = bytes([len(body) & 0xFF, (len(body) >> 8) & 0xFF])
        crc = ioc_crc16(head + body)
        return (b"\xa5\x5a" + head + body +
                bytes([(crc >> 8) & 0xFF, crc & 0xFF]))

    def _next_xfer_id(self):
        self.xfer_id = self.xfer_id % 255 + 1
        return self.xfer_id

    def _pack_name(self, raw11):
        return raw11.rstrip(b" ").decode("ascii")

    def _fs_close_current(self):
        self.fs_open_name = None
        self.fs_handle = 0
        self.fs_writing = False

    def _build_fs_reply(self, rtype, seq, data):
        if rtype == self.CMD_FS_OPEN and self.fs_open_fault is not None:
            fault = self.fs_open_fault
            if fault == "unknown":
                return self._wrap(0xFE, seq, self.ST_UNKNOWN_CMD)
            if fault == "unknown_status":
                return self._wrap(0xFE, seq, self.ST_FS_ERROR)
            if isinstance(fault, tuple):
                return self._wrap(fault[1], seq, self.ST_OK)
            return self._wrap(self.RSP_FS_OPEN, seq, fault)
        if rtype == self.CMD_FS_OPEN:
            mode = data[0] if len(data) >= 1 else 0
            name = self._pack_name(bytes(data[1:12])) if len(data) >= 12 \
                else ""
            if (not name or
                    any(c in name for c in "/\\:.") or
                    not all(0x21 <= ord(c) <= 0x7E for c in name)):
                return self._wrap(self.RSP_FS_OPEN, seq, self.ST_FS_BAD_NAME)
            self._fs_close_current()
            if mode == 1:
                self.fs_files[name] = bytearray()
            elif name not in self.fs_files:
                # Post-delete reopen (the acceptance fixture's cleanup
                # proof): the frozen answer is the no-file failure with
                # an empty payload.  Tests may inject a media/fs error or
                # a nonempty failure reply here.
                fault = self.fs_delete_reopen_fault
                if fault is not None:
                    if isinstance(fault, tuple):
                        return self._wrap(self.RSP_FS_OPEN, seq, fault[0],
                                          fault[1])
                    return self._wrap(self.RSP_FS_OPEN, seq, fault)
                return self._wrap(self.RSP_FS_OPEN, seq, self.ST_FS_NO_FILE)
            self.fs_open_name = name
            self.fs_handle = self.fs_next_handle
            self.fs_next_handle = self.fs_next_handle % 255 + 1
            self.fs_writing = (mode == 1)
            size = len(self.fs_files[name])
            return self._wrap(
                self.RSP_FS_OPEN, seq, self.ST_OK,
                bytes([self.fs_handle]) + size.to_bytes(4, "little"))

        if rtype == self.CMD_FS_WRITE:
            if (len(data) < 7 or data[0] != self.fs_handle or
                    not self.fs_writing or self.fs_open_name is None):
                return self._wrap(self.RSP_FS_WRITE, seq,
                                  self.ST_FS_NO_HANDLE)
            offset = int.from_bytes(bytes(data[1:5]), "little")
            length = int.from_bytes(bytes(data[5:7]), "little")
            if not 1 <= length <= 512:
                return self._wrap(self.RSP_FS_WRITE, seq, self.ST_FS_RANGE)
            ident = self._next_xfer_id()
            self.armed_recv = (ident, rtype, seq, self.fs_open_name,
                               offset, length)
            self.a_tx_window = bytearray()
            self.tx_underrun = False
            ready = (bytes([ident, self.DIR_Z80_TO_MCU,
                            length & 0xFF, (length >> 8) & 0xFF]) +
                     offset.to_bytes(4, "little"))
            return self._wrap(self.RSP_FS_WRITE, seq, self.ST_OK, ready)

        if rtype == self.CMD_FS_READ:
            if len(data) < 7 or data[0] != self.fs_handle or \
                    self.fs_open_name is None:
                return self._wrap(self.RSP_FS_READ, seq,
                                  self.ST_FS_NO_HANDLE)
            offset = int.from_bytes(bytes(data[1:5]), "little")
            length = int.from_bytes(bytes(data[5:7]), "little")
            if not 1 <= length <= 512:
                return self._wrap(self.RSP_FS_READ, seq, self.ST_FS_RANGE)
            content = self.fs_files.get(self.fs_open_name, bytearray())
            got = max(0, min(length, len(content) - offset))
            out = bytearray(8)
            if got:
                ident = self._next_xfer_id()
                out[0] = ident
                out[1] = self.DIR_MCU_TO_Z80
                out[2] = got & 0xFF
                out[3] = (got >> 8) & 0xFF
                out[4:8] = offset.to_bytes(4, "little")
                self.armed_send = (ident, self.RSP_FS_READ, seq,
                                   bytes(content[offset:offset + got]))
            return self._wrap(self.RSP_FS_READ, seq, self.ST_OK, out)

        if rtype == self.CMD_FS_CLOSE:
            if len(data) < 5 or data[0] != self.fs_handle:
                return self._wrap(self.RSP_FS_CLOSE, seq,
                                  self.ST_FS_NO_HANDLE)
            size = int.from_bytes(bytes(data[1:5]), "little")
            if (self.fs_writing and self.fs_open_name in self.fs_files and
                    size < len(self.fs_files[self.fs_open_name])):
                del self.fs_files[self.fs_open_name][size:]
            self._fs_close_current()
            return self._wrap(self.RSP_FS_CLOSE, seq, self.ST_OK)

        if rtype == self.CMD_FS_DELETE:
            name = self._pack_name(bytes(data[:11])) if len(data) >= 11 \
                else ""
            self._fs_close_current()
            if name in self.fs_files:
                del self.fs_files[name]
                return self._wrap(self.RSP_FS_DELETE, seq, self.ST_OK)
            return self._wrap(self.RSP_FS_DELETE, seq, self.ST_FS_NO_FILE)

        # CMD_XFER_STATUS: the DONE record.
        ident = self.last_xfer_id
        status = self.last_status
        if self.done_fault == "id":
            ident = (ident % 255) + 1
        elif self.done_fault == "status":
            status = self.ST_BULK_FAIL
        self.done_fault = None
        out = bytearray(20)
        out[0] = ident
        out[1] = status
        return self._wrap(self.RSP_XFER_STATUS, seq, self.ST_OK, out)

    # -- FS2 filesystem driver -----------------------

    def fs2_add(self, path, content=b"", directory=False):
        """Install a file or directory.  `path` is a '/'-separated name
        without a leading slash; missing parents are created."""
        parts = tuple(p.upper() for p in path.split("/") if p)
        if not parts:
            return
        for i in range(1, len(parts)):
            self.fs2_dirs.add(parts[:i])
        if directory:
            self.fs2_dirs.add(parts)
        else:
            self.fs2_files[parts] = bytearray(content)

    def fs2_bump_generation(self):
        """Simulate a media change: advance the generation and drop every
        context (the firmware's fs2_media_invalidated)."""
        self.fs2_generation = (self.fs2_generation % 0xFFFFFFFF) + 1
        self.fs2_slots = [None, None]
        self.fs2_dir = None
        self.fs2_resolver = []

    @staticmethod
    def _fs2_pack(name):
        if "." in name:
            base, ext = name.split(".", 1)
        else:
            base, ext = name, ""
        return (base.upper().ljust(8)[:8] +
                ext.upper().ljust(3)[:3]).encode("ascii")

    @staticmethod
    def _fs2_unpack(packed):
        base = packed[:8].decode("ascii", "replace").rstrip(" ")
        ext = packed[8:11].decode("ascii", "replace").rstrip(" ")
        return base + (("." + ext) if ext else "")

    def _fs2_reply(self, rtype, seq, status, payload=b""):
        return self._wrap((rtype | 0x80) & 0xFF, seq, status, bytes(payload))

    @staticmethod
    def _fs2_token(slot, cookie):
        return (slot + 1) | (cookie << 8)

    def _fs2_file_for(self, token):
        raw = token & 0xFF
        if raw == 0 or raw > len(self.fs2_slots):
            return None, self.ST_FS2_NO_HANDLE
        entry = self.fs2_slots[raw - 1]
        if entry is None or entry[2] != (token >> 8):
            return None, self.ST_FS2_NO_HANDLE
        return entry, self.ST_FS2_OK

    def _fs2_lookup(self, leaf):
        return tuple(self.fs2_resolver) + (leaf,)

    def _fs2_children(self, path):
        """Directory entries as (name, is_dir, size), sorted."""
        out = []
        for d in self.fs2_dirs:
            if len(d) == len(path) + 1 and d[:len(path)] == path:
                out.append((d[-1], True, 0))
        for f, content in self.fs2_files.items():
            if len(f) == len(path) + 1 and f[:len(path)] == path:
                out.append((f[-1], False, len(content)))
        return sorted(out)

    def _fs2_close_all(self):
        self.fs2_slots = [None, None]

    def _build_fs2_reply(self, rtype, seq, data):
        self.fs2_counts[rtype] += 1
        if rtype == self.CMD_FS2_CAPS:
            return self._fs2_caps(seq)
        if rtype == self.CMD_FS2_GENERATION:
            return self._fs2_reply(rtype, seq, self.ST_FS2_OK,
                                   self.fs2_generation.to_bytes(4, "little"))
        if rtype == self.CMD_FS2_RESET:
            # The deployed firmware's handler_fs2_reset: drop every open
            # context, advance the generation (a media change to every
            # holder), reset the resolver and answer the new generation.
            self.fs2_bump_generation()
            return self._fs2_reply(rtype, seq, self.ST_FS2_OK,
                                   self.fs2_generation.to_bytes(4, "little"))
        if rtype == self.CMD_FS2_ROOT:
            # The resolver reset the deployed firmware answers without
            # touching the card (MCU `handler_fs2_root`).
            self.fs2_resolver = []
            return self._fs2_reply(rtype, seq, self.ST_FS2_OK)
        if not self.fs2_media:
            # An absent card, as the deployed firmware answers it.  A
            # root *open* still succeeds there - the mount context is
            # cached and FatFs' `f_opendir("/")` reads no directory data -
            # and every operation that reaches the volume fails, starting
            # with the first directory *read* (the
            # driver's media probe reads one entry for exactly that
            # reason, so bench and simulator agree on what "no media"
            # means).
            if rtype == self.CMD_FS2_OPENDIR:
                return self._fs2_opendir(seq)
            return self._fs2_reply(rtype, seq, self.ST_FS2_NO_MEDIA)
        if rtype == self.CMD_FS2_PUSH:
            if len(self.fs2_resolver) >= 16:
                return self._fs2_reply(rtype, seq, self.ST_FS2_RANGE)
            leaf = self._fs2_unpack(bytes(data[:11]))
            if self._fs2_lookup(leaf) not in self.fs2_dirs:
                return self._fs2_reply(rtype, seq, self.ST_FS2_NOT_FOUND)
            self.fs2_resolver.append(leaf)
            return self._fs2_reply(rtype, seq, self.ST_FS2_OK)
        if rtype == self.CMD_FS2_OPEN_RO:
            return self._fs2_open(seq, data, False)
        if rtype == self.CMD_FS2_OPEN_RW:
            return self._fs2_open(seq, data, True)
        if rtype == self.CMD_FS2_READ:
            return self._fs2_read(seq, data)
        if rtype == self.CMD_FS2_WRITE:
            return self._fs2_write(seq, data)
        if rtype == self.CMD_FS2_CLOSE:
            return self._fs2_close(seq, data)
        if rtype == self.CMD_FS2_SYNC:
            return self._fs2_sync(seq, data)
        if rtype == self.CMD_FS2_TRUNCATE:
            return self._fs2_truncate(seq, data)
        if rtype == self.CMD_FS2_STAT:
            return self._fs2_stat(seq, data)
        if rtype == self.CMD_FS2_OPENDIR:
            return self._fs2_opendir(seq)
        if rtype == self.CMD_FS2_READDIR:
            return self._fs2_readdir(seq, data)
        if rtype == self.CMD_FS2_CLOSEDIR:
            return self._fs2_closedir(seq, data)
        if rtype == self.CMD_FS2_MKDIR:
            return self._fs2_mkdir(seq, data)
        if rtype == self.CMD_FS2_UNLINK:
            return self._fs2_unlink(seq, data)
        if rtype == self.CMD_FS2_RMDIR:
            return self._fs2_rmdir(seq, data)
        if rtype == self.CMD_FS2_RENAME:
            return self._fs2_rename(seq, data)
        return self._fs2_reply(rtype, seq, self.ST_FS2_IO)

    def _fs2_caps(self, seq):
        if self.fs2_caps_fault == "unknown":
            return self._wrap(0xFE, seq, self.ST_UNKNOWN_CMD)
        if self.fs2_caps_fault == "status":
            return self._fs2_reply(self.CMD_FS2_CAPS, seq, self.ST_FS2_IO)
        flags = 0x003E  # explicit offset, resolver, generation, stat, space
        if self.fs2_read_only:
            flags |= 0x0001
        else:
            flags |= 0x0FC0  # write/trunc/unlink/rename/mkdir/rmdir
        out = bytearray(14)
        out[0] = 2 if self.fs2_caps_fault == "version" else 1
        out[1] = 1
        out[2:4] = flags.to_bytes(2, "little")
        out[4:8] = self.fs2_generation.to_bytes(4, "little")
        out[8:10] = (128 if self.fs2_caps_fault == "chunk"
                     else 512).to_bytes(2, "little")
        out[10] = 2   # file slots
        out[11] = 1   # directory slots
        out[12] = 16  # resolver components
        out[13] = 2   # token bytes
        return self._fs2_reply(self.CMD_FS2_CAPS, seq, self.ST_FS2_OK, out)

    def _fs2_open(self, seq, data, writable):
        rtype = self.CMD_FS2_OPEN_RW if writable else self.CMD_FS2_OPEN_RO
        if writable and self.fs2_read_only:
            return self._fs2_reply(rtype, seq, self.ST_FS2_READ_ONLY)
        if writable:
            mode = data[0]
            leaf = self._fs2_unpack(bytes(data[1:12]))
        else:
            mode = 0
            leaf = self._fs2_unpack(bytes(data[:11]))
        path = self._fs2_lookup(leaf)
        if path in self.fs2_dirs:
            return self._fs2_reply(rtype, seq, self.ST_FS2_IS_DIR)
        exists = path in self.fs2_files
        if writable:
            if mode == 2:       # CREATE_ALWAYS
                self.fs2_files[path] = bytearray()
            elif mode == 1:     # CREATE_NEW
                if exists:
                    return self._fs2_reply(rtype, seq, self.ST_FS2_EXISTS)
                self.fs2_files[path] = bytearray()
            elif not exists:    # UPDATE
                return self._fs2_reply(rtype, seq, self.ST_FS2_NOT_FOUND)
        elif not exists:
            return self._fs2_reply(rtype, seq, self.ST_FS2_NOT_FOUND)
        slot = None
        for i in range(len(self.fs2_slots)):
            if self.fs2_slots[i] is None:
                slot = i
                break
        if slot is None:
            return self._fs2_reply(rtype, seq, self.ST_FS2_NO_HANDLE)
        self.fs2_cookies[slot] = (self.fs2_cookies[slot] % 255) + 1
        self.fs2_slots[slot] = [path, writable, self.fs2_cookies[slot]]
        out = bytearray(7)
        out[0:2] = self._fs2_token(slot, self.fs2_cookies[slot]).to_bytes(
            2, "little")
        out[2:6] = len(self.fs2_files[path]).to_bytes(4, "little")
        return self._fs2_reply(rtype, seq, self.ST_FS2_OK, out)

    def _fs2_read(self, seq, data):
        if self.fs2_read_fault == "stale":
            return self._fs2_reply(self.CMD_FS2_READ, seq, self.ST_FS2_STALE)
        if self.fs2_read_fault == "io":
            return self._fs2_reply(self.CMD_FS2_READ, seq, self.ST_FS2_IO)
        token = int.from_bytes(bytes(data[0:2]), "little")
        offset = int.from_bytes(bytes(data[2:6]), "little")
        length = int.from_bytes(bytes(data[6:8]), "little")
        entry, status = self._fs2_file_for(token)
        if entry is None:
            return self._fs2_reply(self.CMD_FS2_READ, seq, status)
        if self.fs2_stale_path is not None and entry[0] == self.fs2_stale_path:
            return self._fs2_reply(self.CMD_FS2_READ, seq, self.ST_FS2_STALE)
        if length == 0 or length > 512:
            return self._fs2_reply(self.CMD_FS2_READ, seq, self.ST_FS2_RANGE)
        out = bytearray(8)
        if self.fs2_read_fault == "ready_len":
            # READY reports more than the chunk: the provider must reject.
            out[0] = 1
            out[1] = self.DIR_MCU_TO_Z80
            out[2:4] = (length + 1).to_bytes(2, "little")
            out[4:8] = offset.to_bytes(4, "little")
            return self._fs2_reply(self.CMD_FS2_READ, seq, self.ST_FS2_OK, out)
        content = self.fs2_files.get(entry[0], bytearray())
        got = max(0, min(length, len(content) - offset))
        if got:
            ident = self._next_xfer_id()
            out[0] = ident
            out[1] = self.DIR_MCU_TO_Z80
            out[2:4] = got.to_bytes(2, "little")
            out[4:8] = offset.to_bytes(4, "little")
            self.armed_send = (ident, self.RSP_FS2_READ, seq,
                               bytes(content[offset:offset + got]))
        return self._fs2_reply(self.CMD_FS2_READ, seq, self.ST_FS2_OK, out)

    def _fs2_write(self, seq, data):
        if self.fs2_write_fault == "unknown":
            return self._fs2_reply(self.CMD_FS2_WRITE, seq,
                                   self.ST_FS2_UNKNOWN_WRITE)
        if self.fs2_write_fault == "stale":
            return self._fs2_reply(self.CMD_FS2_WRITE, seq, self.ST_FS2_STALE)
        token = int.from_bytes(bytes(data[0:2]), "little")
        offset = int.from_bytes(bytes(data[2:6]), "little")
        length = int.from_bytes(bytes(data[6:8]), "little")
        entry, status = self._fs2_file_for(token)
        if entry is None:
            return self._fs2_reply(self.CMD_FS2_WRITE, seq, status)
        if not entry[1]:
            return self._fs2_reply(self.CMD_FS2_WRITE, seq,
                                   self.ST_FS2_READ_ONLY)
        if length == 0 or length > 512:
            return self._fs2_reply(self.CMD_FS2_WRITE, seq, self.ST_FS2_RANGE)
        ident = self._next_xfer_id()
        self.armed_recv = (ident, self.CMD_FS2_WRITE, seq, entry[0], offset,
                           length)
        self.a_tx_window = bytearray()
        self.tx_underrun = False
        ready = (bytes([ident, self.DIR_Z80_TO_MCU,
                        length & 0xFF, (length >> 8) & 0xFF]) +
                 offset.to_bytes(4, "little"))
        if self.fs2_write_fault == "done_fail":
            self.done_fault = "status"
        return self._fs2_reply(self.CMD_FS2_WRITE, seq, self.ST_FS2_OK, ready)

    def _fs2_close(self, seq, data):
        token = int.from_bytes(bytes(data[0:2]), "little")
        entry, status = self._fs2_file_for(token)
        if entry is None:
            return self._fs2_reply(self.CMD_FS2_CLOSE, seq, status)
        self.fs2_slots[(token & 0xFF) - 1] = None
        return self._fs2_reply(self.CMD_FS2_CLOSE, seq, self.ST_FS2_OK)

    def _fs2_sync(self, seq, data):
        token = int.from_bytes(bytes(data[0:2]), "little")
        entry, status = self._fs2_file_for(token)
        if entry is None:
            return self._fs2_reply(self.CMD_FS2_SYNC, seq, status)
        return self._fs2_reply(self.CMD_FS2_SYNC, seq, self.ST_FS2_OK)

    def _fs2_truncate(self, seq, data):
        token = int.from_bytes(bytes(data[0:2]), "little")
        size = int.from_bytes(bytes(data[2:6]), "little")
        entry, status = self._fs2_file_for(token)
        if entry is None:
            return self._fs2_reply(self.CMD_FS2_TRUNCATE, seq, status)
        if not entry[1]:
            return self._fs2_reply(self.CMD_FS2_TRUNCATE, seq,
                                   self.ST_FS2_READ_ONLY)
        content = self.fs2_files[entry[0]]
        if size < len(content):
            del content[size:]
        return self._fs2_reply(self.CMD_FS2_TRUNCATE, seq, self.ST_FS2_OK)

    def _fs2_stat(self, seq, data):
        path = self._fs2_lookup(self._fs2_unpack(bytes(data[:11])))
        if path in self.fs2_dirs:
            out = bytearray(5)
            out[4] = self.FS2_ATTR_DIR
            return self._fs2_reply(self.CMD_FS2_STAT, seq, self.ST_FS2_OK, out)
        if path in self.fs2_files:
            out = bytearray(5)
            out[0:4] = len(self.fs2_files[path]).to_bytes(4, "little")
            return self._fs2_reply(self.CMD_FS2_STAT, seq, self.ST_FS2_OK, out)
        return self._fs2_reply(self.CMD_FS2_STAT, seq, self.ST_FS2_NOT_FOUND)

    def _fs2_opendir(self, seq):
        if self.fs2_dir is not None:
            return self._fs2_reply(self.CMD_FS2_OPENDIR, seq,
                                   self.ST_FS2_NO_HANDLE)
        path = tuple(self.fs2_resolver)
        if path not in self.fs2_dirs:
            return self._fs2_reply(self.CMD_FS2_OPENDIR, seq,
                                   self.ST_FS2_NOT_FOUND)
        self.fs2_dir_cookie = (self.fs2_dir_cookie % 255) + 1
        self.fs2_dir = [path, self.fs2_dir_cookie, 0]
        return self._fs2_reply(
            self.CMD_FS2_OPENDIR, seq, self.ST_FS2_OK,
            self._fs2_token(0, self.fs2_dir_cookie).to_bytes(2, "little"))

    def _fs2_readdir(self, seq, data):
        token = int.from_bytes(bytes(data[0:2]), "little")
        if (self.fs2_dir is None or (token & 0xFF) != 1 or
                (token >> 8) != self.fs2_dir[1]):
            return self._fs2_reply(self.CMD_FS2_READDIR, seq,
                                   self.ST_FS2_NO_HANDLE)
        entries = self._fs2_children(self.fs2_dir[0])
        cursor = self.fs2_dir[2]
        if cursor >= len(entries):
            return self._fs2_reply(self.CMD_FS2_READDIR, seq, self.ST_FS2_END)
        name, is_dir, size = entries[cursor]
        self.fs2_dir[2] = cursor + 1
        out = bytearray(16)
        out[0:11] = self._fs2_pack(name)
        out[11] = self.FS2_ATTR_DIR if is_dir else 0
        out[12:16] = size.to_bytes(4, "little")
        return self._fs2_reply(self.CMD_FS2_READDIR, seq, self.ST_FS2_OK, out)

    def _fs2_closedir(self, seq, data):
        token = int.from_bytes(bytes(data[0:2]), "little")
        if self.fs2_dir is None or (token >> 8) != self.fs2_dir[1]:
            return self._fs2_reply(self.CMD_FS2_CLOSEDIR, seq,
                                   self.ST_FS2_NO_HANDLE)
        self.fs2_dir = None
        return self._fs2_reply(self.CMD_FS2_CLOSEDIR, seq, self.ST_FS2_OK)

    def _fs2_mkdir(self, seq, data):
        if self.fs2_read_only:
            return self._fs2_reply(self.CMD_FS2_MKDIR, seq,
                                   self.ST_FS2_READ_ONLY)
        path = self._fs2_lookup(self._fs2_unpack(bytes(data[:11])))
        if path in self.fs2_dirs or path in self.fs2_files:
            return self._fs2_reply(self.CMD_FS2_MKDIR, seq, self.ST_FS2_EXISTS)
        if tuple(self.fs2_resolver) not in self.fs2_dirs:
            return self._fs2_reply(self.CMD_FS2_MKDIR, seq,
                                   self.ST_FS2_NOT_FOUND)
        self.fs2_dirs.add(path)
        return self._fs2_reply(self.CMD_FS2_MKDIR, seq, self.ST_FS2_OK)

    def _fs2_unlink(self, seq, data):
        if self.fs2_read_only:
            return self._fs2_reply(self.CMD_FS2_UNLINK, seq,
                                   self.ST_FS2_READ_ONLY)
        path = self._fs2_lookup(self._fs2_unpack(bytes(data[:11])))
        if path in self.fs2_dirs:
            return self._fs2_reply(self.CMD_FS2_UNLINK, seq,
                                   self.ST_FS2_IS_DIR)
        if path not in self.fs2_files:
            return self._fs2_reply(self.CMD_FS2_UNLINK, seq,
                                   self.ST_FS2_NOT_FOUND)
        del self.fs2_files[path]
        self._fs2_close_all()
        return self._fs2_reply(self.CMD_FS2_UNLINK, seq, self.ST_FS2_OK)

    def _fs2_rmdir(self, seq, data):
        if self.fs2_read_only:
            return self._fs2_reply(self.CMD_FS2_RMDIR, seq,
                                   self.ST_FS2_READ_ONLY)
        path = self._fs2_lookup(self._fs2_unpack(bytes(data[:11])))
        if path not in self.fs2_dirs:
            return self._fs2_reply(self.CMD_FS2_RMDIR, seq,
                                   self.ST_FS2_NOT_FOUND)
        if (any(len(d) > len(path) and d[:len(path)] == path
                for d in self.fs2_dirs) or
                any(len(f) > len(path) and f[:len(path)] == path
                    for f in self.fs2_files)):
            return self._fs2_reply(self.CMD_FS2_RMDIR, seq,
                                   self.ST_FS2_READ_ONLY)
        self.fs2_dirs.discard(path)
        return self._fs2_reply(self.CMD_FS2_RMDIR, seq, self.ST_FS2_OK)

    def _fs2_rename(self, seq, data):
        if self.fs2_read_only:
            return self._fs2_reply(self.CMD_FS2_RENAME, seq,
                                   self.ST_FS2_READ_ONLY)
        src = self._fs2_lookup(self._fs2_unpack(bytes(data[0:11])))
        dst = self._fs2_lookup(self._fs2_unpack(bytes(data[11:22])))
        if src not in self.fs2_files and src not in self.fs2_dirs:
            return self._fs2_reply(self.CMD_FS2_RENAME, seq,
                                   self.ST_FS2_NOT_FOUND)
        if dst in self.fs2_files or dst in self.fs2_dirs:
            return self._fs2_reply(self.CMD_FS2_RENAME, seq,
                                   self.ST_FS2_EXISTS)
        if src in self.fs2_files:
            self.fs2_files[dst] = self.fs2_files.pop(src)
        else:
            self.fs2_dirs.discard(src)
            self.fs2_dirs.add(dst)
        self._fs2_close_all()
        return self._fs2_reply(self.CMD_FS2_RENAME, seq, self.ST_FS2_OK)

    # -- SIO1/A bulk-lane port behaviour -------------------------------
    def bulk_data_read(self):
        self._a_tick()
        if self.a_rx:
            self.a_data_reads += 1
            if not self.a_delivered:
                self.a_drain_reads += 1
            return self.a_rx.popleft()
        return 0xFF

    def bulk_data_write(self, value):
        value &= 0xFF
        self.a_tx_window.append(value)
        if self.armed_recv is not None:
            length = self.armed_recv[5]
            if len(self.a_tx_window) >= length + 8:
                self.tx_underrun = True

    def bulk_ctrl_read(self):
        self._a_tick()
        value = 0x04                 # TX buffer empty
        if self.a_rx:
            value |= 0x01            # RX character available
        if self.tx_underrun:
            value |= 0x40            # RR0 D6: transmitter ran dry
        if self.dcd:
            value |= 0x08            # /DCDA: read admission
        if self.cts:
            value |= 0x20            # /CTSA: phase active
        return value

    def bulk_ctrl_write(self, value):
        value &= 0xFF
        if self.a_ptr:
            reg = self.a_ptr
            self.a_ptr = 0
            self._a_write_reg(reg, value)
            return
        low = value & 0x07
        if low:
            self.a_ptr = low
            return
        cmd = (value >> 3) & 0x07
        reset = (value >> 6) & 0x03
        if cmd == 3 and reset == 0:  # WR0 18h: channel reset
            self.a_channel_resets += 1
            self.a_wr.clear()
            self.a_rx.clear()
            self.a_rts = False
            self.a_tx_enabled = False
            self.dcd = False
            self.cts = False
            self.tx_underrun = False
            self.armed_send = None
            self.armed_recv = None
        # reset == 1/2/3 (Rx CRC, Tx CRC, EOM latch), ext-status and
        # error resets carry no separate modelled state here.

    def _a_write_reg(self, reg, value):
        self.a_wr[reg] = value
        if reg == 3:
            self.a_wr3_writes.append(value)
            if value & 0x10:         # Enter Hunt Phase
                self.a_hunt_writes += 1
        elif reg == 5:
            rts = (value & 0x02) != 0
            self.a_tx_enabled = (value & 0x08) != 0
            if rts and not self.a_rts:
                self._a_rts_assert()
            elif self.a_rts and not rts:
                self._a_rts_release()

    def _a_rts_assert(self):
        self.a_rts = True
        self.a_rts_asserts += 1
        self.a_delivered = False
        if self.armed_send is not None:
            self.admit_countdown = self.dcd_delay
        elif self.armed_recv is not None:
            self.admit_countdown = self.cts_delay

    def _a_rts_release(self):
        self.a_rts = False
        self.a_rts_releases += 1
        if self.armed_send is not None:
            # MCU -> Z80 phase ended.  Whatever the host did not consume
            # is gone with the clock; only the persistent pipeline
            # residue remains for the next bounded drain.
            self.a_rx.clear()
            if (self.read_fault != "no_cts_release" and
                    self.pipeline_residue and self.a_delivered):
                self.a_rx.extend(bytes([0xAA]) * self.pipeline_residue)
            if self.read_fault != "no_cts_release":
                self.dcd = False
                self.cts = False
            self.armed_send = None
            # The next drain counts as pre-delivery once again.
            self.a_delivered = False
        elif self.armed_recv is not None:
            self._finish_receive()

    def _a_tick(self):
        """Advance the admission / commit state machines on a control read."""
        if (self.a_rts and self.armed_send is not None and not self.dcd and
                self.read_fault != "no_dcd"):
            if self.admit_countdown > 0:
                self.admit_countdown -= 1
            if self.admit_countdown == 0:
                self.dcd = True
                self.cts = True
                self._start_send()
        if (self.a_rts and self.armed_recv is not None and not self.cts and
                self.write_fault != "no_cts"):
            if self.admit_countdown > 0:
                self.admit_countdown -= 1
            if self.admit_countdown == 0:
                self.cts = True
        if not self.a_rts and self.cts_off_countdown > 0:
            self.cts_off_countdown -= 1
            if self.cts_off_countdown == 0:
                self.dcd = False
                self.cts = False

    def _start_send(self):
        ident, rtype, seq, payload = self.armed_send
        length = 3 + len(payload)
        rtype_w = rtype
        seq_w = seq
        len_w = length
        status_w = 0
        if self.read_fault == "header_type":
            rtype_w = (rtype + 1) & 0xFF
        elif self.read_fault == "header_seq":
            seq_w = (seq + 0x11) & 0xFF
        elif self.read_fault == "header_len":
            len_w = length + 1
        elif self.read_fault == "header_status":
            status_w = 0x40
        packet = (bytes([0xA5, 0x5A, len_w & 0xFF, (len_w >> 8) & 0xFF,
                         rtype_w, seq_w, status_w]) + bytes(payload) +
                  self.read_trailer)
        if self.read_fault == "stall":
            cut = self.stall_bytes
            if cut is None:
                cut = 8 + (len(payload) // 2)
            packet = packet[:cut]
        self.a_rx.extend(packet)
        self.bulk_sends.append(bytes(packet))
        self.a_delivered = True

    def _finish_receive(self):
        ident, rtype, seq, name, offset, length = self.armed_recv
        capture = bytes(self.a_tx_window)
        self.a_tx_window = bytearray()
        self.armed_recv = None
        self.last_bulk_capture = capture

        start = capture.find(b"\xa5\x5a")
        status = self.ST_OK
        payload = b""
        parsed = None
        if start < 0 or start + 5 > len(capture):
            status = self.ST_BULK_NO_SYNC
        else:
            wire_len = capture[start + 2] | (capture[start + 3] << 8)
            wtype = capture[start + 4]
            wseq = capture[start + 5]
            wstatus = capture[start + 6]
            if (wire_len != length + 3 or wtype != rtype or
                    wseq != seq or wstatus != 0):
                status = self.ST_BULK_FAIL
            elif len(capture) < start + 7 + length:
                status = self.ST_BULK_FAIL   # short stream: host underran
            else:
                payload = capture[start + 7:start + 7 + length]
                parsed = (wtype, wseq, wstatus, payload)
        if self.write_fault == "underrun" and status == self.ST_OK:
            status = self.ST_BULK_FAIL
            payload = payload[:-1] + b"\xAA"
        if parsed is not None:
            self.bulk_requests.append((parsed[0], parsed[1], parsed[2],
                                       parsed[3]))
        self.last_xfer_id = ident
        self.last_status = status
        if status == self.ST_OK and parsed is not None and name is not None:
            if rtype == self.CMD_FS2_WRITE:
                content = self.fs2_files.setdefault(name, bytearray())
            else:
                content = self.fs_files.setdefault(name, bytearray())
            need = offset + length
            if len(content) < need:
                content.extend(b"\x00" * (need - len(content)))
            content[offset:offset + length] = payload
        if self.write_fault == "no_cts_deassert":
            self.cts_off_countdown = -1  # never deasserts
        else:
            self.cts_off_countdown = max(self.commit_delay, 1)


def ioc_crc16(data):
    """CRC-16-CCITT (1021h, preset 0, MSB-first, no final XOR)."""
    crc = 0
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class V9958Peer:
    """Register/VRAM-level model of the LunchCrema V9958 (ports A0h-A4h).

    Models exactly what the KonaOS v9958 driver can observe:

      - the command-port two-byte protocol: a first byte is latched, the
        second byte selects a register (bit 7) or the VRAM address
        organisation (bit 6), using R#14 for A16:A14;
      - the indirect port (A3h): writes reach R#17 and auto-increment the
        command-register pointer; a write to R#46 starts the command;
      - the data port (A0h): auto-increment VRAM writes over the full
        17-bit (128 KiB) address space;
      - the palette port (A2h): two-byte entries selected through R#16,
        auto-advancing after every entry;
      - status register 2 (selected through R#15=2): CE stays set while a
        command executes;
      - HMMV fills execute into the modeled VRAM (HMMM is recorded);
      - the configuration latch: D0 (interrupt route) and D1 (/WS_EN)
        are recorded for every write;
      - instruction-timestamped traces for paced-bootstrap and
        WAIT-enabled-phases validation (the Z80 fills `now` with its
        instruction count before each port access).

    Observability: latch_writes, reg_writes, addr_writes, palette_writes,
    palette, vram_writes, status_reads, indirect_writes, commands,
    protocol_errors.
    """

    PORT_DATA = 0xA0
    PORT_CMD = 0xA1
    PORT_PALETTE = 0xA2
    PORT_INDIRECT = 0xA3
    PORT_CONFIG = 0xA4

    REG_SELECT = 0x80
    VRAM_FLAG = 0x40
    STATUS2_SELECT = 2
    CE = 0x01
    REG_INDIRECT_PTR = 17
    REG_COMMAND = 46
    CMD_BLOCK_BASE = 32
    CMD_BLOCK_COUNT = 15
    OP_HMMV = 0xC0
    OP_HMMM = 0xD0

    def __init__(self, command_busy=400):
        self.now = 0
        self.command_busy = command_busy
        self.regs = [0x00] * 47
        self.addr = 0
        self.vram = bytearray(0x20000)
        self.cmd_first = True
        self.cmd_latch = 0
        self.busy_until = 0
        self.pal_index = 0
        self.pal_half = 0
        self.palette = [[0x00, 0x00] for _ in range(16)]
        self.latch = 0x00
        # Observability.
        self.latch_writes = []     # (now, value)
        self.reg_writes = []       # (value, reg, now)
        self.addr_writes = []      # (now, addr)
        self.palette_writes = []   # (now, value, entry, half)
        self.vram_writes = []      # (addr, value) from the data port
        self.status_reads = []     # (now, ce)
        self.indirect_writes = []  # (now, register)
        self.commands = []         # dict per command-engine start
        self.protocol_errors = []  # (now, kind, value)

    # -- driver-visible port behaviour ---------------------------------
    def data_read(self):
        value = self.vram[self.addr]
        self.addr = (self.addr + 1) & 0x1FFFF
        return value

    def data_write(self, value):
        value &= 0xFF
        self.vram[self.addr] = value
        self.vram_writes.append((self.addr, value))
        self.addr = (self.addr + 1) & 0x1FFFF

    def status_read(self):
        ce = 1 if self.now < self.busy_until else 0
        self.status_reads.append((self.now, ce))
        if (self.regs[15] & 0x0F) == self.STATUS2_SELECT:
            value = 0x00
            if ce:
                value |= self.CE
            if ce:
                value |= 0x40  # VR high only while the engine is busy
            return value
        return 0xFE  # S#0: no collision, vblank set

    def ctrl_write(self, value):
        value &= 0xFF
        if self.cmd_first:
            self.cmd_first = False
            self.cmd_latch = value
            return
        self.cmd_first = True
        if value & self.REG_SELECT:
            self._write_reg(value & 0x3F, self.cmd_latch)
        elif value & self.VRAM_FLAG:
            self.addr = (((self.regs[14] & 0x07) << 14) |
                         ((value & 0x3F) << 8) | self.cmd_latch) & 0x1FFFF
            self.addr_writes.append((self.now, self.addr))
        else:
            self.protocol_errors.append((self.now, "ctrl", value))

    def indirect_write(self, value):
        value &= 0xFF
        reg = self.regs[self.REG_INDIRECT_PTR] & 0x3F
        self.indirect_writes.append((self.now, reg))
        self.regs[reg] = value
        self.regs[self.REG_INDIRECT_PTR] = (reg + 1) & 0x3F
        if reg == self.REG_COMMAND:
            self._command_start(value)

    def palette_write(self, value):
        value &= 0xFF
        entry = self.pal_index & 0x0F
        self.palette_writes.append((self.now, value, entry, self.pal_half))
        self.palette[entry][self.pal_half] = value
        if self.pal_half == 0:
            self.pal_half = 1
        else:
            self.pal_half = 0
            self.pal_index = (entry + 1) & 0x0F

    def latch_write(self, value):
        self.latch = value & 0x03
        self.latch_writes.append((self.now, value))

    # -- internal ------------------------------------------------------
    def _write_reg(self, reg, value):
        reg &= 0x3F
        value &= 0xFF
        self.regs[reg] = value
        self.reg_writes.append((value, reg, self.now))
        if reg == 16:
            self.pal_index = value & 0x0F
            self.pal_half = 0
        elif reg == self.REG_COMMAND:
            self._command_start(value)

    def _command_start(self, code):
        block = list(self.regs[self.CMD_BLOCK_BASE:
                               self.CMD_BLOCK_BASE + self.CMD_BLOCK_COUNT])
        entry = {"code": code, "block": block, "start": self.now,
                 "busy_until": None}
        if (code & 0xF0) == self.OP_HMMV:
            dx = block[4] | (block[5] << 8)
            dy = block[6] | (block[7] << 8)
            nx = block[8] | (block[9] << 8)
            ny = block[10] | (block[11] << 8)
            color = block[12]
            entry["fill"] = (dx, dy, nx, ny, color)
            self._fill(dx, dy, nx, ny, color)
            entry["busy_until"] = self.now + self.command_busy
            self.busy_until = entry["busy_until"]
        elif (code & 0xF0) == self.OP_HMMM:
            entry["busy_until"] = self.now + self.command_busy
            self.busy_until = entry["busy_until"]
        self.commands.append(entry)

    def _fill(self, dx, dy, nx, ny, color):
        if nx == 0 or ny == 0:
            return
        first = dx // 2
        last = (dx + nx) // 2
        for y in range(dy, dy + ny):
            base = (y * 256) & 0x1FFFF
            for offset in range(first, last):
                self.vram[(base + offset) & 0x1FFFF] = color

    def busy(self):
        return self.now < self.busy_until


class CtcChannel:
    """One Z80-CTC channel's counter/timer state (see `Ctc`)."""

    def __init__(self, index):
        self.index = index
        self.control_follows = False
        self.counter_mode = False
        self.prescaler = 16
        self.interrupt_enabled = False
        self.constant = 1
        self.down = 1
        self.running = False
        self.remaining = 0
        self.requested = False
        self.writes = []


class Ctc:
    """Z80-CTC counter/timer model (ports 40h-43h).

    Ports select *physical* channels through the board's crossed wiring:
    the CTC's CS0 (pin 18) is A1 and its CS1 (pin 19) is A0 (the
    Zephyr-80 CP/M BIOS's "Measured again on 2026-09-15"), so the select
    is A0*2 + A1 and `PORTS` - indexed by channel - is not the address
    order: physical channel 1 answers 42h and channel 2 answers 41h.  A
    program that writes the timer's word to 42h therefore arms channel 1
    and its interrupt vectors 02h, not the 04h the timer's stub answers.

    Programming follows the CTC register discipline:

      - a channel write with D0 clear sets the shared IM2 vector base
        (D7..D3); channel N then requests the vector base + 2*N;
      - a channel write with D0 set is a control word: D7 interrupt
        enable, D6 counter (1) or timer (0) mode, D5 prescaler 256 (1)
        or 16 (0), D2 the next write is the time constant, D1 software
        reset;
      - a write after a control word with D2 set is that channel's time
        constant (0 means 256), which loads the down-counter and starts
        the channel.

    Only timer mode and counter mode are counted, each at its own rate.
    A timer channel (D6 clear) divides the CPU clock the Z80 model
    advances per instruction (`Z80.tstates`) by its prescaler; a counter
    channel (D6 set) counts its CLK/TRG input instead, which the board
    drives from the local 14.7456 MHz oscillator's divider at
    `TRIGGER_HZ`, expressed on the same T-state clock.  The prescaler
    divides the CPU clock only, so a counter channel decrements once per
    input edge.  A channel's time constant (0 means 256) reloads the
    down-counter every time it reaches zero; an expiry latches **one**
    interrupt request - a second expiry before the CPU accepts is lost,
    which is the single-pending-interrupt behaviour the masked-window
    bound is about.  A latched request is handed to the CPU (and counted
    in `delivered`) only once the CPU can accept one; it is accepted
    before the next instruction, so a delivery equals a handler run.

    Observability: `writes` (port, value, T-state), `expiries` and
    `delivered` per channel, and the channel state itself.
    """

    # The four ports by physical channel: the board's crossed select
    # wiring (see the class docstring), matching hardware.h's
    # KOS_CTC0_PORT..KOS_CTC3_PORT.
    PORTS = (0x40, 0x42, 0x41, 0x43)

    def channel_of_port(self, port):
        """The physical channel a CTC port selects, or None."""
        try:
            return self.PORTS.index(port & 0xFF)
        except ValueError:
            return None

    # The channel trigger inputs, from the board's 14.7456 MHz oscillator
    # divider (Z80 Peripherals Controller, the CTC trigger table).
    TRIGGER_HZ = (1843200, 3686400, 7372800, 7372800)

    # The system clock the T-state base runs at; a trigger edge every
    # CLOCK_HZ / TRIGGER_HZ T-states.
    CLOCK_HZ = 10000000

    def __init__(self):
        self.cpu = None
        self.vector_base = 0x00
        self.channels = [CtcChannel(index) for index in range(4)]
        self.delivered = [0, 0, 0, 0]
        self.expiries = [0, 0, 0, 0]
        self.writes = []

    def vector(self, index):
        """The IM2 vector byte channel `index` requests."""
        return (self.vector_base & 0xF8) | ((index << 1) & 0x07)

    def attach(self, cpu):
        """Route the CTC's ports to this model and give it the clock."""
        self.cpu = cpu
        cpu.ctc = self
        saved_in = cpu.io_in
        saved_out = cpu.io_out

        def io_in(port):
            if (port & 0xFC) == 0x40:
                return self.read(port & 0xFF)
            return saved_in(port)

        def io_out(port, value):
            if (port & 0xFC) == 0x40:
                self.write(port & 0xFF, value)
                return
            saved_out(port, value)

        cpu.io_in = io_in
        cpu.io_out = io_out
        return self

    def read(self, port):
        """A counter read: the selected channel's current down-counter."""
        return self.channels[self.channel_of_port(port)].down & 0xFF

    def write(self, port, value):
        value &= 0xFF
        channel = self.channels[self.channel_of_port(port)]
        channel.writes.append(value)
        self.writes.append((port, value, self.cpu.tstates
                            if self.cpu is not None else 0))
        if channel.control_follows:
            channel.control_follows = False
            channel.constant = 0x100 if value == 0 else value
            channel.down = channel.constant
            channel.remaining = 0
            channel.running = True
            return
        if (value & 0x01) == 0:
            self.vector_base = value & 0xF8
            return
        if value & 0x02:
            # Software reset: stop counting and drop a latched request;
            # the rest of this control word still applies.
            channel.running = False
            channel.requested = False
        channel.counter_mode = (value & 0x40) != 0
        channel.prescaler = 256 if (value & 0x20) else 16
        channel.interrupt_enabled = (value & 0x80) != 0
        channel.control_follows = (value & 0x04) != 0

    def advance(self, tstates):
        """Count `tstates` clocks, then offer a latched request."""
        for channel in self.channels:
            if not channel.running:
                continue
            if channel.counter_mode:
                # One decrement per CLK/TRG edge.  The input is slower
                # than the T-state clock by a rational factor the board
                # fixes, so the phase is kept in exact integers: an edge
                # every CLOCK_HZ T-states.
                channel.remaining += \
                    tstates * self.TRIGGER_HZ[channel.index]
                divisor = self.CLOCK_HZ
            else:
                channel.remaining += tstates
                divisor = channel.prescaler
            while channel.remaining >= divisor:
                channel.remaining -= divisor
                channel.down -= 1
                if channel.down == 0:
                    channel.down = channel.constant
                    self.expiries[channel.index] += 1
                    if channel.interrupt_enabled:
                        channel.requested = True
        cpu = self.cpu
        if cpu is None or not (cpu.iff1 and not cpu.int_pending
                               and cpu.im == 2 and not cpu.ei_delay):
            return
        for channel in self.channels:
            if channel.requested:
                channel.requested = False
                self.delivered[channel.index] += 1
                cpu.assert_int(self.vector(channel.index))
                return


class MemoryDecoder:
    """MEM_DECODER PLD model, revisions 12 and 13.

    Revision 12: mode 11 shows bank 7 at 2000h-DFFFh.  The latch page
    field D7-D5 only selects the ROM page in mode 00.

    Revision 13 (the programmed baseline, architecture section 2.2): the
    board bodge wires the same D7-D5 bits to the PLD's PAGE0-PAGE2 inputs.
    In mode 11 the 6000h-DFFFh window shows the bank selected by the
    *inverted* page field (page 0 -> bank 7, page 1 -> bank 6, ... page 7
    -> bank 0), 2000h-5FFFh stays forced to bank 7 and 0000h-1FFFh keeps
    the bank field.  In mode 10 the 8000h-BFFFh aperture opens only when
    the page field is non-zero and shows bank 7 - page.  P = 0 is
    revision 12 in every mode (proven by tests/test_decoder_rev13.py);
    the simulator therefore defaults to revision 13, and revision 12
    remains only as the reference the equivalence is proven against.

    The equations below are a direct transcription of the decoder PLD,
    which lives with its WinCUPL simulation vectors in the hardware
    project.
    """

    WINDOW_BASE = 0x6000
    WINDOW_END = 0xE000
    APERTURE_BASE = 0x8000
    APERTURE_END = 0xC000

    def __init__(self, revision=13):
        if revision not in (12, 13):
            raise ValueError("unsupported decoder revision %r" % (revision,))
        self.revision = revision

    def ram_bits(self, addr, latch):
        """Return (RAM_A16, RAM_A17, RAM_A18) as 0/1, from the equations."""
        a13 = (addr >> 13) & 1
        a14 = (addr >> 14) & 1
        a15 = (addr >> 15) & 1
        m0 = (latch >> 3) & 1
        m1 = (latch >> 4) & 1
        q0 = latch & 1
        q1 = (latch >> 1) & 1
        q2 = (latch >> 2) & 1
        p0 = (latch >> 5) & 1
        p1 = (latch >> 6) & 1
        p2 = (latch >> 7) & 1

        if self.revision == 12:
            common_8k = a15 & a14 & a13
            os_body = (a13 & (1 - a14)) | (a14 & (1 - a15)) | \
                      (a15 & (1 - a13))
            force_bank0 = m1 & common_8k
            force_b7 = m1 & m0 & os_body
            a16 = (q0 & (1 - force_bank0)) | force_b7
            a17 = (q1 & (1 - force_bank0)) | force_b7
            a18 = (q2 & (1 - force_bank0)) | force_b7
            return a16, a17, a18

        # Revision 13: the mode-10 aperture overrides the application view.
        if m1 and not m0 and a15 and not a14 and (p0 | p1 | p2):
            return 1 - p0, 1 - p1, 1 - p2
        common_8k = a15 & a14 & a13
        os_body = (a13 & (1 - a14)) | (a14 & (1 - a15)) | \
                  (a15 & (1 - a13))
        window = m1 & m0 & (((1 - a15) & a14 & a13) |
                            (a15 & (1 - (a14 & a13))))
        force_bank0 = m1 & common_8k
        force_b7 = m1 & m0 & os_body & (1 - window)
        a16 = (q0 & (1 - force_bank0) & (1 - window)) | force_b7 | \
              ((1 - p0) & window)
        a17 = (q1 & (1 - force_bank0) & (1 - window)) | force_b7 | \
              ((1 - p1) & window)
        a18 = (q2 & (1 - force_bank0) & (1 - window)) | force_b7 | \
              ((1 - p2) & window)
        return a16, a17, a18

    def bank_of(self, addr, latch):
        """SRAM bank backing `addr` in the RAM-only modes (10h/18h)."""
        a16, a17, a18 = self.ram_bits(addr, latch)
        return (a18 << 2) | (a17 << 1) | a16

    def window_span(self):
        """(base, end) of the mode-11 window."""
        return self.WINDOW_BASE, self.WINDOW_END

    def chip_selects(self, mreq, rd, wr, latch):
        """(ROM_CS, SRAM_CS) as 1 = selected, from the equations."""
        m0 = (latch >> 3) & 1
        m1 = (latch >> 4) & 1
        rom_read = (1 - m1) & (1 - m0)
        rom_cs = 1 if (mreq == 0 and rd == 0 and rom_read) else 0
        sram_cs = 1 if (mreq == 0 and
                        (wr == 0 or (rd == 0 and not rom_read))) else 0
        return rom_cs, sram_cs


# --- instruction clocks (T-states) -----------------------------------------
#
# The Z80 real-time model: the T-state cost of the instruction at PC.  The
# CTC counts the system clock, so a timer-attached replay advances
# `Z80.tstates` by these costs; the interrupt probes read them too.  The
# table is the one the IPC-substrate measuring replay used for these same
# kernels and drivers.  ED-block instructions cost 21 T-states per
# iteration (BC, zero meaning 65536); an instruction the table does not
# cover is charged the four-T-state floor instead of stopping the machine.

ONE_BYTE_T = {0: 4,
 1: 10,
 2: 7,
 3: 6,
 4: 4,
 5: 4,
 6: 7,
 7: 4,
 8: 4,
 9: 11,
 10: 7,
 11: 6,
 12: 4,
 13: 4,
 14: 7,
 15: 4,
 16: 13,
 17: 10,
 18: 7,
 19: 6,
 20: 4,
 21: 4,
 22: 7,
 23: 4,
 24: 12,
 25: 11,
 26: 7,
 27: 6,
 28: 4,
 29: 4,
 30: 7,
 31: 4,
 32: 12,
 33: 10,
 34: 16,
 35: 6,
 36: 4,
 37: 4,
 38: 7,
 39: 4,
 40: 12,
 41: 11,
 42: 16,
 43: 6,
 44: 4,
 45: 4,
 46: 7,
 47: 4,
 48: 12,
 49: 10,
 50: 13,
 51: 6,
 52: 11,
 53: 11,
 54: 10,
 55: 4,
 56: 12,
 57: 11,
 58: 13,
 59: 6,
 60: 4,
 61: 4,
 62: 7,
 63: 4,
 64: 4,
 65: 4,
 66: 4,
 67: 4,
 68: 4,
 69: 4,
 70: 7,
 71: 4,
 72: 4,
 73: 4,
 74: 4,
 75: 4,
 76: 4,
 77: 4,
 78: 7,
 79: 4,
 80: 4,
 81: 4,
 82: 4,
 83: 4,
 84: 4,
 85: 4,
 86: 7,
 87: 4,
 88: 4,
 89: 4,
 90: 4,
 91: 4,
 92: 4,
 93: 4,
 94: 7,
 95: 4,
 96: 4,
 97: 4,
 98: 4,
 99: 4,
 100: 4,
 101: 4,
 102: 7,
 103: 4,
 104: 4,
 105: 4,
 106: 4,
 107: 4,
 108: 4,
 109: 4,
 110: 7,
 111: 4,
 112: 7,
 113: 7,
 114: 7,
 115: 7,
 116: 7,
 117: 7,
 118: 4,
 119: 7,
 120: 4,
 121: 4,
 122: 4,
 123: 4,
 124: 4,
 125: 4,
 126: 7,
 127: 4,
 128: 4,
 129: 4,
 130: 4,
 131: 4,
 132: 4,
 133: 4,
 134: 7,
 135: 4,
 136: 4,
 137: 4,
 138: 4,
 139: 4,
 140: 4,
 141: 4,
 142: 7,
 143: 4,
 144: 4,
 145: 4,
 146: 4,
 147: 4,
 148: 4,
 149: 4,
 150: 7,
 151: 4,
 152: 4,
 153: 4,
 154: 4,
 155: 4,
 156: 4,
 157: 4,
 158: 7,
 159: 4,
 160: 4,
 161: 4,
 162: 4,
 163: 4,
 164: 4,
 165: 4,
 166: 7,
 167: 4,
 168: 4,
 169: 4,
 170: 4,
 171: 4,
 172: 4,
 173: 4,
 174: 7,
 175: 4,
 176: 4,
 177: 4,
 178: 4,
 179: 4,
 180: 4,
 181: 4,
 182: 7,
 183: 4,
 184: 4,
 185: 4,
 186: 4,
 187: 4,
 188: 4,
 189: 4,
 190: 7,
 191: 4,
 192: 5,
 193: 10,
 194: 10,
 195: 10,
 196: 10,
 197: 11,
 198: 7,
 199: 11,
 200: 5,
 201: 10,
 202: 10,
 204: 10,
 205: 17,
 206: 7,
 207: 11,
 208: 5,
 209: 10,
 210: 10,
 211: 11,
 212: 10,
 213: 11,
 214: 7,
 215: 11,
 216: 5,
 217: 4,
 218: 10,
 219: 11,
 220: 10,
 222: 7,
 223: 11,
 224: 5,
 225: 10,
 226: 10,
 227: 19,
 228: 10,
 229: 11,
 230: 7,
 231: 11,
 232: 5,
 233: 4,
 234: 10,
 235: 4,
 236: 10,
 238: 7,
 239: 11,
 240: 5,
 241: 10,
 242: 10,
 243: 4,
 244: 10,
 245: 11,
 246: 7,
 247: 11,
 248: 5,
 249: 6,
 250: 10,
 251: 4,
 252: 10,
 254: 7,
 255: 11}

# ED-prefixed non-block instructions used by the kernel/ISR path.
ED_T = {64: 12,
 65: 12,
 66: 15,
 67: 20,
 68: 8,
 69: 14,
 70: 8,
 71: 9,
 72: 12,
 73: 12,
 74: 15,
 75: 20,
 77: 14,
 79: 9,
 80: 12,
 81: 12,
 82: 15,
 83: 20,
 86: 8,
 87: 9,
 88: 12,
 89: 12,
 90: 15,
 91: 20,
 94: 8,
 95: 9,
 96: 12,
 97: 12,
 98: 15,
 99: 20,
 103: 18,
 104: 12,
 105: 12,
 106: 15,
 107: 20,
 111: 18,
 114: 18,
 115: 20,
 120: 12,
 121: 12,
 122: 15,
 123: 20}

# Block instructions: the simulator executes the whole block in one step
# and counts one step per byte iteration, so the repeat cost is 21 T with
# 16 T for the last iteration.
ED_BLOCK = (160,
 161,
 162,
 163,
 168,
 169,
 170,
 171,
 176,
 177,
 178,
 179,
 184,
 185,
 186,
 187)

# DD/FD (IX/IY) forms: displacement forms and the register/stack forms the
# SDCC frame helpers emit.
DD_T = {9: 15,
 25: 15,
 33: 14,
 34: 20,
 35: 10,
 36: 8,
 37: 8,
 38: 11,
 41: 15,
 42: 20,
 43: 10,
 44: 8,
 45: 8,
 46: 11,
 52: 23,
 53: 23,
 54: 19,
 57: 15,
 68: 8,
 69: 8,
 70: 19,
 76: 8,
 77: 8,
 78: 19,
 84: 8,
 85: 8,
 86: 19,
 92: 8,
 93: 8,
 94: 19,
 96: 8,
 97: 8,
 98: 8,
 99: 8,
 100: 8,
 101: 8,
 102: 19,
 103: 8,
 104: 8,
 105: 8,
 106: 8,
 107: 8,
 108: 8,
 109: 8,
 110: 19,
 111: 8,
 112: 19,
 113: 19,
 114: 19,
 115: 19,
 116: 19,
 117: 19,
 119: 19,
 124: 8,
 125: 8,
 126: 19,
 132: 8,
 133: 8,
 134: 19,
 140: 8,
 141: 8,
 142: 19,
 148: 8,
 149: 8,
 150: 19,
 156: 8,
 157: 8,
 158: 19,
 164: 8,
 165: 8,
 166: 19,
 172: 8,
 173: 8,
 174: 19,
 180: 8,
 181: 8,
 182: 19,
 188: 8,
 189: 8,
 190: 19,
 225: 14,
 227: 23,
 229: 15,
 233: 8,
 249: 10}


# An IM2 interrupt acceptance: the acknowledge cycle and vector fetch.
INTERRUPT_TSTATES = 19


def opcode_cost(z):
    """T-states of the instruction at PC (the four-T-state floor when the
    table has no entry)."""
    op = z.rd(z.pc)
    if op == 0xED:
        second = z.rd((z.pc + 1) & 0xFFFF)
        if second in ED_BLOCK:
            count = (z.b << 8) | z.c
            if count == 0:
                count = 0x10000
            return 21 * count - 5
        return ED_T.get(second, 4)
    if op == 0xCB:
        second = z.rd((z.pc + 1) & 0xFFFF)
        return 12 if (second & 7) == 6 else 8
    if op in (0xDD, 0xFD):
        second = z.rd((z.pc + 1) & 0xFFFF)
        if second == 0xCB:
            return 23
        return DD_T.get(second, 4)
    return ONE_BYTE_T.get(op, 4)


class Z80:
    def __init__(self, page0, page1, rom_pages=None, decoder_revision=13):
        self.bank0 = bytearray(page0)
        self.bank7 = bytearray(page1)
        self.banks = {n: bytearray([FILL] * SZ) for n in range(1, 7)}
        # ROM images for the ROM-read modes.  Page 0/1 mirror the boot
        # images; recovery pages are provided by the caller.
        self.rom = {0: bytearray(page0), 1: bytearray(page1)}
        if rom_pages:
            for number, data in rom_pages.items():
                self.rom[number] = bytearray(data)
        self.latch = 0x00
        self.pc = 0
        self.sp = 0
        self.a = self.f = 0
        self.b = self.c = 0
        self.d = self.e = 0
        self.h = self.l = 0
        self.ix = self.iy = 0
        self.alt_a = self.alt_f = 0
        self.alt_b = self.alt_c = 0
        self.alt_d = self.alt_e = 0
        self.alt_h = self.alt_l = 0
        self.cycles = 0
        # Instruction-level T-states: the real-time base an attached CTC
        # counts.  `cycles` stays the instruction count the peripheral
        # peers timestamp with.
        self.tstates = 0
        # An attached CTC model (None until a replay installs one).
        self.ctc = None
        self.halted = False
        self.interrupts = False
        self.iff1 = False
        self.iff2 = False
        self.ei_delay = 0
        self.i = 0
        self.im = 0
        self.int_pending = False
        self.int_vector = 0
        # Banking-latch decoder model (MEM_DECODER revision 13; revision
        # 12 remains as the reference the P = 0 equivalence is proven
        # against).
        self.decoder = MemoryDecoder(decoder_revision)
        # SIO0/B emulation (ports 22h data / 23h control).  Bytes written
        # to the data port are captured in sio_tx; bytes queued in sio_rx
        # are returned by data-port reads and make RR0's RX-available bit
        # set on control-port reads.  Without pending input the control
        # port reports TX-empty/CTS but no RX-ready, so polling drivers
        # see "no data" instead of the previous 0xFF alias.
        self.sio_tx = bytearray()
        self.sio_rx = bytearray()
        self.sio_ctrl_writes = bytearray()
        # Optional IOC command-lane peer on SIO1/B (ports 32h/33h).
        # Tests that boot an IOC image install one before running.
        self.ioc_peer = None
        # Optional V9958 peer on A0h-A4h.  Tests that boot the v9958
        # module install one before running; the peer's `now` is
        # refreshed with the instruction count before every access so
        # paced-bootstrap and WAIT-phase behaviour can be validated.
        self.vdp_peer = None

    def accept_interrupt(self, vector):
        """Accept one maskable IM2 interrupt and fetch its linked target."""
        if not self.iff1 or self.im != 2:
            raise AssertionError("interrupt not enabled in IM2")
        self.halted = False
        # Interrupt acceptance clears both flip-flops; RETI/RETN restore
        # IFF1 from IFF2, so the exit path needs its own EI.
        self.iff1 = False
        self.iff2 = False
        self._clock(INTERRUPT_TSTATES)
        self.push16(self.pc)
        pointer = ((self.i << 8) | (vector & 0xFF)) & 0xFFFF
        self.pc = self.rd16(pointer)

    # ---- I/O ports ----
    def io_in(self, port):
        """Read one I/O port (the single port model for IN forms)."""
        port &= 0xFF
        if port == 0x00:
            return self.latch
        if self.vdp_peer is not None and 0xA0 <= port <= 0xA4:
            self.vdp_peer.now = self.cycles
            if port == V9958Peer.PORT_DATA:
                return self.vdp_peer.data_read()
            if port == V9958Peer.PORT_CMD:
                return self.vdp_peer.status_read()
            if port == V9958Peer.PORT_PALETTE:
                return 0xFF
            if port == V9958Peer.PORT_INDIRECT:
                return 0xFF
            return self.vdp_peer.latch
        if port == 0x22:
            return self.sio_rx.pop(0) if self.sio_rx else 0xFF
        if port == 0x23:
            # RR0: TX-empty + CTS set; RX-available only when a byte
            # is queued.
            return 0xFE | (0x01 if self.sio_rx else 0x00)
        if self.ioc_peer is not None:
            if port == 0x32:
                return self.ioc_peer.data_read()
            if port == 0x33:
                return self.ioc_peer.ctrl_read()
            if port == 0x30:
                return self.ioc_peer.bulk_data_read()
            if port == 0x31:
                return self.ioc_peer.bulk_ctrl_read()
        return 0xFF

    def io_out(self, port, value):
        """Write one I/O port (the single port model for OUT forms)."""
        port &= 0xFF
        value &= 0xFF
        if port == 0x00:
            self.latch = value
        elif self.vdp_peer is not None and 0xA0 <= port <= 0xA4:
            self.vdp_peer.now = self.cycles
            if port == V9958Peer.PORT_DATA:
                self.vdp_peer.data_write(value)
            elif port == V9958Peer.PORT_CMD:
                self.vdp_peer.ctrl_write(value)
            elif port == V9958Peer.PORT_PALETTE:
                self.vdp_peer.palette_write(value)
            elif port == V9958Peer.PORT_INDIRECT:
                self.vdp_peer.indirect_write(value)
            else:
                self.vdp_peer.latch_write(value)
        elif port == 0x22:
            self.sio_tx.append(value)
        elif port == 0x23:
            self.sio_ctrl_writes.append(value)
        elif self.ioc_peer is not None:
            if port == 0x32:
                self.ioc_peer.data_write(value)
            elif port == 0x33:
                self.ioc_peer.ctrl_write(value)
            elif port == 0x30:
                self.ioc_peer.bulk_data_write(value)
            elif port == 0x31:
                self.ioc_peer.bulk_ctrl_write(value)

    # ---- flags ----
    def _sf(self, bit, on):
        self.f = (self.f | bit) if on else (self.f & ~bit)

    @property
    def CF(self):
        return (self.f >> 0) & 1

    @CF.setter
    def CF(self, v):
        self._sf(1, v)

    @property
    def ZF(self):
        return (self.f >> 6) & 1

    @ZF.setter
    def ZF(self, v):
        self._sf(0x40, v)

    @property
    def SNF(self):
        return (self.f >> 7) & 1

    @SNF.setter
    def SNF(self, v):
        self._sf(0x80, v)

    @property
    def PV(self):
        return (self.f >> 2) & 1

    @PV.setter
    def PV(self, v):
        self._sf(0x04, v)

    # ---- interrupts ----
    def assert_int(self, vector):
        """Assert INT with the given IM2 vector byte; delivered when the
        CPU next accepts maskable interrupts."""
        self.int_vector = vector & 0xFF
        self.int_pending = True

    def _accept_interrupt(self):
        self.int_pending = False
        self.iff1 = False
        self.iff2 = False
        self._clock(INTERRUPT_TSTATES)
        self.push16(self.pc)
        self.pc = self.rd16(((self.i << 8) | self.int_vector) & 0xFFFF)
        self.halted = False

    def _interrupt_check(self):
        if self.int_pending and self.iff1 and not self.ei_delay and self.im == 2:
            self._accept_interrupt()
            return True
        return False

    def _cond(self, cc):
        return {0: not self.ZF, 1: self.ZF, 2: not self.CF, 3: self.CF,
                4: not self.PV, 5: self.PV, 6: not self.SNF, 7: self.SNF}[cc]

    @property
    def PF(self):
        return (self.f >> 2) & 1

    @PF.setter
    def PF(self, v):
        self._sf(0x04, v)

    # ---- memory mapping ----
    def _sram(self, bank):
        if bank == 0:
            return self.bank0
        if bank == 7:
            return self.bank7
        return self.banks[bank]

    def bank_of(self, addr):
        """SRAM bank backing `addr` in the RAM-only modes (10h/18h)."""
        mode = self.latch & 0x18
        bank = self.latch & 0x07
        if addr >= 0xE000:
            return 0
        if mode in (0x10, 0x18):
            # Application and OS modes: the decoder answers, so the
            # mode-10 far aperture and the mode-11 window are modelled.
            return self.decoder.bank_of(addr, self.latch)
        raise SystemExit("sim: bank_of unsupported mode %02Xh" % mode)

    def rd(self, addr):
        mode = self.latch & 0x18
        if mode == 0x00:
            # ROM access: every read (including instruction fetch) is the
            # selected ROM page; writes still land in the selected bank.
            return self.rom[(self.latch >> 5) & 0x07][addr]
        if mode == 0x08:
            # Flat SRAM: the selected bank over the whole address space.
            return self._sram(self.latch & 0x07)[addr]
        return self._sram(self.bank_of(addr))[addr]

    def rd16(self, addr):
        return self.rd(addr) | (self.rd((addr + 1) & 0xFFFF) << 8)

    def wr(self, addr, v):
        mode = self.latch & 0x18
        if mode in (0x00, 0x08):
            self._sram(self.latch & 0x07)[addr] = v & 0xFF
            return
        self._sram(self.bank_of(addr))[addr] = v & 0xFF

    def wr16(self, addr, v):
        self.wr(addr, v & 0xFF)
        self.wr((addr + 1) & 0xFFFF, (v >> 8) & 0xFF)

    def push16(self, v):
        self.sp = (self.sp - 2) & 0xFFFF
        self.wr16(self.sp, v)

    def pop16(self):
        v = self.rd16(self.sp)
        self.sp = (self.sp + 2) & 0xFFFF
        return v

    def fetch(self):
        op = self.rd(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return op

    def fetch16(self):
        lo = self.fetch()
        hi = self.fetch()
        return lo | (hi << 8)

    def set_rr(self, code, v):
        if code == 0: self.b, self.c = (v >> 8) & 0xFF, v & 0xFF
        elif code == 1: self.d, self.e = (v >> 8) & 0xFF, v & 0xFF
        elif code == 2: self.h, self.l = (v >> 8) & 0xFF, v & 0xFF
        else: self.sp = v

    def get_rr(self, code):
        if code == 0: return (self.b << 8) | self.c
        if code == 1: return (self.d << 8) | self.e
        if code == 2: return (self.h << 8) | self.l
        return self.sp

    # ---- 8-bit register-file helpers (r codes 0-7; 6 is (HL)) ----
    def read_r(self, code):
        if code == 0: return self.b
        if code == 1: return self.c
        if code == 2: return self.d
        if code == 3: return self.e
        if code == 4: return self.h
        if code == 5: return self.l
        if code == 6: return self.rd(self.hl)
        return self.a

    def write_r(self, code, value):
        value &= 0xFF
        if code == 0: self.b = value
        elif code == 1: self.c = value
        elif code == 2: self.d = value
        elif code == 3: self.e = value
        elif code == 4: self.h = value
        elif code == 5: self.l = value
        elif code == 6: self.wr(self.hl, value)
        else: self.a = value

    def parity(self, value):
        """P/V for IN r,(C): set on even parity."""
        return 0 if bin(value & 0xFF).count("1") % 2 else 1

    def _clock(self, tstates):
        """Advance the elapsed-clock counters, then an attached CTC: a
        request it raises is pending before the next instruction."""
        self.tstates += tstates
        if self.ctc is not None:
            self.ctc.advance(tstates)

    def step(self):
        # HALT: only an accepted interrupt wakes the CPU; the clock keeps
        # running, so an attached CTC keeps counting through the halt.
        if self.halted:
            if self.int_pending and self.iff1:
                self._accept_interrupt()
            else:
                self._clock(4)
            return
        # IM2 acceptance before the next instruction (blocked for exactly
        # one instruction after EI).
        if self._interrupt_check():
            return
        self.cycles += 1
        self._clock(opcode_cost(self))
        op = self.fetch()
        x, y, z, p, q = (op >> 6) & 3, (op >> 3) & 7, op & 7, (op >> 4) & 3, (op >> 5) & 1
        if op == 0x00: pass
        elif op == 0x07:  # rlca
            c = (self.a >> 7) & 1
            self.a = ((self.a << 1) | c) & 0xFF
            self.CF = c
        elif op == 0x0F:  # rrca
            c = self.a & 1
            self.a = ((self.a >> 1) | (c << 7)) & 0xFF
            self.CF = c
        elif op == 0x17:  # rla
            c = (self.a >> 7) & 1
            self.a = ((self.a << 1) | (1 if self.CF else 0)) & 0xFF
            self.CF = c
        elif op == 0x1F:  # rra
            c = self.a & 1
            self.a = ((self.a >> 1) | ((1 if self.CF else 0) << 7)) & 0xFF
            self.CF = c
        elif op == 0x76: self.halted = True
        elif op == 0xF3: self.iff1 = self.iff2 = False
        elif op == 0xFB:
            self.iff1 = self.iff2 = True
            # EI admits an interrupt only after the following instruction.
            # The per-step countdown below consumes one tick for EI itself.
            self.ei_delay = 2
        elif op == 0x08:  # ex af,af'
            self.a, self.f, self.alt_a, self.alt_f = \
                self.alt_a, self.alt_f, self.a, self.f
        elif op == 0x10:  # djnz d
            off = self.fetch()
            if off & 0x80: off -= 256
            self.b = (self.b - 1) & 0xFF
            if self.b:
                self.pc = (self.pc + off) & 0xFFFF
        elif op == 0xD9:  # exx
            self.b, self.c, self.alt_b, self.alt_c = \
                self.alt_b, self.alt_c, self.b, self.c
            self.d, self.e, self.alt_d, self.alt_e = \
                self.alt_d, self.alt_e, self.d, self.e
            self.h, self.l, self.alt_h, self.alt_l = \
                self.alt_h, self.alt_l, self.h, self.l
        elif op == 0xF9:  # ld sp,hl
            self.sp = (self.h << 8) | self.l
        elif op == 0xED and False: pass
        elif op == 0xC9:  # ret
            self.pc = self.pop16()
        elif op == 0xC3: self.pc = self.fetch16()
        elif op == 0xE9:  # jp (hl)
            self.pc = (self.h << 8) | self.l
        elif op in (0x18, 0x28, 0x20, 0x38, 0x30):
            off = self.fetch()
            if off & 0x80: off -= 256
            if op == 0x18 or (op == 0x28 and self.ZF) or (op == 0x20 and not self.ZF) \
               or (op == 0x38 and self.CF) or (op == 0x30 and not self.CF):
                self.pc = (self.pc + off) & 0xFFFF
        elif op in (0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA):
            nn = self.fetch16()
            cond = {
                0xC2: not self.ZF, 0xCA: self.ZF,
                0xD2: not self.CF, 0xDA: self.CF,
                0xE2: not self.PF, 0xEA: self.PF,
                0xF2: not self.SNF, 0xFA: self.SNF,
            }[op]
            if cond:
                self.pc = nn
        elif op in (0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC):
            nn = self.fetch16()
            if self._cond((op >> 3) & 7):
                self.push16(self.pc)
                self.pc = nn
        elif op in (0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8):
            if self._cond((op >> 3) & 7):
                self.pc = self.pop16()
        elif op == 0xCD:  # call
            target = self.fetch16()
            self.push16(self.pc)
            self.pc = target
        elif op == 0xC7 or (0xC7 <= op <= 0xFF and (op & 0xC7) == 0xC7):
            self.push16(self.pc)
            self.pc = op & 0x38
        elif op in (0xC5, 0xD5, 0xE5, 0xF5):
            vals = {0xC5: (self.b << 8) | self.c, 0xD5: (self.d << 8) | self.e,
                    0xE5: (self.h << 8) | self.l, 0xF5: (self.a << 8) | self.f}
            self.push16(vals[op])
        elif op in (0xC1, 0xD1, 0xE1, 0xF1):
            v = self.pop16()
            if op == 0xC1: self.b, self.c = v >> 8, v & 0xFF
            elif op == 0xD1: self.d, self.e = v >> 8, v & 0xFF
            elif op == 0xE1: self.h, self.l = v >> 8, v & 0xFF
            else: self.a, self.f = v >> 8, v & 0xFF
        elif op in (0xDD, 0xFD):
            self.step_prefix(op)
        elif op == 0xCB:
            self.step_cb()
        elif op in (0xD3, 0xDB):
            port = self.fetch()
            if op == 0xD3:
                self.io_out(port, self.a)
            else:
                self.a = self.io_in(port)
        elif op in (0x01, 0x11, 0x21, 0x31):
            self.set_rr((op >> 4) & 3, self.fetch16())
        elif op == 0x09:  # add hl,bc
            self.hl = self._add16(self.get_rr(2), self.get_rr(0))
        elif op == 0x19:  # add hl,de
            self.hl = self._add16(self.get_rr(2), self.get_rr(1))
        elif op == 0x29:  # add hl,hl
            self.hl = self._add16(self.get_rr(2), self.get_rr(2))
        elif op == 0x22:  # ld (nn),hl
            self.wr16(self.fetch16(), (self.h << 8) | self.l)
        elif op == 0x2A:  # ld hl,(nn)
            self.hl = self.rd16(self.fetch16())
        elif op == 0x32:  # ld (nn),a
            self.wr(self.fetch16(), self.a)
        elif op == 0x3A:  # ld a,(nn)
            self.a = self.rd(self.fetch16())
        elif op == 0x36:  # ld (hl),n
            self.wr((self.h << 8) | self.l, self.fetch())
        elif op == 0x34:  # inc (hl)
            addr = (self.h << 8) | self.l
            old = self.rd(addr)
            v = (old + 1) & 0xFF
            self.wr(addr, v)
            self._inc_flags(old, v)
        elif op == 0x35:  # dec (hl)
            addr = (self.h << 8) | self.l
            old = self.rd(addr)
            v = (old - 1) & 0xFF
            self.wr(addr, v)
            self._dec_flags(old, v)
        elif op == 0x39:  # add hl,sp
            hl = (self.h << 8) | self.l
            res = hl + self.sp
            self.hl = res
            self._sf(1, (res & 0x10000) != 0)
        elif op in (0x47, 0x4F, 0x57, 0x5F, 0x67, 0x6F, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D,
                    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D,
                    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D,
                    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D):
            self.ld_reg(op)
        elif op in (0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E):  # ld r,(hl)
            regs = {0x46: 'b', 0x4E: 'c', 0x56: 'd', 0x5E: 'e', 0x66: 'h', 0x6E: 'l'}
            setattr(self, regs[op], self.rd((self.h << 8) | self.l))
        elif op in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77):  # ld (hl),r
            regs = {0x70: 'b', 0x71: 'c', 0x72: 'd', 0x73: 'e',
                    0x74: 'h', 0x75: 'l', 0x77: 'a'}
            self.wr((self.h << 8) | self.l, getattr(self, regs[op]))
        elif op == 0x7E: self.a = self.rd((self.h << 8) | self.l)
        elif op == 0x77: self.wr((self.h << 8) | self.l, self.a)
        elif op == 0x02: self.wr((self.b << 8) | self.c, self.a)
        elif op == 0x0A: self.a = self.rd((self.b << 8) | self.c)
        elif op == 0x12: self.wr((self.d << 8) | self.e, self.a)
        elif op == 0x1A: self.a = self.rd((self.d << 8) | self.e)
        elif op == 0x3E: self.a = self.fetch()
        elif op in (0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E):
            regs = {0x06: 'b', 0x0E: 'c', 0x16: 'd', 0x1E: 'e', 0x26: 'h', 0x2E: 'l'}
            setattr(self, regs[op], self.fetch())
        elif op == 0x17:  # rla
            oldc = self.CF
            self.CF = (self.a >> 7) & 1
            self.a = ((self.a << 1) | oldc) & 0xFF
        elif op == 0x1F:  # rra
            oldc = self.CF
            self.CF = self.a & 1
            self.a = ((self.a >> 1) | (oldc << 7)) & 0xFF
        elif op == 0x2F: self.a ^= 0xFF
        elif op == 0x37: self.CF = 1
        elif op == 0x3F: self.CF ^= 1
        elif op == 0xAF: self.a = 0
        elif op == 0xA7: self._logic(self.a & self.a)
        elif op == 0xB7: self._logic(self.a | self.a)
        elif op == 0x87:
            self.alu(op)
        elif 0x80 <= op <= 0xBF:
            self.alu(op)
        elif op == 0xE6: self.a = self._logic(self.a & self.fetch())
        elif op == 0xEE: self.a = self._logic(self.a ^ self.fetch())
        elif op == 0xF6: self.a = self._logic(self.a | self.fetch())
        elif op == 0xFE:  # cp n
            n = self.fetch()
            self._logic(self.a - n, sub=True, operand=n, left=self.a)
        elif op == 0xD6: self._sub(self.fetch())
        elif op == 0xDE:  # sbc a,n
            self._sub(self.fetch(), 1 if self.CF else 0)
        elif op == 0xC6: self._add(self.fetch())
        elif op == 0xCE:  # adc a,n
            self._add(self.fetch(), 1 if self.CF else 0)
        elif op == 0xEB:  # ex de,hl
            self.d, self.e, self.h, self.l = self.h, self.l, self.d, self.e
        elif op == 0xE3:  # ex (sp),hl
            hl = (self.h << 8) | self.l
            self.hl = self.rd16(self.sp)
            self.wr16(self.sp, hl)
        elif op == 0x0B: self.set_rr(0, (self.get_rr(0) - 1) & 0xFFFF)
        elif op == 0x1B: self.set_rr(1, (self.get_rr(1) - 1) & 0xFFFF)
        elif op == 0x2B: self.hl = (self.get_rr(2) - 1) & 0xFFFF
        elif op in (0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x3C):  # inc r
            regs = {0x04: 'b', 0x0C: 'c', 0x14: 'd', 0x1C: 'e',
                    0x24: 'h', 0x2C: 'l', 0x3C: 'a'}
            old = getattr(self, regs[op])
            v = (old + 1) & 0xFF
            setattr(self, regs[op], v)
            self._inc_flags(old, v)
        elif op in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x3D):  # dec r
            regs = {0x05: 'b', 0x0D: 'c', 0x15: 'd', 0x1D: 'e',
                    0x25: 'h', 0x2D: 'l', 0x3D: 'a'}
            old = getattr(self, regs[op])
            v = (old - 1) & 0xFF
            setattr(self, regs[op], v)
            self._dec_flags(old, v)
        elif op == 0x3B: self.sp = (self.sp - 1) & 0xFFFF
        elif op == 0x03: self.set_rr(0, (self.get_rr(0) + 1) & 0xFFFF)
        elif op == 0x13: self.set_rr(1, (self.get_rr(1) + 1) & 0xFFFF)
        elif op == 0x23: self.hl = (self.get_rr(2) + 1) & 0xFFFF
        elif op == 0x33: self.sp = (self.sp + 1) & 0xFFFF
        elif op == 0xED:
            self.step_ed()
        else:
            raise SystemExit("sim: unsupported opcode %02X at %04X" % (op, (self.pc - 1) & 0xFFFF))
        # The one-instruction EI delay must survive the EI step itself:
        # EI sets the delay, the instruction after EI runs while the check
        # is blocked, and only then does the delay expire.
        if self.ei_delay and op != 0xFB:
            self.ei_delay -= 1

    def _add16(self, hl, rr):
        res = hl + rr
        self._sf(1, (res & 0x10000) != 0)
        return res & 0xFFFF

    @property
    def hl(self):
        return (self.h << 8) | self.l

    @hl.setter
    def hl(self, v):
        self.h, self.l = (v >> 8) & 0xFF, v & 0xFF

    def ld_reg(self, op):
        dst = {0x47: 'b', 0x4F: 'c', 0x57: 'd', 0x5F: 'e', 0x67: 'h', 0x6F: 'l',
               0x78: 'a', 0x79: 'a', 0x7A: 'a', 0x7B: 'a', 0x7C: 'a', 0x7D: 'a'}.get(op)
        src = {0x47: 'a', 0x4F: 'a', 0x57: 'a', 0x5F: 'a', 0x67: 'a', 0x6F: 'a',
               0x78: 'b', 0x79: 'c', 0x7A: 'd', 0x7B: 'e', 0x7C: 'h', 0x7D: 'l'}.get(op)
        if op & 0xF8 == 0x40:  # 40h-47h: ld b,*
            dst = 'b'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x48:  # 48h-4Fh: ld c,*
            dst = 'c'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x50:  # 50h-57h: ld d,*
            dst = 'd'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x58:  # 58h-5Fh: ld e,*
            dst = 'e'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x60:  # 60h-67h: ld h,*
            dst = 'h'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        elif op & 0xF8 == 0x68:  # 68h-6Fh: ld l,*
            dst = 'l'
            src = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a'][op & 7]
            if src is None:
                return
        if dst and src:
            setattr(self, dst, getattr(self, src))

    def _logic(self, res, sub=False, add=False, operand=0, left=0):
        raw = res
        res = raw & 0xFF
        self.ZF = res == 0
        self.SNF = (res & 0x80) != 0
        if sub:
            self._sf(0x10, ((left ^ operand ^ res) & 0x10) != 0)
            self._sf(0x02, 1)           # N = 1
            self.CF = raw < 0
            # Signed overflow for SBC/SUB/CP.  SDCC's signed comparison
            # idioms test P/V, so a stale value mis-runs valid code.
            self.PV = ((left ^ operand) & (left ^ res) & 0x80) != 0
        elif add:
            self._sf(0x10, ((left ^ operand ^ res) & 0x10) != 0)
            self._sf(0x02, 0)           # N = 0
            self.CF = raw > 0xFF
            self.PV = ((left ^ res) & (operand ^ res) & 0x80) != 0
        else:
            self._sf(0x10, 0)
            self._sf(0x02, 0)           # N = 0
            self.CF = 0
            self.PV = (bin(res).count('1') & 1) == 0
        return res

    def _sub(self, n, carry=0):
        left = self.a
        v = left - n - carry
        self.a = self._logic(v, sub=True, operand=n, left=left)

    def _add(self, n, carry=0):
        left = self.a
        v = left + n + carry
        self.a = self._logic(v, add=True, operand=n, left=left)

    def _inc_flags(self, old, res):
        self.ZF = res == 0
        self.SNF = (res & 0x80) != 0
        self._sf(0x10, (old & 0x0F) == 0x0F)
        self._sf(0x02, 0)
        self.PV = res == 0x80

    def _dec_flags(self, old, res):
        self.ZF = res == 0
        self.SNF = (res & 0x80) != 0
        self._sf(0x10, (old & 0x0F) == 0x00)
        self._sf(0x02, 1)
        self.PV = res == 0x7F

    def alu(self, op):
        srcs = {0: 'b', 1: 'c', 2: 'd', 3: 'e', 4: 'h', 5: 'l', 7: 'a'}
        r = op & 7
        if r == 6:  # (hl)
            v = self.rd((self.h << 8) | self.l)
        else:
            v = getattr(self, srcs[r])
        grp = op & 0xF8
        if grp == 0x80:
            self._add(v)
        elif grp == 0x88:  # adc
            self._add(v, 1 if self.CF else 0)
        elif grp == 0x90:
            self._sub(v)
        elif grp == 0x98:  # sbc
            self._sub(v, 1 if self.CF else 0)
        elif grp == 0xA0:
            self.a = self._logic(self.a & v)
        elif grp == 0xA8:
            self.a = self._logic(self.a ^ v)
        elif grp == 0xB0:
            self.a = self._logic(self.a | v)
        elif grp == 0xB8:  # cp r: flags only, A unchanged
            self._logic(self.a - v, sub=True, operand=v, left=self.a)
        else:
            raise SystemExit("sim: unsupported alu op %02X" % op)

    def step_ed(self):
        op = self.fetch()
        if op == 0xB0:  # ldir
            bc = (self.b << 8) | self.c
            while bc:
                self.wr((self.d << 8) | self.e, self.rd((self.h << 8) | self.l))
                self.hl = (self.get_rr(2) + 1) & 0xFFFF
                de = (self.d << 8) | self.e
                self.d, self.e = ((de + 1) >> 8) & 0xFF, (de + 1) & 0xFF
                bc -= 1
                self.b, self.c = (bc >> 8) & 0xFF, bc & 0xFF
                self.cycles += 1
            self.ZF = 0
        elif op == 0x7B:  # ld sp,(nn)
            self.sp = self.rd16(self.fetch16())
        elif op == 0x73:  # ld (nn),sp
            self.wr16(self.fetch16(), self.sp)
        elif op == 0x5B:  # ld de,(nn)
            self.de = self.rd16(self.fetch16())
        elif op == 0x4B:  # ld bc,(nn)
            self.bc = self.rd16(self.fetch16())
        elif op == 0x53:  # ld (nn),de
            self.wr16(self.fetch16(), (self.d << 8) | self.e)
        elif op == 0x43:  # ld (nn),bc
            self.wr16(self.fetch16(), (self.b << 8) | self.c)
        elif op == 0x47:  # ld i,a
            self.i = self.a
        elif op == 0x57:  # ld a,i (P/V reflects IFF2)
            self.a = self.i
            self._sf(0x04, self.iff2)
        elif op in (0x46, 0x56, 0x5E):
            self.im = {0x46: 0, 0x56: 1, 0x5E: 2}[op]
        elif op == 0x4D:  # reti: IFF1 <- IFF2
            self.pc = self.pop16()
            self.iff1 = self.iff2
        elif op == 0x45:  # retn: IFF1 <- IFF2
            self.pc = self.pop16()
            self.iff1 = self.iff2
        elif op in (0x42, 0x52, 0x62, 0x72,
                    0x4A, 0x5A, 0x6A, 0x7A):  # sbc/adc hl,rr
            rr = (op >> 4) & 3
            carry = 1 if self.CF else 0
            if op & 0x08:  # adc hl,rr
                res = self.hl + self.get_rr(rr) + carry
                self.CF = res > 0xFFFF
            else:          # sbc hl,rr
                res = self.hl - self.get_rr(rr) - carry
                self.CF = res < 0
            self.hl = res & 0xFFFF
            self.ZF = self.hl == 0
            self.SNF = (self.hl & 0x8000) != 0
        elif op == 0xA2:  # ini: (HL) <- in(C), HL++, B--
            value = self.io_in(self.c)
            self.wr((self.h << 8) | self.l, value)
            self.hl = (self.hl + 1) & 0xFFFF
            self.b = (self.b - 1) & 0xFF
            self.ZF = self.b == 0
            self.SNF = (value & 0x80) != 0
            self._sf(0x02, 1)
            self.PV = self.parity(value)
        elif op == 0xA3:  # outi: out(C) <- (HL), HL++, B--
            value = self.rd((self.h << 8) | self.l)
            self.io_out(self.c, value)
            self.hl = (self.hl + 1) & 0xFFFF
            self.b = (self.b - 1) & 0xFF
            self.ZF = self.b == 0
            self.SNF = (value & 0x80) != 0
            self._sf(0x02, 1)
            self.PV = self.parity(value)
        elif (op & 0xC7) == 0x40:  # in r,(c); op 70h reads and discards
            value = self.io_in(self.c)
            code = (op >> 3) & 7
            if code != 6:
                self.write_r(code, value)
            self.ZF = value == 0
            self.SNF = (value & 0x80) != 0
            self.PV = self.parity(value)
        elif (op & 0xC7) == 0x41:  # out (c),r; op 71h writes zero
            code = (op >> 3) & 7
            value = 0 if op == 0x71 else self.read_r(code)
            self.io_out(self.c, value)
        elif op == 0x44:  # neg is an exact 0 - A subtract
            old = self.a
            self.a = 0
            self._sub(old, carry=0)
        else:
            raise SystemExit("sim: unsupported ED %02X at %04X" % (op, (self.pc - 2) & 0xFFFF))

    @property
    def de(self):
        return (self.d << 8) | self.e

    @de.setter
    def de(self, v):
        self.d, self.e = (v >> 8) & 0xFF, v & 0xFF

    @property
    def bc(self):
        return (self.b << 8) | self.c

    @bc.setter
    def bc(self, v):
        self.b, self.c = (v >> 8) & 0xFF, v & 0xFF

    def step_cb(self):
        op = self.fetch()
        regs = {0: 'b', 1: 'c', 2: 'd', 3: 'e', 4: 'h', 5: 'l', 7: 'a'}
        r = op & 7
        mem = r == 6
        value = self.rd(self.hl) if mem else getattr(self, regs[r])
        if op < 0x40:  # rotate/shift (flags approximated; Z/S accurate)
            grp = (op >> 3) & 7
            if grp == 0:    # rlc
                c = (value >> 7) & 1
                value = ((value << 1) | c) & 0xFF
                self.CF = c
            elif grp == 1:  # rrc
                c = value & 1
                value = ((value >> 1) | (c << 7)) & 0xFF
                self.CF = c
            elif grp == 2:  # rl
                c = (value >> 7) & 1
                value = ((value << 1) | self.CF) & 0xFF
                self.CF = c
            elif grp == 3:  # rr
                c = value & 1
                value = ((value >> 1) | (self.CF << 7)) & 0xFF
                self.CF = c
            elif grp == 4:  # sla
                c = (value >> 7) & 1
                value = (value << 1) & 0xFF
                self.CF = c
            elif grp == 5:  # sra
                c = value & 1
                value = (value >> 1) | (value & 0x80)
                self.CF = c
            else:           # srl
                c = value & 1
                value = value >> 1
                self.CF = c
            if mem:
                self.wr(self.hl, value)
            else:
                setattr(self, regs[r], value)
            self.ZF = value == 0
            self.SNF = (value & 0x80) != 0
            self.PV = (bin(value).count('1') & 1) == 0
        elif op < 0x80:  # bit n,r / bit n,(hl): Z = !bit
            bit = (op >> 3) & 7
            self.ZF = not ((value >> bit) & 1)
        elif op < 0xC0:  # res n,r / res n,(hl)
            bit = (op >> 3) & 7
            value = value & ~(1 << bit) & 0xFF
            if mem:
                self.wr(self.hl, value)
            else:
                setattr(self, regs[r], value)
        else:            # set n,r / set n,(hl)
            bit = (op >> 3) & 7
            value = value | (1 << bit)
            if mem:
                self.wr(self.hl, value)
            else:
                setattr(self, regs[r], value)

    def step_prefix(self, op):
        op2 = self.fetch()
        if op == 0xDD:
            reg = self.ix
        else:
            reg = self.iy
        if op2 == 0xCB:  # DD/FD CB d op: rotate/bit/res/set on (ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            addr = (reg + off) & 0xFFFF
            op3 = self.fetch()
            v = self.rd(addr)
            regs = {0: 'b', 1: 'c', 2: 'd', 3: 'e', 4: 'h', 5: 'l', 7: 'a'}
            if op3 < 0x40:  # rotate/shift memory, copy to register
                grp = (op3 >> 3) & 7
                if grp == 0:    # rlc
                    c = (v >> 7) & 1
                    v = ((v << 1) | c) & 0xFF
                    self.CF = c
                elif grp == 1:  # rrc
                    c = v & 1
                    v = ((v >> 1) | (c << 7)) & 0xFF
                    self.CF = c
                elif grp == 2:  # rl
                    c = (v >> 7) & 1
                    v = ((v << 1) | self.CF) & 0xFF
                    self.CF = c
                elif grp == 3:  # rr
                    c = v & 1
                    v = ((v >> 1) | (self.CF << 7)) & 0xFF
                    self.CF = c
                elif grp == 4:  # sla
                    c = (v >> 7) & 1
                    v = (v << 1) & 0xFF
                    self.CF = c
                elif grp == 5:  # sra
                    c = v & 1
                    v = (v >> 1) | (v & 0x80)
                    self.CF = c
                else:           # srl
                    c = v & 1
                    v = v >> 1
                    self.CF = c
                self.wr(addr, v)
                r = op3 & 7
                if r != 6:
                    setattr(self, regs[r], v)
                self.ZF = v == 0
                self.SNF = (v & 0x80) != 0
                self.PV = (bin(v).count('1') & 1) == 0
            elif op3 < 0x80:  # bit n,(ix+d)
                bit = (op3 >> 3) & 7
                self.ZF = not ((v >> bit) & 1)
            elif op3 < 0xC0:  # res n,(ix+d)
                bit = (op3 >> 3) & 7
                self.wr(addr, v & ~(1 << bit) & 0xFF)
            else:             # set n,(ix+d)
                bit = (op3 >> 3) & 7
                self.wr(addr, v | (1 << bit))
        elif op2 == 0x21:
            if op == 0xDD:
                self.ix = self.fetch16()
            else:
                self.iy = self.fetch16()
        elif op2 == 0x2A:  # ld ix/iy,(nn)
            v = self.rd16(self.fetch16())
            if op == 0xDD:
                self.ix = v
            else:
                self.iy = v
        elif op2 == 0x22:  # ld (nn),ix/iy
            self.wr16(self.fetch16(), reg)
        elif op2 == 0x23:  # inc ix/iy
            if op == 0xDD:
                self.ix = (self.ix + 1) & 0xFFFF
            else:
                self.iy = (self.iy + 1) & 0xFFFF
        elif op2 == 0x2B:  # dec ix/iy
            if op == 0xDD:
                self.ix = (self.ix - 1) & 0xFFFF
            else:
                self.iy = (self.iy - 1) & 0xFFFF
        elif op2 == 0xE5:  # push ix/iy
            self.push16(self.ix if op == 0xDD else self.iy)
        elif op2 == 0xE1:  # pop ix/iy
            v = self.pop16()
            if op == 0xDD:
                self.ix = v
            else:
                self.iy = v
        elif op2 == 0xE9:  # jp (ix)/(iy)
            self.pc = reg
        elif op2 == 0xF9:  # ld sp,ix/iy
            self.sp = reg
        elif op2 in (0x09, 0x19, 0x29, 0x39):  # add ix/iy,rr (flags approx.)
            rr = (op2 >> 4) & 3
            # The prefixed forms answer the index register where the
            # unprefixed encoding selects HL (add iy,iy is FD 29).
            res = reg + (reg if rr == 2 else self.get_rr(rr))
            if op == 0xDD:
                self.ix = res & 0xFFFF
            else:
                self.iy = res & 0xFFFF
            self._sf(1, (res & 0x10000) != 0)
        elif op2 == 0x36:  # ld (ix+d),n
            off = self.fetch()
            if off & 0x80: off -= 256
            self.wr((reg + off) & 0xFFFF, self.fetch())
        elif op2 in (0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E):  # ld r,(ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            v = self.rd((reg + off) & 0xFFFF)
            regs = {0x46: 'b', 0x4E: 'c', 0x56: 'd', 0x5E: 'e',
                    0x66: 'h', 0x6E: 'l', 0x7E: 'a'}
            setattr(self, regs[op2], v)
        elif op2 in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77):  # ld (ix+d),r
            off = self.fetch()
            if off & 0x80: off -= 256
            regs = {0x70: 'b', 0x71: 'c', 0x72: 'd', 0x73: 'e',
                    0x74: 'h', 0x75: 'l', 0x77: 'a'}
            self.wr((reg + off) & 0xFFFF, getattr(self, regs[op2]))
        elif op2 in (0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE):  # alu a,(ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            v = self.rd((reg + off) & 0xFFFF)
            grp = op2 & 0xF8
            if grp == 0x80:
                self._add(v)
            elif grp == 0x88:
                self._add(v, 1 if self.CF else 0)  # adc (ix+d)
            elif grp == 0x90:
                self._sub(v)
            elif grp == 0x98:
                self._sub(v, 1 if self.CF else 0)  # sbc (ix+d)
            elif grp == 0xA0:
                self.a = self._logic(self.a & v)
            elif grp == 0xA8:
                self.a = self._logic(self.a ^ v)
            elif grp == 0xB0:
                self.a = self._logic(self.a | v)
            else:
                self._logic(self.a - v, sub=True)  # cp
        elif op2 in (0x34, 0x35):  # inc/dec (ix+d)
            off = self.fetch()
            if off & 0x80: off -= 256
            addr = (reg + off) & 0xFFFF
            old = self.rd(addr)
            v = (old + 1) & 0xFF if op2 == 0x34 else (old - 1) & 0xFF
            self.wr(addr, v)
            if op2 == 0x34:
                self._inc_flags(old, v)
            else:
                self._dec_flags(old, v)
        else:
            raise SystemExit("sim: unsupported %02X %02X" % (op, op2))


class Ctc0Injector:
    """Host stand-in for the Z80-CTC0 counter (test helper).

    The simulator models no timer, so acceptance replays that include the
    interrupt-executive group inject the deliveries themselves: while the
    position-independent smoke caller (mode 10, copied into the caller
    window) or the mode-11 helper opens an interrupt window with the
    simulator-only arming cell at FFh, one IM2 interrupt at vector 00h is
    injected.  Call `watch(cpu)` once per instruction; the cell is the
    kernel's `_irq_active_source`, which the test assembly arms.
    """

    def __init__(self, syms, caller_base=0x1000, vector=0x00):
        self.blob_start = syms["kos_irq_test_blob_start"]
        self.blob_end = syms["kos_irq_test_blob_end"]
        self.helper_start = syms["_kos_irq_test_mode11"]
        self.helper_end = syms["kos_irq_test_mode11_end"]
        self.source_cell = syms["_irq_active_source"]
        self.caller_base = caller_base
        self.vector = vector
        self.armed = False
        self.pending = 0

    def __call__(self, cpu):
        op = cpu.rd(cpu.pc)
        in_blob = (self.caller_base <= cpu.pc
                   < self.caller_base + (self.blob_end - self.blob_start))
        in_helper = self.helper_start <= cpu.pc < self.helper_end
        if (in_blob or in_helper) and op == 0xFB:  # EI
            self.pending = 2
        if self.armed and cpu.iff1 and not cpu.int_pending:
            if cpu.rd(self.source_cell) == 0xFF:
                cpu.assert_int(self.vector)
        if self.pending > 0:
            self.pending -= 1
            if self.pending == 0:
                self.armed = True
        elif (in_blob or in_helper) and op == 0xF3 and not cpu.halted:
            self.armed = False


# The nucleus mapping (mode 11, page field zero) the serial connect delay
# is written for; see skip_serial_connect_delay().
SERIAL_CONNECT_DELAY_LATCH = 0x18


def skip_serial_connect_delay(cpu, delay_address):
    """Skip the kernel's ~1 s serial connect spin; True when it fired.

    `_kernel_serial_connect_delay` is a pure CPU spin (no I/O), and every
    replay returns from it instead of burning the wall-clock delay.  The
    address is only that spin while the nucleus is mapped: latch 18h is
    mode 11 with the page field at zero, which puts bank 7 in the
    6000h-DFFFh window.  With bank 6 in the window - the mode-10 driver
    boot gate and the mode-11 driver calls - the same PC is
    mid-instruction of a resident-driver image, so the skip must not
    fire there; popping the stack in the middle of driver code corrupts
    the loader.
    """
    if cpu.latch == SERIAL_CONNECT_DELAY_LATCH and cpu.pc == delay_address:
        cpu.pc = cpu.pop16()
        return True
    return False


def read_symbols(map_path):
    import re
    syms = {}
    with open(map_path) as handle:
        for line in handle:
            m = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
            if m:
                syms[m.group(2)] = int(m.group(1), 16)
    return syms


def main():
    img_path = sys.argv[1]
    with open(img_path, "rb") as handle:
        img = handle.read()
    page0, page1 = img[:0x10000], img[0x10000:]
    z = Z80(page0, page1)
    syms = read_symbols(img_path.replace("konaos-selftest.bin", "konaos.map"))

    # Locate the driver's `call _kos_selftest_run` to find the return site.
    run = syms["_kos_selftest_run"]
    driver = syms["_kos_run_syscall_selftest"]
    code = bytes(page1[a] if a >= 0x2000 and a < 0xE000 else page0[a] for a in range(driver, driver + 0x80))
    call_bytes = bytes([0xCD, run & 0xFF, (run >> 8) & 0xFF])
    ret_site = driver + code.index(call_bytes) + 3

    bank = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    # Boot state: bank 0 populated from page 0, bank 7 from page 1,
    # mode 11 over bank 0, kernel stack in common.
    z.latch = 0x18
    z.sp = 0xFEFE
    z.pc = run
    z.a = bank
    z.push16(ret_site)
    z.sp = 0xFEFC

    trace = len(sys.argv) > 2 and sys.argv[2] == "trace"
    limit = 2000000
    steps = 0
    while steps < limit:
        steps += 1
        if trace and steps < 600:
            print("%6d PC=%04X SP=%04X A=%02X BC=%02X%02X DE=%02X%02X IX=%04X IY=%04X"
                  " latch=%02X" % (steps, z.pc, z.sp, z.a, z.b, z.c, z.d, z.e, z.ix, z.iy, z.latch))
        z.step()
        if z.halted:
            print("sim: HALT at step %d, PC=%04X" % (steps, z.pc))
            break
        if z.pc == ret_site:
            print("sim: returned to driver at step %d" % steps)
            print("sim: result=%02X bank=%02X sp=%04X"
                  % (z.bank0[syms["_kos_selftest_result"]], z.bank0[syms["_kos_selftest_bank"]], z.sp))
            break
        if z.pc > 0x10000 or steps > limit:
            print("sim: runaway at step %d, PC=%04X" % (steps, z.pc))
            break
    else:
        print("sim: step limit reached, PC=%04X" % z.pc)


if __name__ == "__main__":
    main()

"""The simulator's C Z80 core (ctypes side).

`make_core_class(Machine, Ctc, tables)` returns `z80sim.Z80`: a subclass of
the board class `z80sim.Machine` whose registers and counters live in the
C `struct core` (z80core.h) and whose `step()` and `run()` run the C core.  Memory stays the Python bytearrays: the core
writes through their buffers, so `z.bank0[...]` and `z.rd()` see the same
bytes.  A board="mk2" instance keeps its mapper state (16 entries, the
control bits) in the C struct and the 1 MiB physical array in `z.phys`;
`z.mapper` is a view that reads and writes that state, and translates
through the C core.  Ports other than the latch's model call the instance's `io_in` /
`io_out` attributes, so wrappers installed on an instance (the CTC's, a
test's) keep working.  An attached CTC moves its counting state into C;
the V9958 frame clock calls back into Python while its period is set.

The library is `build/sim/libz80core.so` (`make sim-core`).
"""

import ctypes
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LIBRARY = os.path.join(ROOT, "build", "sim", "libz80core.so")

CORE_OK, CORE_PYERR, CORE_EXIT, CORE_KEYERR, CORE_INDEXERR = range(5)
RUN_REASONS = ("limit", "stop", "out", "halt")

IO_IN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)
IO_OUT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_int)
CLOCK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int64)
ACCEPT = ctypes.CFUNCTYPE(ctypes.c_int)

REGISTERS = ("a", "f", "b", "c", "d", "e", "h", "l",
             "alt_a", "alt_f", "alt_b", "alt_c",
             "alt_d", "alt_e", "alt_h", "alt_l",
             "pc", "sp", "ix", "iy", "i", "im")
FLAGS = ("iff1", "iff2", "halted", "int_pending")
COUNTERS = ("int_vector", "ei_delay", "latch")


class CtcChannelState(ctypes.Structure):
    _fields_ = [("control_follows", ctypes.c_int32),
                ("counter_mode", ctypes.c_int32),
                ("prescaler", ctypes.c_int32),
                ("interrupt_enabled", ctypes.c_int32),
                ("constant", ctypes.c_int32),
                ("down", ctypes.c_int32),
                ("running", ctypes.c_int32),
                ("requested", ctypes.c_int32),
                ("remaining", ctypes.c_int64)]


class CtcState(ctypes.Structure):
    _fields_ = [("vector_base", ctypes.c_int32),
                ("pad", ctypes.c_int32),
                ("ch", CtcChannelState * 4),
                ("delivered", ctypes.c_int64 * 4),
                ("expiries", ctypes.c_int64 * 4)]


class CoreState(ctypes.Structure):
    _fields_ = ([(name, ctypes.c_int32) for name in REGISTERS] +
                [(name, ctypes.c_int32) for name in
                 ("iff1", "iff2", "halted", "int_pending", "int_vector",
                  "ei_delay", "latch", "revision")] +
                [("cycles", ctypes.c_int64),
                 ("tstates", ctypes.c_int64),
                 ("bank", ctypes.c_void_p * 8),
                 ("bank_len", ctypes.c_int64 * 8),
                 ("rom", ctypes.c_void_p * 8),
                 ("rom_len", ctypes.c_int64 * 8),
                 ("one_t", ctypes.c_int32 * 256),
                 ("ed_t", ctypes.c_int32 * 256),
                 ("dd_t", ctypes.c_int32 * 256),
                 ("ed_block", ctypes.c_int32 * 256),
                 ("interrupt_tstates", ctypes.c_int32),
                 ("ctc", ctypes.POINTER(CtcState)),
                 ("py_clock", ctypes.c_int32),
                 ("pad0", ctypes.c_int32),
                 ("io_in", IO_IN),
                 ("io_out", IO_OUT),
                 ("clock", CLOCK),
                 ("err", ctypes.c_int32),
                 ("err_arg", ctypes.c_int32),
                 ("msg", ctypes.c_char * 96),
                 ("stop_out_port", ctypes.c_int32),
                 ("out_hit", ctypes.c_int32),
                 ("py_accept", ctypes.c_int32),
                 ("pad1", ctypes.c_int32),
                 ("accept", ACCEPT),
                 ("nmi_pending", ctypes.c_int32),
                 ("pad2", ctypes.c_int32),
                 ("rt_hz", ctypes.c_int64),
                 ("rt_acc", ctypes.c_int64),
                 ("rt_ticks", ctypes.c_int64),
                 ("nmi_count", ctypes.c_int64),
                 ("board", ctypes.c_int32),
                 ("dat_bank", ctypes.c_int32),
                 ("map_enable", ctypes.c_int32),
                 ("pad3", ctypes.c_int32),
                 ("dat", ctypes.c_int32 * 16),
                 ("phys", ctypes.c_void_p),
                 ("phys_len", ctypes.c_int64)])


_lib = None


def library():
    """The loaded core library; a missing build is an error naming its
    target."""
    global _lib
    if _lib is None:
        if not os.path.isfile(LIBRARY):
            raise RuntimeError(
                "the simulator core %s is missing; run `make sim-core`"
                % LIBRARY)
        lib = ctypes.CDLL(LIBRARY)
        lib.core_step.argtypes = [ctypes.POINTER(CoreState)]
        lib.core_step.restype = ctypes.c_int
        lib.core_clock.argtypes = [ctypes.POINTER(CoreState), ctypes.c_int64]
        lib.core_clock.restype = None
        lib.core_accept.argtypes = [ctypes.POINTER(CoreState)]
        lib.core_accept.restype = ctypes.c_int
        lib.core_run.argtypes = [ctypes.POINTER(CoreState), ctypes.c_int64,
                                 ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_int, ctypes.c_int,
                                 ctypes.POINTER(ctypes.c_int64)]
        lib.core_run.restype = ctypes.c_int
        lib.core_rd.argtypes = [ctypes.POINTER(CoreState), ctypes.c_int]
        lib.core_rd.restype = ctypes.c_int
        lib.core_wr.argtypes = [ctypes.POINTER(CoreState), ctypes.c_int,
                                ctypes.c_int]
        lib.core_wr.restype = None
        lib.core_translate.argtypes = [ctypes.POINTER(CoreState),
                                       ctypes.c_int]
        lib.core_translate.restype = ctypes.c_int
        _lib = lib
    return _lib


def _register(name):
    def get(self):
        return getattr(self._c, name)

    def put(self, value):
        setattr(self._c, name, int(value))
    return property(get, put)


def _mk1_only(prop):
    """The Mk1 banking latch: absent on a Mk2 instance."""
    def get(self):
        if self.board == "mk2":
            raise AttributeError("latch")
        return prop.fget(self)

    def put(self, value):
        if self.board == "mk2":
            raise AttributeError("latch")
        prop.fset(self, value)
    return property(get, put)


def _flag(name):
    def get(self):
        return bool(getattr(self._c, name))

    def put(self, value):
        setattr(self._c, name, 1 if value else 0)
    return property(get, put)


class _ChannelView:
    """A CtcChannel whose counting state lives in the C core's CTC."""

    _NAMES = ("control_follows", "counter_mode", "prescaler",
              "interrupt_enabled", "constant", "down", "running",
              "requested", "remaining")
    _BOOLS = ("control_follows", "counter_mode", "interrupt_enabled",
              "running", "requested")

    def __init__(self, state, channel):
        object.__setattr__(self, "_s", state)
        object.__setattr__(self, "index", channel.index)
        object.__setattr__(self, "writes", channel.writes)
        for name in self._NAMES:
            setattr(self, name, getattr(channel, name))

    def __getattr__(self, name):
        if name in _ChannelView._NAMES:
            value = getattr(self._s, name)
            return bool(value) if name in _ChannelView._BOOLS else value
        raise AttributeError(name)

    def __setattr__(self, name, value):
        if name in _ChannelView._NAMES:
            setattr(self._s, name, int(value))
        else:
            object.__setattr__(self, name, value)


class _CounterView:
    """`delivered` / `expiries`: a list-like view of a C int64[4]."""

    def __init__(self, array):
        self._a = array

    def __getitem__(self, index):
        if isinstance(index, slice):
            return list(self._a)[index]
        return self._a[index]

    def __setitem__(self, index, value):
        self._a[index] = value

    def __len__(self):
        return 4

    def __iter__(self):
        return iter(list(self._a))

    def __eq__(self, other):
        return list(self._a) == list(other)

    def __repr__(self):
        return repr(list(self._a))


class _EntryView:
    """`Mk2Mapper.entries`: a list-like view of the C int32[16]."""

    def __init__(self, array):
        self._a = array

    def __getitem__(self, index):
        if isinstance(index, slice):
            return list(self._a)[index]
        return self._a[index]

    def __setitem__(self, index, value):
        self._a[index] = value

    def __len__(self):
        return len(self._a)

    def __iter__(self):
        return iter(list(self._a))

    def __eq__(self, other):
        return list(self._a) == list(other)

    def __repr__(self):
        return repr(list(self._a))


def make_core_class(Machine, Ctc, tables):
    """The simulator's `Z80`: the board class `Machine` (z80sim.py) with
    the C core's registers, stepping, interrupts and clock.  `tables` is
    (ONE_BYTE_T, ED_T, DD_T, ED_BLOCK, INTERRUPT_TSTATES)."""
    one_t, ed_t, dd_t, ed_block, interrupt_tstates = tables
    lib = library()

    class CtcBound(Ctc):
        """A Ctc whose state the C core counts."""

        @property
        def vector_base(self):
            return self._cs.vector_base

        @vector_base.setter
        def vector_base(self, value):
            self._cs.vector_base = int(value)

        def advance(self, tstates):
            raise AssertionError("a C-bound CTC is advanced by the core")

    class MapperView(Machine.Mapper):
        """A Mk2Mapper whose state lives in the C core and whose
        translation runs there; reset, I/O decode and the entry pattern
        are the Python reference's."""

        def __init__(self, cpu, seed):
            object.__setattr__(self, "_cpu", cpu)
            object.__setattr__(self, "_s", cpu._c)
            super().__init__(seed)

        def _make_entries(self):
            return _EntryView(self._s.dat)

        @property
        def dat_bank(self):
            return self._s.dat_bank

        @dat_bank.setter
        def dat_bank(self, value):
            self._s.dat_bank = int(value)

        @property
        def map_enable(self):
            return self._s.map_enable

        @map_enable.setter
        def map_enable(self, value):
            self._s.map_enable = int(value)

        def physical(self, la):
            return lib.core_translate(ctypes.byref(self._s), int(la))

    class CZ80(Machine):
        """The board with the C core as its CPU."""

        CORE = "c"

        def __init__(self, page0, page1, rom_pages=None,
                     decoder_revision=13, board="mk1", mapper_seed=1):
            c = CoreState()
            c.board = 1 if board == "mk2" else 0
            object.__setattr__(self, "_c", c)
            object.__setattr__(self, "_ctc", None)
            object.__setattr__(self, "_exc", None)
            object.__setattr__(self, "_views", [])
            for n in range(256):
                c.one_t[n] = one_t.get(n, 4)
                c.ed_t[n] = ed_t.get(n, 4)
                c.dd_t[n] = dd_t.get(n, 4)
                c.ed_block[n] = 1 if n in ed_block else 0
            c.interrupt_tstates = interrupt_tstates
            c.revision = decoder_revision
            c.stop_out_port = -1

            def io_in(port):
                try:
                    return self.io_in(port) & 0xFF
                except BaseException as exc:  # re-raised by step()
                    object.__setattr__(self, "_exc", exc)
                    return -1

            def io_out(port, value):
                try:
                    self.io_out(port, value)
                    return 0
                except BaseException as exc:
                    object.__setattr__(self, "_exc", exc)
                    return -1

            def clock(tstates):
                try:
                    self.vdp_peer.advance(tstates, self)
                    return 0
                except BaseException as exc:
                    object.__setattr__(self, "_exc", exc)
                    return -1

            def accept():
                try:
                    self._accept_interrupt()
                    return 0
                except BaseException as exc:
                    object.__setattr__(self, "_exc", exc)
                    return -1

            callbacks = (IO_IN(io_in), IO_OUT(io_out), CLOCK(clock),
                         ACCEPT(accept))
            object.__setattr__(self, "_callbacks", callbacks)
            c.io_in, c.io_out, c.clock, c.accept = callbacks
            super().__init__(page0, page1, rom_pages, decoder_revision,
                             board, mapper_seed)
            self._bind_memory()

        def _make_mapper(self, seed):
            return MapperView(self, seed)

        def _rd_mk2(self, addr):
            value = lib.core_rd(ctypes.byref(self._c), int(addr) & 0xFFFF)
            if self._c.err:
                self._raise(self._c.err)
            return value

        def _wr_mk2(self, addr, v):
            lib.core_wr(ctypes.byref(self._c), int(addr) & 0xFFFF,
                        int(v) & 0xFF)
            if self._c.err:
                self._raise(self._c.err)

        def _bind_memory(self):
            c = self._c
            views = []
            if self.board == "mk2":
                view = (ctypes.c_char * len(self.phys)).from_buffer(
                    self.phys)
                c.phys = ctypes.addressof(view)
                c.phys_len = len(self.phys)
                object.__setattr__(self, "_views", [view])
                return
            for bank in range(8):
                data = self._sram(bank)
                view = (ctypes.c_char * len(data)).from_buffer(data)
                views.append(view)
                c.bank[bank] = ctypes.addressof(view)
                c.bank_len[bank] = len(data)
            for page in range(8):
                data = self.rom.get(page)
                if data is None:
                    c.rom[page] = None
                    c.rom_len[page] = 0
                    continue
                view = (ctypes.c_char * len(data)).from_buffer(data)
                views.append(view)
                c.rom[page] = ctypes.addressof(view)
                c.rom_len[page] = len(data)
            object.__setattr__(self, "_views", views)

        # -- attached models ------------------------------------------
        @property
        def ctc(self):
            return self._ctc

        @ctc.setter
        def ctc(self, value):
            object.__setattr__(self, "_ctc", value)
            if value is None:
                self._c.ctc = None
                return
            if not isinstance(value, CtcBound):
                state = CtcState()
                state.vector_base = value.vector_base
                for index in range(4):
                    state.delivered[index] = value.delivered[index]
                    state.expiries[index] = value.expiries[index]
                object.__setattr__(value, "_cs", state)
                value.__class__ = CtcBound
                value.channels = [_ChannelView(state.ch[index], channel)
                                  for index, channel in
                                  enumerate(value.channels)]
                value.delivered = _CounterView(state.delivered)
                value.expiries = _CounterView(state.expiries)
            self._c.ctc = ctypes.pointer(value._cs)

        # -- execution ------------------------------------------------
        def _sync(self):
            vdp = self.vdp_peer
            self._c.py_clock = 1 if (vdp is not None and
                                     vdp.frame_period) else 0
            # An instance that wraps _accept_interrupt sees every
            # acceptance (test_timer counts them).
            self._c.py_accept = 1 if "_accept_interrupt" in \
                self.__dict__ else 0

        def _accept_interrupt(self):
            err = lib.core_accept(ctypes.byref(self._c))
            if err:
                self._raise(err)

        def _raise(self, err):
            c = self._c
            if err == CORE_PYERR:
                exc = self._exc
                object.__setattr__(self, "_exc", None)
                raise exc
            if err == CORE_EXIT:
                raise SystemExit(c.msg.decode("ascii"))
            if err == CORE_KEYERR:
                raise KeyError(c.err_arg)
            raise IndexError("bytearray index out of range")

        def step(self):
            self._sync()
            err = lib.core_step(ctypes.byref(self._c))
            if err:
                self._raise(err)

        def _clock(self, tstates):
            self._sync()
            self._c.err = CORE_OK
            lib.core_clock(ctypes.byref(self._c), tstates)
            if self._c.err:
                self._raise(self._c.err)

        def run(self, limit, stops=None, delay=None, check_first=True,
                out_port=None, halt=False):
            """Up to `limit` iterations of: a stop check (PC in the
            StopSet `stops`; not on the first iteration unless
            `check_first`), the serial-connect skip (`delay` = (address,
            latch), Mk1 only), then one step.  After the step the run also
            ends when it wrote `out_port`, or when `halt` is set and the CPU
            is halted.  Returns (reason, iterations done): "limit", "stop"
            (before that iteration), "out" or "halt" (after it)."""
            self._sync()
            c = self._c
            done = ctypes.c_int64(0)
            delay_addr, delay_latch = delay if delay else (-1, 0)
            c.stop_out_port = -1 if out_port is None else out_port
            reason = lib.core_run(ctypes.byref(c), limit,
                                  stops.bits if stops is not None else None,
                                  1 if check_first else 0, delay_addr,
                                  delay_latch, 1 if halt else 0,
                                  ctypes.byref(done))
            c.stop_out_port = -1
            if reason < 0:
                self._raise(c.err)
            return RUN_REASONS[reason], done.value

    for name in REGISTERS + COUNTERS:
        setattr(CZ80, name, _register(name))
    CZ80.latch = _mk1_only(CZ80.latch)
    for name in FLAGS:
        setattr(CZ80, name, _flag(name))
    for name in ("cycles", "tstates", "nmi_pending", "rt_hz", "rt_acc",
                 "rt_ticks", "nmi_count"):
        setattr(CZ80, name, _register(name))
    return CZ80

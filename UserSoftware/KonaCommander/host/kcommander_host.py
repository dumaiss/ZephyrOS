#!/usr/bin/env python3
"""Sandboxed KCP1 host service for KonaCommanderKos."""

from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

MAGIC = b"KCP1"
VERSION = 1
REQUEST = 0
RESPONSE = 1
MAX_PAYLOAD = 768
MAX_PATH = 191
MAX_NAME = 63

CMD_HELLO = 0x01
CMD_LIST = 0x02
CMD_OPEN = 0x03
CMD_READ = 0x04
CMD_CLOSE = 0x05
CMD_PING = 0x06
CMD_CREATE = 0x07
CMD_WRITE = 0x08
CMD_COMMIT = 0x09
CMD_ABORT = 0x0A

CAP_WRITE = 0x01

OK = 0
BAD_REQUEST = 1
NOT_FOUND = 2
NOT_DIRECTORY = 3
NOT_FILE = 4
DENIED = 5
BAD_HANDLE = 6
NO_HANDLES = 7
IO_ERROR = 8
UNSUPPORTED = 9
TOO_LARGE = 10

COMMAND_NAMES = {
    CMD_HELLO: "HELLO", CMD_LIST: "LIST", CMD_OPEN: "OPEN",
    CMD_READ: "READ", CMD_CLOSE: "CLOSE", CMD_PING: "PING",
    CMD_CREATE: "CREATE", CMD_WRITE: "WRITE", CMD_COMMIT: "COMMIT",
    CMD_ABORT: "ABORT",
}


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = (((value << 1) ^ 0x1021) if value & 0x8000
                     else value << 1) & 0xFFFF
    return value


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\0")
    code_at = 0
    code = 1
    for byte in data:
        if byte == 0:
            output[code_at] = code
            code_at = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_at] = code
                code_at = len(output)
                output.append(0)
                code = 1
    output[code_at] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    output = bytearray()
    offset = 0
    while offset < len(data):
        code = data[offset]
        offset += 1
        if code == 0 or offset + code - 1 > len(data):
            raise ValueError("bad COBS record")
        output.extend(data[offset:offset + code - 1])
        offset += code - 1
        if code != 0xFF and offset < len(data):
            output.append(0)
    return bytes(output)


@dataclass(frozen=True)
class Frame:
    kind: int
    command: int
    sequence: int
    status: int = 0
    flags: int = 0
    payload: bytes = b""

    def pack(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError("payload too large")
        header = MAGIC + bytes((VERSION, self.kind, self.command,
                                self.sequence, self.status, self.flags))
        raw = header + struct.pack("<H", len(self.payload)) + self.payload
        return raw + struct.pack("<H", crc16(raw))

    @staticmethod
    def unpack(raw: bytes) -> "Frame":
        if len(raw) < 14 or raw[:4] != MAGIC:
            raise ValueError("bad frame header")
        if raw[4] != VERSION:
            raise ValueError("unsupported version")
        length = struct.unpack_from("<H", raw, 10)[0]
        if length > MAX_PAYLOAD or len(raw) != 14 + length:
            raise ValueError("bad frame length")
        if crc16(raw[:-2]) != struct.unpack_from("<H", raw, len(raw) - 2)[0]:
            raise ValueError("bad frame CRC")
        return Frame(raw[5], raw[6], raw[7], raw[8], raw[9], raw[12:-2])


class RequestError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


@dataclass
class OpenHandle:
    stream: BinaryIO
    writable: bool = False
    temp_path: Path | None = None
    final_path: Path | None = None
    position: int = 0


class FileService:
    """Protocol engine with no serial dependency."""

    def __init__(self, root: Path, display_name: str | None = None,
                 allow_write: bool = False):
        self.root = root.expanduser().resolve(strict=True)
        if not self.root.is_dir():
            raise ValueError("root must be a directory")
        label = display_name if display_name is not None else self.root.name
        self.display_name = label.encode("utf-8", "replace")[:63]
        self.allow_write = allow_write
        self.handles: dict[int, OpenHandle] = {}
        self.last_request: bytes | None = None
        self.last_response: bytes | None = None

    def close(self) -> None:
        for opened in self.handles.values():
            opened.stream.close()
            if opened.temp_path is not None:
                opened.temp_path.unlink(missing_ok=True)
        self.handles.clear()

    def _new_handle(self) -> int:
        handle = next((number for number in range(1, 9)
                       if number not in self.handles), None)
        if handle is None:
            raise RequestError(NO_HANDLES, "host file table full")
        return handle

    def _path(self, encoded: bytes) -> Path:
        if not encoded or len(encoded) > MAX_PATH or encoded[:1] != b"/":
            raise RequestError(BAD_REQUEST, "bad path")
        try:
            virtual = encoded.decode("utf-8")
        except UnicodeDecodeError as error:
            raise RequestError(BAD_REQUEST, "path is not UTF-8") from error
        relative = virtual.lstrip("/")
        candidate = (self.root / relative).resolve(strict=False)
        try:
            candidate.relative_to(self.root)
        except ValueError as error:
            raise RequestError(DENIED, "path escapes host root") from error
        return candidate

    @staticmethod
    def _take_path(payload: bytes, offset: int = 0) -> tuple[bytes, int]:
        if offset >= len(payload):
            raise RequestError(BAD_REQUEST, "missing path")
        length = payload[offset]
        offset += 1
        if length == 0 or length > MAX_PATH or offset + length > len(payload):
            raise RequestError(BAD_REQUEST, "bad path length")
        return payload[offset:offset + length], offset + length

    def _hello(self, payload: bytes) -> bytes:
        if payload:
            raise RequestError(BAD_REQUEST, "HELLO takes no payload")
        capabilities = CAP_WRITE if self.allow_write else 0
        return (struct.pack("<HB", MAX_PAYLOAD, len(self.display_name)) +
                self.display_name + bytes((capabilities,)))

    def _list(self, payload: bytes) -> bytes:
        encoded, offset = self._take_path(payload)
        if len(payload) != offset + 5:
            raise RequestError(BAD_REQUEST, "bad LIST payload")
        cursor, wanted = struct.unpack_from("<IB", payload, offset)
        if wanted < 1 or wanted > 32:
            raise RequestError(BAD_REQUEST, "bad LIST count")
        directory = self._path(encoded)
        if not directory.exists():
            raise RequestError(NOT_FOUND, "directory not found")
        if not directory.is_dir():
            raise RequestError(NOT_DIRECTORY, "not a directory")
        try:
            entries = []
            with os.scandir(directory) as iterator:
                for item in iterator:
                    name = item.name.encode("utf-8", "replace")
                    if not name or len(name) > MAX_NAME or b"/" in name:
                        continue
                    try:
                        resolved = Path(item.path).resolve(strict=True)
                        resolved.relative_to(self.root)
                        is_dir = item.is_dir(follow_symlinks=True)
                        size = 0 if is_dir else min(item.stat(follow_symlinks=True).st_size,
                                                    0xFFFFFFFF)
                    except (OSError, ValueError):
                        continue
                    entries.append((not is_dir, item.name.casefold(), name,
                                    is_dir, size))
            entries.sort(key=lambda row: (row[0], row[1]))
        except PermissionError as error:
            raise RequestError(DENIED, "directory access denied") from error
        except OSError as error:
            raise RequestError(IO_ERROR, str(error)) from error
        if cursor > len(entries):
            raise RequestError(BAD_REQUEST, "LIST cursor out of range")
        packed = bytearray(b"\0\0\0\0\0")
        index = cursor
        count = 0
        while index < len(entries) and count < wanted:
            _, _, name, is_dir, size = entries[index]
            record = bytes((1 if is_dir else 0,)) + struct.pack("<I", size)
            record += bytes((len(name),)) + name
            if len(packed) + len(record) > MAX_PAYLOAD:
                break
            packed.extend(record)
            index += 1
            count += 1
        next_cursor = index if index < len(entries) else 0
        struct.pack_into("<I", packed, 0, next_cursor)
        packed[4] = count
        return bytes(packed)

    def _open(self, payload: bytes) -> bytes:
        encoded, offset = self._take_path(payload)
        if offset != len(payload):
            raise RequestError(BAD_REQUEST, "bad OPEN payload")
        path = self._path(encoded)
        if not path.exists():
            raise RequestError(NOT_FOUND, "file not found")
        if not path.is_file():
            raise RequestError(NOT_FILE, "not a regular file")
        handle = self._new_handle()
        try:
            stream = path.open("rb")
            size = min(path.stat().st_size, 0xFFFFFFFF)
        except PermissionError as error:
            raise RequestError(DENIED, "file access denied") from error
        except OSError as error:
            raise RequestError(IO_ERROR, str(error)) from error
        self.handles[handle] = OpenHandle(stream)
        return bytes((handle,)) + struct.pack("<I", size)

    def _read(self, payload: bytes) -> bytes:
        if len(payload) != 7:
            raise RequestError(BAD_REQUEST, "bad READ payload")
        handle = payload[0]
        offset, wanted = struct.unpack_from("<IH", payload, 1)
        if wanted < 1 or wanted > MAX_PAYLOAD - 6:
            raise RequestError(TOO_LARGE, "bad READ count")
        opened = self.handles.get(handle)
        if opened is None or opened.writable:
            raise RequestError(BAD_HANDLE, "file handle is closed")
        try:
            opened.stream.seek(offset)
            data = opened.stream.read(wanted)
        except OSError as error:
            raise RequestError(IO_ERROR, str(error)) from error
        return struct.pack("<IH", offset, len(data)) + data

    def _close(self, payload: bytes) -> bytes:
        if len(payload) != 1:
            raise RequestError(BAD_REQUEST, "bad CLOSE payload")
        opened = self.handles.pop(payload[0], None)
        if opened is not None:
            opened.stream.close()
            if opened.temp_path is not None:
                opened.temp_path.unlink(missing_ok=True)
        return b""

    def _create(self, payload: bytes) -> bytes:
        if not self.allow_write:
            raise RequestError(DENIED, "host writes disabled; restart with --write")
        encoded, offset = self._take_path(payload)
        if offset != len(payload):
            raise RequestError(BAD_REQUEST, "bad CREATE payload")
        final_path = self._path(encoded)
        parent = final_path.parent
        if not parent.exists():
            raise RequestError(NOT_FOUND, "destination directory not found")
        if not parent.is_dir():
            raise RequestError(NOT_DIRECTORY, "destination parent is not a directory")
        if final_path.exists() and not final_path.is_file():
            raise RequestError(NOT_FILE, "destination is not a regular file")
        handle = self._new_handle()
        try:
            temporary = tempfile.NamedTemporaryFile(
                mode="w+b", prefix=".kcommander-", suffix=".tmp",
                dir=parent, delete=False)
        except PermissionError as error:
            raise RequestError(DENIED, "destination is not writable") from error
        except OSError as error:
            raise RequestError(IO_ERROR, str(error)) from error
        temp_path = Path(temporary.name)
        self.handles[handle] = OpenHandle(
            temporary, True, temp_path, final_path, 0)
        return bytes((handle,))

    def _write(self, payload: bytes) -> bytes:
        if len(payload) < 7:
            raise RequestError(BAD_REQUEST, "bad WRITE payload")
        handle = payload[0]
        offset, count = struct.unpack_from("<IH", payload, 1)
        if count < 1 or count > MAX_PAYLOAD - 7 or len(payload) != 7 + count:
            raise RequestError(TOO_LARGE, "bad WRITE count")
        opened = self.handles.get(handle)
        if opened is None or not opened.writable:
            raise RequestError(BAD_HANDLE, "write handle is closed")
        if offset != opened.position:
            raise RequestError(BAD_REQUEST, "WRITE offset is not sequential")
        try:
            written = opened.stream.write(payload[7:])
        except OSError as error:
            raise RequestError(IO_ERROR, str(error)) from error
        if written != count:
            raise RequestError(IO_ERROR, "short host write")
        opened.position += written
        return struct.pack("<IH", offset, written)

    def _commit(self, payload: bytes) -> bytes:
        if len(payload) != 1:
            raise RequestError(BAD_REQUEST, "bad COMMIT payload")
        opened = self.handles.pop(payload[0], None)
        if (opened is None or not opened.writable or opened.temp_path is None or
                opened.final_path is None):
            if opened is not None:
                self.handles[payload[0]] = opened
            raise RequestError(BAD_HANDLE, "write handle is closed")
        try:
            opened.stream.flush()
            os.fsync(opened.stream.fileno())
            opened.stream.close()
            os.replace(opened.temp_path, opened.final_path)
        except PermissionError as error:
            opened.stream.close()
            opened.temp_path.unlink(missing_ok=True)
            raise RequestError(DENIED, "destination replace denied") from error
        except OSError as error:
            opened.stream.close()
            opened.temp_path.unlink(missing_ok=True)
            raise RequestError(IO_ERROR, str(error)) from error
        return b""

    def _abort(self, payload: bytes) -> bytes:
        if len(payload) != 1:
            raise RequestError(BAD_REQUEST, "bad ABORT payload")
        opened = self.handles.pop(payload[0], None)
        if opened is not None:
            opened.stream.close()
            if opened.temp_path is not None:
                opened.temp_path.unlink(missing_ok=True)
        return b""

    def dispatch(self, request: Frame) -> Frame:
        if request.kind != REQUEST or request.status != 0 or request.flags != 0:
            raise RequestError(BAD_REQUEST, "bad request header")
        operations = {
            CMD_HELLO: self._hello,
            CMD_LIST: self._list,
            CMD_OPEN: self._open,
            CMD_READ: self._read,
            CMD_CLOSE: self._close,
            CMD_CREATE: self._create,
            CMD_WRITE: self._write,
            CMD_COMMIT: self._commit,
            CMD_ABORT: self._abort,
            CMD_PING: lambda payload: payload if len(payload) <= 32 else
            (_ for _ in ()).throw(RequestError(TOO_LARGE, "PING too large")),
        }
        operation = operations.get(request.command)
        if operation is None:
            raise RequestError(UNSUPPORTED, "unsupported command")
        payload = operation(request.payload)
        return Frame(RESPONSE, request.command, request.sequence, OK, 0, payload)

    def process(self, decoded_request: bytes) -> bytes:
        if decoded_request == self.last_request and self.last_response is not None:
            return self.last_response
        try:
            request = Frame.unpack(decoded_request)
            try:
                response = self.dispatch(request)
            except RequestError as error:
                message = str(error).encode("utf-8", "replace")[:96]
                response = Frame(RESPONSE, request.command, request.sequence,
                                 error.status, 0, message)
        except ValueError:
            raise
        packed = response.pack()
        self.last_request = decoded_request
        self.last_response = packed
        return packed


def write_paced(writer: BinaryIO, data: bytes, pace_seconds: float) -> None:
    """Write target-bound bytes slowly enough for KonaOS's polled SIO API.

    The application must leave and re-enter the SERIAL driver between reads;
    an unpaced 115200-baud burst overruns the Z80 SIO's small receive FIFO.
    """
    if pace_seconds <= 0:
        writer.write(data)
        writer.flush()
        return
    for byte in data:
        writer.write(bytes((byte,)))
        writer.flush()
        time.sleep(pace_seconds)


def serve_stream(reader: BinaryIO, writer: BinaryIO, service: FileService,
                 pace_seconds: float = 0.0, trace: bool = False) -> None:
    encoded = bytearray()
    while True:
        chunk = reader.read(1)
        if not chunk:
            return
        byte = chunk[0]
        if byte == 0:
            if not encoded:
                continue
            try:
                request = cobs_decode(bytes(encoded))
                request_frame = Frame.unpack(request)
                if trace:
                    print(f"KCP1 RX {COMMAND_NAMES.get(request_frame.command, 'UNKNOWN')} "
                          f"seq={request_frame.sequence} bytes={len(request_frame.payload)}",
                          file=sys.stderr)
                response = service.process(request)
                response_frame = Frame.unpack(response)
                wire_response = cobs_encode(response) + b"\0"
                write_paced(writer, wire_response, pace_seconds)
                if trace:
                    print(f"KCP1 TX {COMMAND_NAMES.get(response_frame.command, 'UNKNOWN')} "
                          f"seq={response_frame.sequence} status={response_frame.status} "
                          f"wire={len(wire_response)}", file=sys.stderr)
            except ValueError as error:
                print(f"KCP1 dropped frame: {error}", file=sys.stderr)
            encoded.clear()
        elif len(encoded) < 900:
            encoded.append(byte)
        else:
            encoded.clear()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path,
                        help="host directory exported as virtual /")
    parser.add_argument("--name", help="short name shown in KonaCommander")
    parser.add_argument("--port", help="serial device, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--pace-ms", type=float, default=4.0,
                        help="delay between target-bound bytes (default: 4.0)")
    parser.add_argument("--trace", action="store_true",
                        help="log every valid KCP1 request and response")
    parser.add_argument("--write", action="store_true",
                        help="allow atomic file uploads into the exported root")
    parser.add_argument("--stdio", action="store_true",
                        help="use stdin/stdout as a binary KCP1 stream")
    args = parser.parse_args()
    if args.stdio == bool(args.port):
        parser.error("select exactly one of --port or --stdio")
    if args.pace_ms < 0:
        parser.error("--pace-ms cannot be negative")
    service = FileService(args.root, args.name, args.write)
    try:
        if args.stdio:
            serve_stream(sys.stdin.buffer, sys.stdout.buffer, service,
                         0.0, args.trace)
        else:
            try:
                import serial
            except ImportError as error:
                raise SystemExit("pyserial is required for --port mode") from error
            with serial.Serial(port=args.port, baudrate=args.baud,
                               bytesize=serial.EIGHTBITS,
                               parity=serial.PARITY_NONE,
                               stopbits=serial.STOPBITS_ONE,
                               timeout=None, write_timeout=2,
                               xonxoff=False, rtscts=False, dsrdtr=False) as device:
                device.reset_input_buffer()
                access = "read/write" if args.write else "read-only"
                print(f"KonaCommander: {service.root} ({access}) on {args.port} "
                      f"at {args.baud} 8N1, TX pace {args.pace_ms:g} ms",
                      file=sys.stderr)
                serve_stream(device, device, service,
                             args.pace_ms / 1000.0, args.trace)
    except KeyboardInterrupt:
        return 0
    finally:
        service.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


#!/usr/bin/env python3
"""Convert audio to the KonaOS PCM v1 format (docs/snd-sdk.md section 12)
and back.

    pcmconv.py IN.wav|IN.mp3|... [-o OUT.pcm] [--codec u8|dpcm4]
               [--title TEXT] [--rate HZ] [--gain DB | --normalize]
               [--dither tpdf|none] [--seed N]
    pcmconv.py IN.pcm -o OUT.pcm --codec u8|dpcm4   (re-encode, exact)
    pcmconv.py --info FILE.pcm
    pcmconv.py --to-wav FILE.pcm [-o OUT.wav]

Codec u8 stores the 8-bit samples as they are; codec dpcm4 stores them
losslessly as 4-bit deltas in 256-sample blocks, about half the size (a
block that would not shrink is stored raw).

Decoding, down-mixing to mono (the channels' average) and resampling
(with its anti-alias low-pass) are FFmpeg's, run through subprocess and streamed as 32-bit
float mono at the target rate; the 8-bit quantization (gain, TPDF dither,
clipping) is done here, chunk by chunk, so a song is never held in
memory.  Without FFmpeg, a PCM WAV whose rate already equals the target
is still converted with the standard library alone.

The dither generator is seeded (--seed, default 1), so one input and one
set of options always produce the same bytes.

Exit status: 0 on success, 1 for a conversion error, 2 for bad usage.
"""

import argparse
import array
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import wave

MAGIC = b"KPCM"
VERSION = 1
CODEC_U8 = 0
CODEC_DPCM4 = 1
CODECS = {"u8": CODEC_U8, "dpcm4": CODEC_DPCM4}
BLOCK = 256              # samples per DPCM4 block (one ring page)
BLOCK_RAW = 0
BLOCK_DPCM4 = 1
HEADER_SIZE = 512
NAME_OFFSET = 32
NAME_SIZE = 64
DEFAULT_RATE = 16000
RATE_MIN = 4000          # the player's range (kos/snd.h SND_PCM_RATE_*)
RATE_MAX = 24000
CHUNK = 16384            # samples per processing chunk


class ConvError(Exception):
    pass


def encode_title(title):
    """The title in CP850, the KonaOS console's character set, cut to 63
    bytes.  Returns (bytes, whether a character had no CP850 form)."""
    name = title.encode("cp850", "replace")[:NAME_SIZE - 1]
    try:
        title.encode("cp850")
        return name, False
    except UnicodeEncodeError:
        return name, True


def pack_header(rate, samples, title, codec=CODEC_U8, payload=None):
    """The 512-byte v1 header."""
    name, _lossy = encode_title(title)
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4sBBBBIIIIHHI", header, 0, MAGIC, VERSION, codec,
                     1, 8, rate, samples,
                     samples if payload is None else payload,
                     HEADER_SIZE, HEADER_SIZE, 0, 0)
    header[NAME_OFFSET:NAME_OFFSET + len(name)] = name
    return bytes(header)


def parse_header(raw):
    """dict of the header fields; ConvError when it is not PCM v1."""
    if len(raw) < HEADER_SIZE:
        raise ConvError("file is shorter than the 512-byte header")
    (magic, version, codec, channels, bits, rate, samples, payload,
     offset, size, flags, _reserved) = struct.unpack_from(
         "<4sBBBBIIIIHHI", raw, 0)
    if magic != MAGIC:
        raise ConvError("not a KonaOS PCM file (magic %r)" % magic)
    if (version, channels, bits) != (VERSION, 1, 8) or \
            codec not in CODECS.values():
        raise ConvError("unsupported PCM v%d codec %d, %d ch, %d bit"
                        % (version, codec, channels, bits))
    if (codec == CODEC_U8 and payload != samples) or \
            offset != HEADER_SIZE or size != HEADER_SIZE:
        raise ConvError("inconsistent header (payload %d, samples %d, "
                        "offset %d, size %d)"
                        % (payload, samples, offset, size))
    field = raw[NAME_OFFSET:NAME_OFFSET + NAME_SIZE]
    if b"\0" not in field:
        raise ConvError("song name is not NUL-terminated")
    name = field.split(b"\0", 1)[0].decode("cp850")
    return {"rate": rate, "samples": samples, "payload": payload,
            "offset": offset, "flags": flags, "name": name, "codec": codec}


def encode_block(block, pred):
    """One block of 256 samples (bytes or ints) after the predictor `pred`:
    the stored block.  A DPCM4 block is kind 1, its body length - 1, then
    nibbles high first: n != 8 adds (n ^ 8) - 8, 8 escapes to the sample's
    two nibbles.  A block whose body would not be smaller than the raw
    block's 256 bytes is stored raw (kind 0, the 256 bytes)."""
    nibbles = []
    for sample in block:
        delta = (sample - pred) & 0xFF
        if delta <= 7 or delta >= 0xF9:
            nibbles.append(delta & 0x0F)
        else:
            nibbles.extend((8, sample >> 4, sample & 0x0F))
        pred = sample
    if len(nibbles) & 1:
        nibbles.append(0)
    if len(nibbles) // 2 > 254:
        return bytes([BLOCK_RAW]) + bytes(block)
    body = bytes((nibbles[i] << 4) | nibbles[i + 1]
                 for i in range(0, len(nibbles), 2))
    return bytes([BLOCK_DPCM4, len(body) - 1]) + body


def decode_block(data, at, pred):
    """The block at data[at]: (256 samples, the offset after it)."""
    kind = data[at]
    if kind == BLOCK_RAW:
        return bytes(data[at + 1:at + 1 + BLOCK]), at + 1 + BLOCK
    if kind != BLOCK_DPCM4:
        raise ConvError("unknown DPCM4 block kind %d at %d" % (kind, at))
    length = data[at + 1] + 1
    body = data[at + 2:at + 2 + length]
    out = bytearray()
    nib = 0

    def take():
        nonlocal nib
        byte = body[nib >> 1]
        value = (byte >> 4) if (nib & 1) == 0 else (byte & 0x0F)
        nib += 1
        return value

    while len(out) < BLOCK:
        n = take()
        if n == 8:
            pred = (take() << 4) | take()
        else:
            pred = (pred + (n ^ 8) - 8) & 0xFF
        out.append(pred)
    return bytes(out), at + 2 + length


class Dpcm4Writer:
    """Samples in, DPCM4 blocks out, in 256-sample blocks; the last one is
    padded with its final sample."""

    def __init__(self, handle):
        self.handle = handle
        self.pending = bytearray()
        self.pred = 0x80
        self.size = 0
        self.raw_blocks = 0
        self.blocks = 0

    def write(self, data):
        self.pending.extend(data)
        while len(self.pending) >= BLOCK:
            self._block(bytes(self.pending[:BLOCK]))
            del self.pending[:BLOCK]

    def _block(self, block):
        stored = encode_block(block, self.pred)
        self.pred = block[-1]
        self.blocks += 1
        self.raw_blocks += stored[0] == BLOCK_RAW
        self.handle.write(stored)
        self.size += len(stored)

    def close(self):
        if self.pending:
            last = self.pending[-1]
            self._block(bytes(self.pending) +
                        bytes([last]) * (BLOCK - len(self.pending)))
            self.pending = bytearray()


class U8Writer:
    def __init__(self, handle):
        self.handle = handle
        self.size = 0

    def write(self, data):
        self.handle.write(data)
        self.size += len(data)

    def close(self):
        pass


def payload_samples(handle, info):
    """The stream's samples, chunk by chunk, whatever its codec."""
    left = info["samples"]
    handle.seek(HEADER_SIZE)
    if info["codec"] == CODEC_U8:
        while left:
            data = handle.read(min(left, 65536))
            if not data:
                raise ConvError("the payload ends early")
            left -= len(data)
            yield data
        return
    data = handle.read(info["payload"])
    at = 0
    pred = 0x80
    while left:
        if at >= len(data):
            raise ConvError("the payload ends early")
        block, at = decode_block(data, at, pred)
        pred = block[-1]
        block = block[:min(left, BLOCK)]
        left -= len(block)
        yield block


class Quantizer:
    """Float [-1, 1) samples to unsigned 8-bit: gain, TPDF dither of one
    LSB peak each way, rounding, clipping.  Counts the clipped samples."""

    def __init__(self, gain, dither, seed):
        self.scale = 128.0 * gain
        self.dither = dither
        self.rng = random.Random(seed)
        self.clipped = 0
        self.peak = 0.0

    def __call__(self, floats):
        out = bytearray(len(floats))
        scale = self.scale
        rnd = self.rng.random
        dither = self.dither == "tpdf"
        peak = self.peak
        clipped = 0
        for i, x in enumerate(floats):
            ax = -x if x < 0 else x
            if ax > peak:
                peak = ax
            v = x * scale + 128.0
            if dither:
                v += rnd() - rnd()
            q = math.floor(v + 0.5)
            if q < 0:
                q = 0
                clipped += 1
            elif q > 255:
                q = 255
                clipped += 1
            out[i] = q
        self.peak = peak
        self.clipped += clipped
        return out


def have_ffmpeg():
    return shutil.which("ffmpeg") is not None


def ffmpeg_resampler():
    """The aresample options: SoX's resampler when this FFmpeg has it,
    else swr with a longer filter than its default."""
    try:
        conf = subprocess.run(["ffmpeg", "-hide_banner", "-buildconf"],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, check=False).stdout
    except OSError:
        conf = b""
    if b"libsoxr" in conf:
        return "resampler=soxr:precision=28"
    return "filter_size=64:cutoff=0.95"


def ffmpeg_stream(path, rate):
    """Yield float sample chunks: `path` decoded, mono, at `rate`."""
    command = ["ffmpeg", "-v", "error", "-nostdin", "-i", path, "-vn",
               # One resampler does both: the rate (with its low-pass)
               # and the down-mix, which rematrix_maxval=1 makes a
               # plain average (FFmpeg's default sums at -3 dB, so a
               # centred stereo signal would clip).
               "-af", "aresample=%d:%s:ochl=mono:rematrix_maxval=1.0"
               % (rate, ffmpeg_resampler()),
               "-ac", "1", "-ar", str(rate), "-f", "f32le",
               "-acodec", "pcm_f32le", "-"]
    try:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
    except OSError as error:
        raise ConvError("cannot run ffmpeg: %s" % error)
    pending = b""
    try:
        while True:
            data = proc.stdout.read(CHUNK * 4)
            if not data:
                break
            data = pending + data
            whole = len(data) - (len(data) % 4)
            pending = data[whole:]
            floats = array.array("f")
            floats.frombytes(data[:whole])
            if sys.byteorder != "little":
                floats.byteswap()
            yield floats
    finally:
        proc.stdout.close()
        err = proc.stderr.read().decode("utf-8", "replace").strip()
        code = proc.wait()
    if code != 0:
        raise ConvError("ffmpeg could not decode %s: %s"
                        % (path, err.splitlines()[-1] if err else
                           "exit status %d" % code))


def wav_stream(path, rate):
    """Yield float chunks from an integer PCM WAV at `rate`, down-mixed by
    averaging its channels (standard library only)."""
    try:
        reader = wave.open(path, "rb")
    except (wave.Error, EOFError) as error:
        raise ConvError("%s is not a readable PCM WAV: %s" % (path, error))
    with reader:
        channels = reader.getnchannels()
        width = reader.getsampwidth()
        if reader.getframerate() != rate:
            raise ConvError("%s is %d Hz; resampling to %d Hz needs ffmpeg"
                            % (path, reader.getframerate(), rate))
        if width not in (1, 2, 3, 4):
            raise ConvError("unsupported WAV sample width %d" % width)
        full = float(1 << (8 * width - 1))
        while True:
            frames = reader.readframes(CHUNK)
            if not frames:
                break
            count = len(frames) // (width * channels)
            out = array.array("f", bytes(4 * count))
            for i in range(count):
                total = 0
                base = i * width * channels
                for c in range(channels):
                    at = base + c * width
                    if width == 1:
                        value = frames[at] - 128
                    else:
                        value = int.from_bytes(frames[at:at + width],
                                               "little", signed=True)
                    total += value
                out[i] = total / (channels * full)
            yield out


def source_stream(path, rate):
    if not os.path.isfile(path):
        raise ConvError("%s: no such file" % path)
    if have_ffmpeg():
        return ffmpeg_stream(path, rate)
    if path.lower().endswith(".wav"):
        return wav_stream(path, rate)
    raise ConvError("decoding %s needs ffmpeg, which is not installed"
                    % os.path.basename(path))


def measure_peak(path, rate):
    peak = 0.0
    for floats in source_stream(path, rate):
        for x in floats:
            ax = -x if x < 0 else x
            if ax > peak:
                peak = ax
    return peak


def convert(src, dst, title, rate, gain_db, normalize, dither, seed,
            out=sys.stdout, codec=CODEC_U8):
    if not RATE_MIN <= rate <= RATE_MAX:
        raise ConvError("rate %d Hz is outside the player's %d-%d Hz"
                        % (rate, RATE_MIN, RATE_MAX))
    if encode_title(title)[1]:
        sys.stderr.write("pcmconv: warning: the console's CP850 cannot show "
                         "all of %r; stored as %r (use --title)\n"
                         % (title, encode_title(title)[0].decode("cp850")))
    gain = 10.0 ** (gain_db / 20.0)
    if normalize:
        peak = measure_peak(src, rate)
        if peak > 0.0:
            # Leave room for rounding plus the dither's one-LSB peak
            # on both sides of the 0-255 range.
            gain = (126.0 / 128.0) / peak
    quantize = Quantizer(gain, dither, seed)
    samples = 0
    tmp = dst + ".part"
    try:
        with open(tmp, "wb") as handle:
            handle.write(bytes(HEADER_SIZE))
            writer = (Dpcm4Writer if codec == CODEC_DPCM4 else U8Writer)(handle)
            for floats in source_stream(src, rate):
                writer.write(quantize(floats))
                samples += len(floats)
            writer.close()
            if samples == 0:
                raise ConvError("%s decoded to no samples" % src)
            handle.seek(0)
            handle.write(pack_header(rate, samples, title, codec,
                                     writer.size))
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    seconds = samples / float(rate)
    peak_db = (20.0 * math.log10(quantize.peak * gain)
               if quantize.peak > 0.0 else float("-inf"))
    out.write("%s -> %s\n" % (src, dst))
    out.write("  title     %s\n" % encode_title(title)[0].decode("cp850"))
    out.write("  duration  %d:%06.3f (%d samples)\n"
              % (int(seconds // 60), seconds % 60.0, samples))
    out.write("  rate      %d Hz\n" % rate)
    out.write("  output    %d bytes, codec %s (%.0f%% of the samples)\n"
              % (os.path.getsize(dst), codec_name(codec),
                 100.0 * writer.size / samples))
    out.write("  level     peak %.1f dBFS, gain %.1f dB, %d clipped, "
              "dither %s\n"
              % (peak_db, 20.0 * math.log10(gain), quantize.clipped, dither))
    return samples


def codec_name(codec):
    return [name for name, value in CODECS.items() if value == codec][0]


def recode(src, dst, codec, out=sys.stdout):
    """Re-encode a .pcm file's samples in `codec` (exact: both codecs are
    lossless)."""
    info = read_pcm(src)
    tmp = dst + ".part"
    try:
        with open(src, "rb") as reader, open(tmp, "wb") as handle:
            handle.write(bytes(HEADER_SIZE))
            writer = (Dpcm4Writer if codec == CODEC_DPCM4 else U8Writer)(handle)
            for chunk in payload_samples(reader, info):
                writer.write(chunk)
            writer.close()
            handle.seek(0)
            handle.write(pack_header(info["rate"], info["samples"],
                                     info["name"], codec, writer.size))
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    out.write("%s -> %s: codec %s, %d bytes (%.0f%% of the samples)\n"
              % (src, dst, codec_name(codec), writer.size,
                 100.0 * writer.size / max(1, info["samples"])))


def is_pcm(path):
    try:
        with open(path, "rb") as handle:
            return handle.read(4) == MAGIC
    except OSError:
        return False


def read_pcm(path):
    with open(path, "rb") as handle:
        info = parse_header(handle.read(HEADER_SIZE))
        size = os.fstat(handle.fileno()).st_size
    if size < HEADER_SIZE + info["payload"]:
        raise ConvError("%s is %d bytes; its header promises %d"
                        % (path, size, HEADER_SIZE + info["payload"]))
    return info


def info_command(path, out=sys.stdout):
    info = read_pcm(path)
    seconds = info["samples"] / float(info["rate"])
    out.write("%s: \"%s\" %d:%06.3f, %d samples, %d Hz, codec %s, "
              "%d bytes\n"
              % (path, info["name"], int(seconds // 60), seconds % 60.0,
                 info["samples"], info["rate"], codec_name(info["codec"]),
                 info["payload"]))
    return info


def to_wav(src, dst, out=sys.stdout):
    info = read_pcm(src)
    with open(src, "rb") as handle, wave.open(dst, "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(1)
        writer.setframerate(info["rate"])
        for data in payload_samples(handle, info):
            writer.writeframes(data)
    out.write("%s -> %s (%d samples, %d Hz, unsigned 8-bit)\n"
              % (src, dst, info["samples"], info["rate"]))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert audio to KonaOS PCM v1 (8-bit mono).")
    parser.add_argument("input")
    parser.add_argument("-o", "--output")
    parser.add_argument("--title", help="song name (default: file name)")
    parser.add_argument("--rate", type=int, default=DEFAULT_RATE,
                        help="sample rate in Hz (default 16000)")
    level = parser.add_mutually_exclusive_group()
    level.add_argument("--gain", type=float, default=0.0,
                       help="gain in dB before quantization")
    level.add_argument("--normalize", action="store_true",
                       help="scale the peak to full scale (two passes)")
    parser.add_argument("--dither", choices=("tpdf", "none"), default="tpdf")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--codec", choices=sorted(CODECS), default="u8",
                        help="u8 (default) or dpcm4 (lossless, ~half)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--info", action="store_true",
                      help="print a .pcm file's header")
    mode.add_argument("--to-wav", action="store_true",
                      help="export a .pcm file as unsigned 8-bit WAV")
    args = parser.parse_args(argv)
    try:
        if args.info:
            info_command(args.input)
        elif args.to_wav:
            to_wav(args.input,
                   args.output or os.path.splitext(args.input)[0] + ".wav")
        elif is_pcm(args.input):
            if not args.output:
                raise ConvError("re-encoding a .pcm file needs -o")
            recode(args.input, args.output, CODECS[args.codec])
        else:
            stem = os.path.splitext(os.path.basename(args.input))[0]
            convert(args.input,
                    args.output or os.path.splitext(args.input)[0] + ".pcm",
                    args.title if args.title is not None else stem,
                    args.rate, args.gain, args.normalize, args.dither,
                    args.seed, codec=CODECS[args.codec])
    except ConvError as error:
        sys.stderr.write("pcmconv: %s\n" % error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

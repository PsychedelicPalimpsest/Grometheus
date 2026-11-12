"""Extract selected vendor driver files from a ROM dump without unpacking super."""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import io
from pathlib import Path
import re
import shutil
import struct
import subprocess
from typing import BinaryIO, Iterator
import zipfile


SPARSE_MAGIC = 0xED26FF3A
RAW, FILL, SKIP, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
CHUNK = 1024 * 1024
SUPER_PART = re.compile(r"super(?:\.(\d+)\.img|\.img_sparsechunk\.(\d+))$")
EXTENT = re.compile(r"^\s*(\d+) \.\. (\d+) linear (\S+) (\d+)\s*$")


class RomError(ValueError):
    pass


@dataclass(frozen=True)
class Member:
    name: str
    size: int
    path: Path | None = None


class RomDump:
    def __init__(self, source: Path):
        self.source = source.expanduser().resolve()
        self.archive = zipfile.ZipFile(self.source) if zipfile.is_zipfile(self.source) else None
        if self.archive:
            self.members = [Member(info.filename, info.file_size) for info in self.archive.infolist()
                            if not info.is_dir()]
        elif self.source.is_dir():
            self.members = [Member(str(path.relative_to(self.source)), path.stat().st_size, path)
                            for path in self.source.rglob('*') if path.is_file() and not path.is_symlink()]
        else:
            raise RomError(f"ROM dump must be a ZIP archive or directory: {source}")

    def close(self) -> None:
        if self.archive:
            self.archive.close()

    def __enter__(self) -> RomDump:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    @contextmanager
    def open(self, member: Member) -> Iterator[BinaryIO]:
        if self.archive:
            with self.archive.open(member.name) as stream:
                yield stream
        else:
            assert member.path
            with member.path.open('rb') as stream:
                yield stream

    def unique(self, basename: str) -> Member | None:
        matches = [m for m in self.members if Path(m.name).name == basename]
        if len(matches) > 1:
            raise RomError(f"ambiguous {basename} in {self.source}")
        return matches[0] if matches else None

    def boot_image(self, directory: Path) -> Path:
        for name in ('boot.img', 'boot_a.img', 'init_boot.img', 'init_boot_a.img'):
            member = self.unique(name)
            if member:
                if member.path:
                    return member.path
                directory.mkdir(parents=True, exist_ok=True)
                for sibling in (member, self.unique('init_boot.img'), self.unique('init_boot_a.img')):
                    if sibling:
                        with self.open(sibling) as source, (directory / Path(sibling.name).name).open('wb') as target:
                            shutil.copyfileobj(source, target, CHUNK)
                return directory / name
        raise RomError(f"no boot.img or init_boot.img in {self.source}")

    def super_members(self) -> list[Member]:
        whole = self.unique('super.img')
        if whole:
            return [whole]
        found: dict[int, Member] = {}
        for member in self.members:
            match = SUPER_PART.fullmatch(Path(member.name).name)
            if match:
                index = int(match.group(1) or match.group(2))
                if index in found:
                    raise RomError(f"duplicate super chunk {index}")
                found[index] = member
        if not found:
            raise RomError("no super image or vendor image in ROM dump")
        if sorted(found) != list(range(len(found))):
            raise RomError("missing or noncontiguous super image chunk")
        return [found[i] for i in range(len(found))]


def read_exact(stream: BinaryIO, count: int) -> bytes:
    value = stream.read(count)
    if len(value) != count:
        raise RomError("truncated super image")
    return value


def sparse_header(stream: BinaryIO) -> tuple[int, int, int, int]:
    header = read_exact(stream, 28)
    magic, major, minor, file_header, chunk_header, block, total_blocks, chunks, _ = struct.unpack('<IHHHHIIII', header)
    if magic != SPARSE_MAGIC or major != 1 or file_header < 28 or chunk_header < 12 or not block or block % 512:
        raise RomError("invalid Android sparse image header")
    if file_header > 4096 or chunk_header > 4096:
        raise RomError("oversized sparse header")
    read_exact(stream, file_header - 28)
    return block, total_blocks, chunks, chunk_header


def copy_intersection(stream: BinaryIO, destination: BinaryIO, source_start: int,
                      length: int, wanted: list[tuple[int, int, int]],
                      *, fill: bytes | None = None) -> None:
    """wanted entries are (super start, super end, vendor destination offset)."""
    consumed = 0
    for begin, end, output_start in sorted(wanted):
        left, right = max(begin, source_start), min(end, source_start + length)
        if left >= right:
            continue
        destination.seek(output_start + left - begin)
        if fill is None:
            stream.seek(left - source_start - consumed, io.SEEK_CUR)
            remaining = right - left
            while remaining:
                data = read_exact(stream, min(CHUNK, remaining))
                destination.write(data)
                remaining -= len(data)
            consumed = right - source_start
        else:
            remaining = right - left
            rotated = fill[(left - source_start) % 4:] + fill[:(left - source_start) % 4]
            pattern = rotated * (CHUNK // len(fill))
            while remaining:
                data = pattern[:min(len(pattern), remaining)]
                destination.write(data)
                remaining -= len(data)
    if fill is None:
        stream.seek(length - consumed, io.SEEK_CUR)


def copy_sparse(stream: BinaryIO, output: BinaryIO, wanted: list[tuple[int, int, int]],
                base: int = 0) -> int:
    block, blocks, chunks, header_size = sparse_header(stream)
    logical = 0
    max_end = max(end for _, end, _ in wanted)
    for _ in range(chunks):
        kind, _, chunk_blocks, total_size = struct.unpack('<HHII', read_exact(stream, 12))
        read_exact(stream, header_size - 12)
        length = chunk_blocks * block
        payload = total_size - header_size
        if payload < 0 or logical + length > blocks * block:
            raise RomError("invalid sparse chunk length")
        if kind == RAW and payload == length:
            if base + logical >= max_end:
                break
            copy_intersection(stream, output, base + logical, length, wanted)
        elif kind == FILL and payload == 4:
            pattern = read_exact(stream, 4)
            if base + logical < max_end:
                copy_intersection(stream, output, base + logical, length, wanted, fill=pattern)
        elif kind == SKIP and payload == 0:
            pass
        elif kind == CRC and payload == 4 and length == 0:
            read_exact(stream, 4)
        else:
            raise RomError(f"unsupported sparse chunk type or size {kind:#x}")
        logical += length
        if base + logical >= max_end:
            break
    return blocks * block


def parse_lpdump(output: str, slot: str | None = None) -> tuple[str, list[tuple[int, int, int]]]:
    partitions: dict[str, list[tuple[int, int, int]]] = {}
    current = None
    for line in output.splitlines():
        if line.startswith('  Name: '):
            current = line.split(': ', 1)[1]
            partitions[current] = []
        elif line.startswith('------------------------'):
            current = None
        elif current:
            match = EXTENT.match(line)
            if match:
                start, end, device, physical = match.groups()
                if device != 'super' or int(end) < int(start):
                    raise RomError(f"unsupported vendor extent: {line.strip()}")
                partitions[current].append((int(start) * 512, (int(end) + 1) * 512, int(physical) * 512))
    choices = [(name, extents) for name, extents in partitions.items()
               if name in ('vendor', 'vendor_a', 'vendor_b') and extents]
    if slot:
        choices = [(name, extents) for name, extents in choices if name == f'vendor_{slot}']
    if len(choices) != 1:
        raise RomError(f"expected one populated vendor partition, found {[name for name, _ in choices]}; use --vendor-slot if needed")
    name, extents = choices[0]
    wanted = []
    size = 0
    for logical_start, logical_end, physical_start in extents:
        if logical_start != size:
            raise RomError("vendor extents have a gap or overlap")
        length = logical_end - logical_start
        wanted.append((physical_start, physical_start + length, logical_start))
        size = logical_end
    return name, wanted


def _prefix(rom: RomDump, member: Member, output: Path) -> None:
    with rom.open(member) as source, output.open('wb') as target:
        magic = read_exact(source, 4)
        source.seek(0)
        if struct.unpack('<I', magic)[0] == SPARSE_MAGIC:
            # Metadata is near the beginning; do not materialize the full super image.
            copy_sparse(source, target, [(0, 16 * CHUNK, 0)])
        else:
            target.write(source.read(16 * CHUNK))
        target.truncate(16 * CHUNK)


def _logical_span(rom: RomDump, member: Member) -> int:
    with rom.open(member) as source:
        magic = read_exact(source, 4)
        source.seek(0)
        if struct.unpack('<I', magic)[0] == SPARSE_MAGIC:
            block, blocks, _, _ = sparse_header(source)
            return block * blocks
    return member.size


def vendor_image(rom: RomDump, directory: Path, slot: str | None = None) -> Path:
    if slot not in (None, 'a', 'b'):
        raise RomError('vendor slot must be a or b')
    direct = rom.unique('vendor.img')
    if not direct:
        choices = [m for name in ('vendor_a.img', 'vendor_b.img')
                   if (m := rom.unique(name)) and m.size > 0]
        if slot:
            direct = next((m for m in choices if Path(m.name).name == f'vendor_{slot}.img'), None)
        elif len(choices) == 1:
            direct = choices[0]
        elif len(choices) > 1:
            raise RomError('both standalone vendor images are present; use --vendor-slot')
    if direct:
        output = directory / 'vendor.img'
        with rom.open(direct) as source, output.open('wb') as target:
            magic = read_exact(source, 4)
            source.seek(0)
            if struct.unpack('<I', magic)[0] == SPARSE_MAGIC:
                target.truncate(copy_sparse(source, target, [(0, 1 << 45, 0)]))
            else:
                shutil.copyfileobj(source, target, CHUNK)
        return output
    members = rom.super_members()
    if not shutil.which('lpdump'):
        raise RomError('lpdump is required to read super partition metadata')
    prefix = directory / 'super-prefix.img'
    _prefix(rom, members[0], prefix)
    result = subprocess.run(['lpdump', str(prefix)], capture_output=True, text=True, check=False)
    prefix.unlink(missing_ok=True)
    if result.returncode:
        raise RomError(f'lpdump could not read super metadata: {result.stderr.strip()}')
    _, wanted = parse_lpdump(result.stdout, slot)
    output = directory / 'vendor.img'
    size = max(offset + end - begin for begin, end, offset in wanted)
    overlay = len(members) > 1 and all(
        Path(member.name).name.startswith('super.img_sparsechunk.') for member in members)
    spans = [_logical_span(rom, member) for member in members]
    if overlay and len(set(spans)) != 1:
        raise RomError('sparse super chunks disagree on logical image size')
    super_size = spans[0] if overlay else sum(spans)
    if size > 64 * (1 << 30) or max(end for _, end, _ in wanted) > super_size:
        raise RomError('vendor extents exceed super image bounds')
    with output.open('wb') as target:
        target.truncate(size)
        raw_base = 0
        for member in members:
            with rom.open(member) as source:
                magic = read_exact(source, 4)
                source.seek(0)
                if struct.unpack('<I', magic)[0] == SPARSE_MAGIC:
                    # Motorola sparsechunks each describe the entire logical super.
                    # Numbered super.N.img pieces are consecutive slices instead.
                    total = copy_sparse(source, target, wanted, 0 if overlay else raw_base)
                    if not overlay:
                        raw_base += total
                else:
                    if raw_base < max(end for _, end, _ in wanted):
                        copy_intersection(source, target, raw_base, member.size, wanted)
                    raw_base += member.size
    return output


def extract_modules(rom: RomDump, modules: tuple[str, ...], destination: Path,
                    slot: str | None = None,
                    firmware: tuple[str, ...] = ()) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    vendor = vendor_image(rom, destination, slot)
    with vendor.open('rb') as stream:
        stream.seek(1080)
        if stream.read(2) != b'\x53\xef':
            raise RomError('vendor is not ext4; this ROM needs another filesystem extractor')
    module_dir = destination / 'lib/modules'
    module_dir.mkdir(parents=True, exist_ok=True)
    if not shutil.which('debugfs'):
        raise RomError('debugfs is required to extract modules from ext4 vendor')
    for directory, names in (('lib/modules', modules), ('firmware', firmware)):
        for name in names:
            parts = name.split('/')
            if not parts or any(not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', part)
                                for part in parts) or (directory == 'lib/modules' and
                                                        (len(parts) != 1 or not name.endswith('.ko'))):
                raise RomError(f'unsafe driver file name: {name}')
            target = destination / directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            result = subprocess.run(['debugfs', '-R', f'dump /{directory}/{name} {target}', str(vendor)],
                                    capture_output=True, text=True, check=False)
            if result.returncode or not target.is_file() or not target.stat().st_size:
                raise RomError(f'missing /vendor/{directory}/{name}: {result.stderr.strip()}')
    vendor.unlink()
    return destination

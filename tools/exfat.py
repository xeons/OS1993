"""A small exFAT reader and writer for test images (no host tool writes files
into one without mounting it). fsck.exfat checks what this writes.

    v = ExFat(path, offset)          # the partition's byte offset in the image
    v.mkdir("Sub Folder"); v.write("Sub Folder/A File.txt", b"...")
    v.listdir("Sub Folder") -> [Entry(name, attr, size, first, contig)]
    v.read("Sub Folder/A File.txt") -> bytes
    v.close()

Files it writes are contiguous (NoFatChain); it reads FAT chains too.
"""
import struct
from collections import namedtuple

Entry = namedtuple("Entry", "name attr size first contig idx count")
EOC = 0xFFFFFFFF


def _sum16(data, skip=(2, 3)):
    s = 0
    for i, b in enumerate(data):
        if i in skip:
            continue
        s = (((s & 1) << 15) | (s >> 1)) + b & 0xFFFF
    return s


class ExFat:
    def __init__(self, path, offset):
        self.f = open(path, "r+b")
        self.off = offset
        self.bps = 512
        bs = self.sectors(0, 1)
        assert bs[3:11] == b"EXFAT   ", "not exFAT"
        self.bps = 1 << bs[108]                         # the volume's sector size
        (self.fat_off, self.fat_len, self.heap_off, self.nclus, self.root) = struct.unpack_from("<IIIII", bs, 80)
        self.cbytes = self.bps << bs[109]
        self.bitmap = self.upcase_first = None
        self.up = {}
        for e in self.raw_entries(self.root, False, 0):
            t, ent = e[1], e[2]
            if t == 0x81:
                self.bm_first, self.bm_len = struct.unpack_from("<IQ", ent, 20)
            elif t == 0x82:
                first, ln = struct.unpack_from("<IQ", ent, 20)
                self._load_upcase(first, ln)
        self.bm = bytearray(self.read_chain(self.bm_first, True, self.bm_len))

    # ---- raw access
    def sectors(self, lba, n):
        self.f.seek(self.off + lba * self.bps)
        return self.f.read(n * self.bps)

    def clus_pos(self, c):
        return self.off + self.heap_off * self.bps + (c - 2) * self.cbytes

    def read_clus(self, c):
        self.f.seek(self.clus_pos(c))
        return self.f.read(self.cbytes)

    def write_clus(self, c, data):
        self.f.seek(self.clus_pos(c))
        self.f.write(data.ljust(self.cbytes, b"\0"))

    def fat(self, c):
        self.f.seek(self.off + self.fat_off * self.bps + c * 4)
        return struct.unpack("<I", self.f.read(4))[0]

    def chain(self, first, contig, length):
        n = (length + self.cbytes - 1) // self.cbytes if length else None
        c, out = first, []
        while c >= 2 and c < self.nclus + 2 and (n is None or len(out) < n):
            out.append(c)
            c = c + 1 if contig else self.fat(c)
        return out

    def read_chain(self, first, contig, length):
        return b"".join(self.read_clus(c) for c in self.chain(first, contig, length))[:length]

    def _load_upcase(self, first, ln):
        data = self.read_chain(first, True, ln)
        ch, i = 0, 0
        while i + 1 < len(data):
            u = struct.unpack_from("<H", data, i)[0]
            i += 2
            if u == 0xFFFF and i + 1 < len(data):
                ch += struct.unpack_from("<H", data, i)[0]
                i += 2
            else:
                self.up[ch] = u
                ch += 1

    def upper(self, c):
        return self.up.get(c, c)

    def name_hash(self, name):
        h = 0
        for ch in name:
            u = self.upper(ord(ch))
            for b in (u & 0xFF, u >> 8):
                h = (((h & 1) << 15) | (h >> 1)) + b & 0xFFFF
        return h

    # ---- directories
    def dir_clusters(self, first, contig, length):
        return self.chain(first, contig, length if contig else 0)

    def raw_entries(self, first, contig, length):
        """(index, type, 32 bytes) for each entry up to the end marker."""
        idx = 0
        for c in self.dir_clusters(first, contig, length):
            data = self.read_clus(c)
            for k in range(0, self.cbytes, 32):
                ent = data[k:k + 32]
                if ent[0] == 0:
                    return
                yield idx, ent[0], ent
                idx += 1

    def entries(self, first, contig, length):
        raw = list(self.raw_entries(first, contig, length))
        i = 0
        while i < len(raw):
            idx, t, ent = raw[i]
            if t != 0x85:
                i += 1
                continue
            count = ent[1] + 1
            sset = b"".join(r[2] for r in raw[i:i + count])
            assert _sum16(sset) == struct.unpack_from("<H", ent, 2)[0], "bad set checksum"
            st = raw[i + 1][2]
            nlen = st[3]
            name = "".join(r[2][2:32].decode("utf-16-le") for r in raw[i + 2:i + count])[:nlen]
            assert self.name_hash(name) == struct.unpack_from("<H", st, 4)[0], f"bad name hash: {name}"
            attr = struct.unpack_from("<H", ent, 4)[0]
            first_c, size = struct.unpack_from("<I", st, 20)[0], struct.unpack_from("<Q", st, 24)[0]
            yield Entry(name, attr, size, first_c, bool(st[1] & 2), idx, count)
            i += count

    def lookup(self, path):
        first, contig, length = self.root, False, 0
        e = None
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for e in self.entries(first, contig, length):
                if e.name.upper() == part.upper():
                    break
            else:
                raise FileNotFoundError(path)
            first, contig, length = e.first, e.contig, e.size
        return e

    def _dir_of(self, path):
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        d = self.lookup("/".join(parts[:-1])) if len(parts) > 1 else None
        return d, parts[-1]

    def listdir(self, path=""):
        if not path.strip("/"):
            return list(self.entries(self.root, False, 0))
        e = self.lookup(path)
        return list(self.entries(e.first, e.contig, e.size))

    def read(self, path):
        e = self.lookup(path)
        return self.read_chain(e.first, e.contig, e.size)

    # ---- writing (contiguous files, into the directory's first cluster run)
    def alloc(self, n):
        run = 0
        for c in range(2, self.nclus + 2):
            used = self.bm[(c - 2) // 8] >> ((c - 2) % 8) & 1
            run = 0 if used else run + 1
            if run == n:
                start = c - n + 1
                for k in range(start, start + n):
                    self.bm[(k - 2) // 8] |= 1 << ((k - 2) % 8)
                return start
        raise OSError("disk full")

    def _add_set(self, dirent, name, attr, first, size, contig):
        nlen = len(name)
        names = (nlen + 14) // 15
        f = bytearray(32)
        f[0], f[1] = 0x85, 1 + names
        ts = (46 << 25) | (6 << 21) | (1 << 16) | (12 << 11)   # 2026-06-01 12:00
        struct.pack_into("<HHIII", f, 4, attr, 0, ts, ts, ts)
        s = bytearray(32)
        s[0], s[1], s[3] = 0xC0, 1 | (2 if contig else 0), nlen
        struct.pack_into("<H", s, 4, self.name_hash(name))
        struct.pack_into("<Q", s, 8, size)
        struct.pack_into("<IQ", s, 20, first, size)
        parts = [f, s]
        u = name.encode("utf-16-le")
        for i in range(names):
            n = bytearray(32)
            n[0] = 0xC1
            chunk = u[i * 30:(i + 1) * 30]
            n[2:2 + len(chunk)] = chunk
            parts.append(n)
        sset = b"".join(parts)
        struct.pack_into("<H", parts[0], 2, _sum16(sset))
        sset = b"".join(parts)
        if dirent is None:
            dfirst, dcontig, dlen = self.root, False, 0
        else:
            dfirst, dcontig, dlen = dirent.first, dirent.contig, dirent.size
        clusters = self.dir_clusters(dfirst, dcontig, dlen)
        used = sum(1 for _ in self.raw_entries(dfirst, dcontig, dlen))
        k = used * 32
        if k + len(sset) > len(clusters) * self.cbytes:
            raise OSError("directory full (this writer doesn't grow them)")
        for b in range(0, len(sset), 32):
            pos = k + b
            c = clusters[pos // self.cbytes]
            self.f.seek(self.clus_pos(c) + pos % self.cbytes)
            self.f.write(sset[b:b + 32])

    def write(self, path, data, attr=0x20):
        d, name = self._dir_of(path)
        n = max(1, (len(data) + self.cbytes - 1) // self.cbytes)
        first = self.alloc(n)
        for i in range(n):
            self.write_clus(first + i, data[i * self.cbytes:(i + 1) * self.cbytes])
        self._add_set(d, name, attr, first if data else 0, len(data), True)
        if not data:                                    # nothing allocated after all
            self.bm[(first - 2) // 8] &= ~(1 << ((first - 2) % 8))

    def alloc_run(self, n):
        """n free clusters in a row, marked in the bitmap (fast, for big files)."""
        run = 0
        for c in range(2, self.nclus + 2):
            byte = self.bm[(c - 2) // 8]
            if byte == 0xFF and (c - 2) % 8 == 0:
                run = 0
                continue
            used = byte >> ((c - 2) % 8) & 1
            run = 0 if used else run + 1
            if run == n:
                start = c - n + 1
                for k in range(start, start + n):
                    self.bm[(k - 2) // 8] |= 1 << ((k - 2) % 8)
                return start
        raise OSError("disk full")

    def write_sparse(self, path, size, pieces):
        """A contiguous file of size bytes, valid to its end; only pieces
        ({offset: bytes}) are written (a sparse image reads zeros elsewhere)."""
        d, name = self._dir_of(path)
        n = (size + self.cbytes - 1) // self.cbytes
        first = self.alloc_run(n)
        for off, data in pieces.items():
            self.f.seek(self.clus_pos(first) + off)
            self.f.write(data)
        self._add_set(d, name, 0x20, first, size, True)

    def read_at(self, path, off, n):
        e = self.lookup(path)
        if e.contig:
            self.f.seek(self.clus_pos(e.first) + off)
            return self.f.read(n)
        chain = self.chain(e.first, False, e.size)
        out = b""
        while n:
            c = chain[off // self.cbytes]
            k = min(n, self.cbytes - off % self.cbytes)
            self.f.seek(self.clus_pos(c) + off % self.cbytes)
            out += self.f.read(k)
            off += k
            n -= k
        return out

    def entry_raw(self, path):
        """The File entry (32 bytes) of path, for its timestamps."""
        e = self.lookup(path)
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        d = self.lookup("/".join(parts[:-1])) if len(parts) > 1 else None
        first, contig, length = (self.root, False, 0) if d is None else (d.first, d.contig, d.size)
        for idx, t, ent in self.raw_entries(first, contig, length):
            if idx == e.idx:
                return ent
        return None

    def mkdir(self, path):
        d, name = self._dir_of(path)
        c = self.alloc(1)
        self.write_clus(c, b"")
        self._add_set(d, name, 0x10, c, self.cbytes, True)

    def close(self):
        data = bytes(self.bm)
        for i, c in enumerate(self.chain(self.bm_first, True, self.bm_len)):
            self.f.seek(self.clus_pos(c))
            self.f.write(data[i * self.cbytes:(i + 1) * self.cbytes])
        # PercentInUse (boot sector byte 112) is outside the checksum: leave it.
        self.f.close()

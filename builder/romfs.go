package main

import (
	"crypto/sha256"
	"encoding/binary"
	"hash"
	"io"
	"sort"
	"strings"
	"unicode/utf16"
)

// Data made of pieces, written out in order without copying the files into
// one buffer: byte slices and runs of zeros.
type pieces struct {
	parts []piece
	size  int64
}

type piece struct {
	data  []byte
	zeros int64
}

func (p *pieces) add(b []byte) {
	p.parts = append(p.parts, piece{data: b})
	p.size += int64(len(b))
}

func (p *pieces) zero(n int64) {
	if n > 0 {
		p.parts = append(p.parts, piece{zeros: n})
		p.size += n
	}
}

func (p *pieces) padTo(align int64) {
	p.zero((align - p.size%align) % align)
}

func (p *pieces) addPieces(q *pieces) {
	p.parts = append(p.parts, q.parts...)
	p.size += q.size
}

var zeroBlock = make([]byte, 64<<10)

func (p *pieces) WriteTo(w io.Writer) (int64, error) {
	var n int64
	for _, part := range p.parts {
		if part.data != nil {
			k, err := w.Write(part.data)
			n += int64(k)
			if err != nil {
				return n, err
			}
			continue
		}
		for left := part.zeros; left > 0; {
			c := left
			if c > int64(len(zeroBlock)) {
				c = int64(len(zeroBlock))
			}
			k, err := w.Write(zeroBlock[:c])
			n += int64(k)
			if err != nil {
				return n, err
			}
			left -= c
		}
	}
	return n, nil
}

// The 3DS RomFS ("level 3"): the file system libctru reads, with the hash
// tables it looks names up in.

type romfsDir struct {
	name    string
	parent  *romfsDir
	dirs    []*romfsDir
	files   []*romfsEntry
	offset  uint32
	nextHsh uint32
}

type romfsEntry struct {
	name    string
	data    []byte
	parent  *romfsDir
	offset  uint32
	dataOff uint64
	nextHsh uint32
}

const romfsNone = 0xFFFFFFFF

func utf16Name(s string) []uint16 { return utf16.Encode([]rune(s)) }

// libctru's name hash (romfs_dev.c).
func romfsHash(parent uint32, name []uint16, buckets uint32) uint32 {
	h := parent ^ 123456789
	for _, c := range name {
		h = (h >> 5) | (h << 27)
		h ^= uint32(c)
	}
	return h % buckets
}

// How many hash buckets for count entries (as makerom and 3dstool choose).
func romfsBuckets(count int) uint32 {
	switch {
	case count < 3:
		return 3
	case count < 19:
		return uint32(count) | 1
	}
	n := uint32(count)
	for n%2 == 0 || n%3 == 0 || n%5 == 0 || n%7 == 0 || n%11 == 0 || n%13 == 0 || n%17 == 0 {
		n++
	}
	return n
}

func align(n, a int64) int64 { return (n + a - 1) / a * a }

// Builds the level 3 RomFS of the files: their metadata, then their data
// (each file 16-byte aligned).
func buildRomFS(files []romFile) *pieces {
	root := &romfsDir{}
	dirOf := map[string]*romfsDir{"": root}
	var getDir func(path string) *romfsDir
	getDir = func(path string) *romfsDir {
		if d, ok := dirOf[path]; ok {
			return d
		}
		parentPath, name := "", path
		if i := strings.LastIndex(path, "/"); i >= 0 {
			parentPath, name = path[:i], path[i+1:]
		}
		parent := getDir(parentPath)
		d := &romfsDir{name: name, parent: parent}
		parent.dirs = append(parent.dirs, d)
		dirOf[path] = d
		return d
	}
	for _, f := range files {
		dirPath, name := "", f.path
		if i := strings.LastIndex(f.path, "/"); i >= 0 {
			dirPath, name = f.path[:i], f.path[i+1:]
		}
		d := getDir(dirPath)
		d.files = append(d.files, &romfsEntry{name: name, data: f.data, parent: d})
	}

	// Directories breadth first, each one's files together.
	var dirs []*romfsDir
	var entries []*romfsEntry
	queue := []*romfsDir{root}
	for len(queue) > 0 {
		d := queue[0]
		queue = queue[1:]
		sort.Slice(d.dirs, func(i, j int) bool { return d.dirs[i].name < d.dirs[j].name })
		sort.Slice(d.files, func(i, j int) bool { return d.files[i].name < d.files[j].name })
		dirs = append(dirs, d)
		entries = append(entries, d.files...)
		queue = append(queue, d.dirs...)
	}
	var dirTableSize, fileTableSize uint32
	for _, d := range dirs {
		d.offset = dirTableSize
		dirTableSize += 0x18 + uint32(align(int64(len(utf16Name(d.name))*2), 4))
	}
	var dataSize uint64
	for _, f := range entries {
		f.offset = fileTableSize
		fileTableSize += 0x20 + uint32(align(int64(len(utf16Name(f.name))*2), 4))
		dataSize = uint64(align(int64(dataSize), 16))
		f.dataOff = dataSize
		dataSize += uint64(len(f.data))
	}

	dirBuckets, fileBuckets := romfsBuckets(len(dirs)), romfsBuckets(len(entries))
	dirHash := make([]uint32, dirBuckets)
	fileHash := make([]uint32, fileBuckets)
	for i := range dirHash {
		dirHash[i] = romfsNone
	}
	for i := range fileHash {
		fileHash[i] = romfsNone
	}
	for _, d := range dirs {
		parent := uint32(0)
		if d.parent != nil {
			parent = d.parent.offset
		}
		h := romfsHash(parent, utf16Name(d.name), dirBuckets)
		d.nextHsh, dirHash[h] = dirHash[h], d.offset
	}
	for _, f := range entries {
		h := romfsHash(f.parent.offset, utf16Name(f.name), fileBuckets)
		f.nextHsh, fileHash[h] = fileHash[h], f.offset
	}

	le := binary.LittleEndian
	put := func(b []byte, v uint32) []byte { return le.AppendUint32(b, v) }
	putName := func(b []byte, name string) []byte {
		u := utf16Name(name)
		for _, c := range u {
			b = le.AppendUint16(b, c)
		}
		for len(u)*2%4 != 0 {
			b = append(b, 0, 0)
			u = append(u, 0)
		}
		return b
	}

	dirTable := make([]byte, 0, dirTableSize)
	for _, d := range dirs {
		parent, sibling, child, file := uint32(0), uint32(romfsNone), uint32(romfsNone), uint32(romfsNone)
		if d.parent != nil {
			parent = d.parent.offset
			sibs := d.parent.dirs
			for i, s := range sibs {
				if s == d && i+1 < len(sibs) {
					sibling = sibs[i+1].offset
				}
			}
		}
		if len(d.dirs) > 0 {
			child = d.dirs[0].offset
		}
		if len(d.files) > 0 {
			file = d.files[0].offset
		}
		dirTable = put(put(put(put(put(put(dirTable, parent), sibling), child), file), d.nextHsh), uint32(len(utf16Name(d.name))*2))
		dirTable = putName(dirTable, d.name)
	}
	fileTable := make([]byte, 0, fileTableSize)
	for _, f := range entries {
		sibling := uint32(romfsNone)
		sibs := f.parent.files
		for i, s := range sibs {
			if s == f && i+1 < len(sibs) {
				sibling = sibs[i+1].offset
			}
		}
		fileTable = put(put(fileTable, f.parent.offset), sibling)
		fileTable = le.AppendUint64(fileTable, f.dataOff)
		fileTable = le.AppendUint64(fileTable, uint64(len(f.data)))
		fileTable = put(put(fileTable, f.nextHsh), uint32(len(utf16Name(f.name))*2))
		fileTable = putName(fileTable, f.name)
	}

	const headerSize = 0x28
	dirHashOff := uint32(headerSize)
	dirTableOff := dirHashOff + dirBuckets*4
	fileHashOff := dirTableOff + dirTableSize
	fileTableOff := fileHashOff + fileBuckets*4
	dataOff := uint32(align(int64(fileTableOff+fileTableSize), 16))

	meta := make([]byte, 0, dataOff)
	for _, v := range []uint32{headerSize, dirHashOff, dirBuckets * 4, dirTableOff, dirTableSize, fileHashOff, fileBuckets * 4, fileTableOff, fileTableSize, dataOff} {
		meta = put(meta, v)
	}
	for _, v := range dirHash {
		meta = put(meta, v)
	}
	meta = append(meta, dirTable...)
	for _, v := range fileHash {
		meta = put(meta, v)
	}
	meta = append(meta, fileTable...)
	for len(meta) < int(dataOff) {
		meta = append(meta, 0)
	}

	out := &pieces{}
	out.add(meta)
	for _, f := range entries {
		out.zero(int64(dataOff) + int64(f.dataOff) - out.size)
		out.add(f.data)
	}
	return out
}

// Hashes data in blocks of blockSize (the last one zero-padded): the
// SHA-256 of each, one after another.
type blockHasher struct {
	blockSize int
	h         hash.Hash
	filled    int
	sums      []byte
}

func newBlockHasher(blockSize int) *blockHasher {
	return &blockHasher{blockSize: blockSize, h: sha256.New()}
}

func (b *blockHasher) Write(p []byte) (int, error) {
	n := len(p)
	for len(p) > 0 {
		c := b.blockSize - b.filled
		if c > len(p) {
			c = len(p)
		}
		b.h.Write(p[:c])
		b.filled += c
		p = p[c:]
		if b.filled == b.blockSize {
			b.sums = b.h.Sum(b.sums)
			b.h.Reset()
			b.filled = 0
		}
	}
	return n, nil
}

func (b *blockHasher) finish() []byte {
	if b.filled > 0 {
		b.Write(make([]byte, b.blockSize-b.filled))
	}
	return b.sums
}

func hashBlocks(data []byte, blockSize int) []byte {
	b := newBlockHasher(blockSize)
	b.Write(data)
	return b.finish()
}

// The RomFS of an NCCH (a CIA's program): level 3 under an IVFC hash tree
// (levels 1 and 2 and the master hash, 4KB blocks), laid out as makerom does:
// the header and master hash, level 3, level 1, level 2, each 4KB aligned.
// Returns the region and its start that the NCCH's superblock hash covers
// (the header and master hash, in whole media units).
func buildIVFC(level3 *pieces) (*pieces, []byte) {
	const block = 0x1000
	h3 := newBlockHasher(block)
	level3.WriteTo(h3)
	level2 := h3.finish()
	level1 := hashBlocks(level2, block)
	master := hashBlocks(level1, block)

	l1Off := int64(0)
	l2Off := align(l1Off+int64(len(level1)), block)
	l3Off := align(l2Off+int64(len(level2)), block)
	le := binary.LittleEndian
	hdr := []byte("IVFC")
	hdr = le.AppendUint32(hdr, 0x10000)
	hdr = le.AppendUint32(hdr, uint32(len(master)))
	for _, lv := range []struct{ off, size int64 }{{l1Off, int64(len(level1))}, {l2Off, int64(len(level2))}, {l3Off, level3.size}} {
		hdr = le.AppendUint64(hdr, uint64(lv.off))
		hdr = le.AppendUint64(hdr, uint64(lv.size))
		hdr = le.AppendUint32(hdr, 12)
		hdr = le.AppendUint32(hdr, 0)
	}
	hdr = le.AppendUint32(hdr, 0x5C) // header size
	hdr = le.AppendUint32(hdr, 0)    // optional info size
	hdr = append(hdr, 0, 0, 0, 0)    // to 0x60
	hdr = append(hdr, master...)
	super := make([]byte, align(int64(len(hdr)), 0x200))
	copy(super, hdr)

	out := &pieces{}
	out.add(hdr)
	out.padTo(block)
	out.addPieces(level3)
	out.padTo(block)
	out.add(level1)
	out.padTo(block)
	out.add(level2)
	out.padTo(block)
	return out, super
}

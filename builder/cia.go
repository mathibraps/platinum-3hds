package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"io"
)

// The CIA: a template made by makerom from the port's program with a
// placeholder RomFS (templates/template.cia). The builder keeps everything
// but the RomFS and the icon, and redoes the hashes and sizes they change:
// the NCCH's superblock hashes and sizes, the ExeFS's icon hash, the TMD's
// content record and its hashes, the CIA's content size.

const mediaUnit = 0x200

type ciaTemplate struct {
	header, certs, ticket, tmd, meta []byte
	ncchHead                         []byte // the NCCH up to its RomFS (header, exheader, ExeFS)
	romfsOffset                      int64  // in media units
}

func align64(n int64) int64 { return align(n, 64) }

func parseCIA(t []byte) (*ciaTemplate, error) {
	le := binary.LittleEndian
	if len(t) < 0x2020 {
		return nil, fmt.Errorf("template CIA too small")
	}
	hs := int64(le.Uint32(t[0:]))
	certSize, tikSize, tmdSize, metaSize := int64(le.Uint32(t[8:])), int64(le.Uint32(t[0xC:])), int64(le.Uint32(t[0x10:])), int64(le.Uint32(t[0x14:]))
	contentSize := int64(le.Uint64(t[0x18:]))
	offCert := align64(hs)
	offTik := align64(offCert + certSize)
	offTMD := align64(offTik + tikSize)
	offContent := align64(offTMD + tmdSize)
	offMeta := align64(offContent + contentSize)
	if offMeta+metaSize > int64(len(t)) || metaSize < 0x400+0x36C0 {
		return nil, fmt.Errorf("template CIA cut short")
	}
	ncch := t[offContent : offContent+contentSize]
	if string(ncch[0x100:0x104]) != "NCCH" {
		return nil, fmt.Errorf("template CIA has no NCCH")
	}
	romfsOff := int64(le.Uint32(ncch[0x1B0:]))
	return &ciaTemplate{
		header:      t[:hs],
		certs:       t[offCert : offCert+certSize],
		ticket:      t[offTik : offTik+tikSize],
		tmd:         append([]byte(nil), t[offTMD:offTMD+tmdSize]...),
		meta:        append([]byte(nil), t[offMeta:offMeta+metaSize]...),
		ncchHead:    append([]byte(nil), ncch[:romfsOff*mediaUnit]...),
		romfsOffset: romfsOff,
	}, nil
}

// Writes the CIA with this RomFS (level 3) and SMDH.
func (c *ciaTemplate) write(w io.Writer, level3 *pieces, smdh []byte) error {
	le := binary.LittleEndian
	ncch := append([]byte(nil), c.ncchHead...)

	// The icon in the ExeFS, and its hash in the ExeFS header.
	exefs := ncch[int64(le.Uint32(ncch[0x1A0:]))*mediaUnit:]
	found := false
	for i := 0; i < 10; i++ {
		name := string(bytes.TrimRight(exefs[i*16:i*16+8], "\x00"))
		off, size := le.Uint32(exefs[i*16+8:]), le.Uint32(exefs[i*16+12:])
		if name != "icon" {
			continue
		}
		if int(size) != len(smdh) {
			return fmt.Errorf("template icon is %d bytes, not %d", size, len(smdh))
		}
		copy(exefs[0x200+off:], smdh)
		sum := sha256.Sum256(smdh)
		copy(exefs[0xC0+(9-i)*0x20:], sum[:])
		found = true
	}
	if !found {
		return fmt.Errorf("template ExeFS has no icon")
	}
	exefsSuper := sha256.Sum256(exefs[:int64(le.Uint32(ncch[0x1A8:]))*mediaUnit])
	copy(ncch[0x1C0:], exefsSuper[:])

	// The RomFS, and the NCCH's sizes and hashes of it.
	romfs, super := buildIVFC(level3)
	romfsSuper := sha256.Sum256(super)
	romfsUnits := romfs.size / mediaUnit
	le.PutUint32(ncch[0x1B4:], uint32(romfsUnits))
	le.PutUint32(ncch[0x1B8:], uint32(len(super)/mediaUnit))
	copy(ncch[0x1E0:], romfsSuper[:])
	le.PutUint32(ncch[0x104:], uint32(c.romfsOffset+romfsUnits))

	content := &pieces{}
	content.add(ncch)
	content.zero(c.romfsOffset*mediaUnit - int64(len(ncch)))
	content.addPieces(romfs)
	contentHash := sha256.New()
	content.WriteTo(contentHash)

	// The TMD: the content's size and hash, then the hashes over its records.
	tmd := c.tmd
	sigSize := map[uint32]int{0x10000: 0x200 + 0x3C, 0x10001: 0x100 + 0x3C, 0x10002: 0x3C + 0x40, 0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}[binary.BigEndian.Uint32(tmd)]
	if sigSize == 0 {
		return fmt.Errorf("template TMD has an unknown signature type")
	}
	hdr := tmd[4+sigSize:]
	count := int(binary.BigEndian.Uint16(hdr[0x9E:]))
	info := hdr[0xC4 : 0xC4+64*0x24]
	chunks := hdr[0xC4+64*0x24 : 0xC4+64*0x24+count*0x30]
	if count != 1 {
		return fmt.Errorf("template has %d contents, not 1", count)
	}
	binary.BigEndian.PutUint64(chunks[8:], uint64(content.size))
	copy(chunks[0x10:0x30], contentHash.Sum(nil))
	chunkHash := sha256.Sum256(chunks)
	copy(info[4:0x24], chunkHash[:])
	infoHash := sha256.Sum256(info)
	copy(hdr[0xA4:0xC4], infoHash[:])

	// The meta section's copy of the SMDH.
	meta := c.meta
	copy(meta[0x400:], smdh)

	header := append([]byte(nil), c.header...)
	le.PutUint64(header[0x18:], uint64(content.size))
	out := &pieces{}
	for _, part := range [][]byte{header, c.certs, c.ticket, tmd} {
		out.add(part)
		out.padTo(64)
	}
	out.addPieces(content)
	out.padTo(64)
	out.add(meta)
	_, err := out.WriteTo(w)
	return err
}

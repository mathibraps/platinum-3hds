package main

import (
	"encoding/binary"
	"fmt"
	"io"
)

// The 3DSX for the Homebrew Launcher: a template made by 3dsxtool with the
// SMDH but no RomFS (templates/template.3dsx). The RomFS (level 3, as
// 3dsxtool appends it) goes right after the SMDH, and the extended header
// points to it.
func write3DSX(w io.Writer, template []byte, level3 *pieces, smdh []byte) error {
	le := binary.LittleEndian
	if len(template) < 0x2C || string(template[:4]) != "3DSX" || le.Uint16(template[4:]) < 0x2C {
		return fmt.Errorf("template 3DSX has no extended header")
	}
	head := append([]byte(nil), template...)
	smdhOff, smdhSize := le.Uint32(head[0x20:]), le.Uint32(head[0x24:])
	if int(smdhSize) != len(smdh) || int(smdhOff)+len(smdh) != len(head) {
		return fmt.Errorf("template 3DSX does not end with its SMDH")
	}
	copy(head[smdhOff:], smdh)
	le.PutUint32(head[0x28:], uint32(len(head)))
	out := &pieces{}
	out.add(head)
	out.addPieces(level3)
	_, err := out.WriteTo(w)
	return err
}

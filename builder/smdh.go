package main

import "encoding/binary"

// The HOME menu icon from the ROM's DS icon: 32x32 scaled to 48x48 (each
// pixel tripled, then 2x2 averaged), on a platinum backdrop where the DS
// icon is transparent; and 24x24 from that.

type image struct {
	w, h int
	px   [][3]uint8
}

func newImage(w, h int) *image { return &image{w, h, make([][3]uint8, w*h)} }

func (m *image) at(x, y int) [3]uint8     { return m.px[y*m.w+x] }
func (m *image) set(x, y int, c [3]uint8) { m.px[y*m.w+x] = c }

func iconFromDS(rgb [32][32][3]uint8, opaque [32][32]bool) (large, small *image) {
	big := newImage(96, 96)
	for y := 0; y < 96; y++ {
		for x := 0; x < 96; x++ {
			sx, sy := x/3, y/3
			if opaque[sy][sx] {
				big.set(x, y, rgb[sy][sx])
				continue
			}
			// Backdrop: silver, lighter at the top left, a soft dark rim.
			t := float64(x+y) / 190
			v := 236 - 70*t
			edge := min(min(x, 95-x), min(y, 95-y))
			if edge < 3 {
				v *= 0.55 + 0.15*float64(edge)
			}
			c := uint8(v)
			big.set(x, y, [3]uint8{c, c, uint8(min(255, v+10))})
		}
	}
	return shrink(big, 2), shrink(shrink(big, 2), 2)
}

func shrink(m *image, f int) *image {
	out := newImage(m.w/f, m.h/f)
	for y := 0; y < out.h; y++ {
		for x := 0; x < out.w; x++ {
			var sum [3]int
			for dy := 0; dy < f; dy++ {
				for dx := 0; dx < f; dx++ {
					c := m.at(x*f+dx, y*f+dy)
					sum[0] += int(c[0])
					sum[1] += int(c[1])
					sum[2] += int(c[2])
				}
			}
			n := f * f
			out.set(x, y, [3]uint8{uint8(sum[0] / n), uint8(sum[1] / n), uint8(sum[2] / n)})
		}
	}
	return out
}

// An icon in the SMDH's form: RGB565 in 8x8 tiles, Morton order inside each.
func encodeIcon(m *image) []byte {
	out := make([]byte, m.w*m.h*2)
	for y := 0; y < m.h; y++ {
		for x := 0; x < m.w; x++ {
			c := m.at(x, y)
			v := uint16(c[0]>>3)<<11 | uint16(c[1]>>2)<<5 | uint16(c[2]>>3)
			morton := 0
			for i := 0; i < 3; i++ {
				morton |= (x>>i&1)<<(2*i) | (y>>i&1)<<(2*i+1)
			}
			i := ((y/8)*(m.w/8)+x/8)*64 + morton
			binary.LittleEndian.PutUint16(out[i*2:], v)
		}
	}
	return out
}

// Puts the icons into a copy of an SMDH (small 24x24 at 0x2040, large 48x48
// at 0x24C0).
func smdhWithIcon(smdh []byte, large, small *image) []byte {
	out := append([]byte(nil), smdh...)
	copy(out[0x2040:], encodeIcon(small))
	copy(out[0x24C0:], encodeIcon(large))
	return out
}

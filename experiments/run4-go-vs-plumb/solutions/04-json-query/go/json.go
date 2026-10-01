package main

import (
	"errors"
	"strings"
	"unicode/utf8"
)

type kind int

const (
	kString kind = iota
	kNumber
	kLiteral // true, false, null
	kObject
	kArray
)

type member struct {
	key string
	val *value
}

// value is a parsed JSON value. Numbers and literals keep their source text.
type value struct {
	kind    kind
	text    string // string: decoded content; number/literal: source text
	members []member
	elems   []*value
}

var errInvalid = errors.New("invalid JSON")

type parser struct {
	s string
	i int
}

// parseDocument parses a whole line as one JSON value.
func parseDocument(s string) (*value, error) {
	p := &parser{s: s}
	p.ws()
	v, err := p.value()
	if err != nil {
		return nil, err
	}
	p.ws()
	if p.i != len(p.s) {
		return nil, errInvalid
	}
	return v, nil
}

func (p *parser) ws() {
	for p.i < len(p.s) {
		switch p.s[p.i] {
		case ' ', '\t', '\n', '\r':
			p.i++
		default:
			return
		}
	}
}

func (p *parser) value() (*value, error) {
	if p.i >= len(p.s) {
		return nil, errInvalid
	}
	switch c := p.s[p.i]; {
	case c == '{':
		return p.object()
	case c == '[':
		return p.array()
	case c == '"':
		s, err := p.str()
		if err != nil {
			return nil, err
		}
		return &value{kind: kString, text: s}, nil
	case c == '-' || (c >= '0' && c <= '9'):
		return p.number()
	case strings.HasPrefix(p.s[p.i:], "true"):
		p.i += 4
		return &value{kind: kLiteral, text: "true"}, nil
	case strings.HasPrefix(p.s[p.i:], "false"):
		p.i += 5
		return &value{kind: kLiteral, text: "false"}, nil
	case strings.HasPrefix(p.s[p.i:], "null"):
		p.i += 4
		return &value{kind: kLiteral, text: "null"}, nil
	}
	return nil, errInvalid
}

func (p *parser) digits() int {
	n := 0
	for p.i < len(p.s) && p.s[p.i] >= '0' && p.s[p.i] <= '9' {
		p.i++
		n++
	}
	return n
}

func (p *parser) number() (*value, error) {
	start := p.i
	if p.s[p.i] == '-' {
		p.i++
	}
	if p.i >= len(p.s) {
		return nil, errInvalid
	}
	if p.s[p.i] == '0' {
		p.i++
	} else if p.digits() == 0 {
		return nil, errInvalid
	}
	if p.i < len(p.s) && p.s[p.i] == '.' {
		p.i++
		if p.digits() == 0 {
			return nil, errInvalid
		}
	}
	if p.i < len(p.s) && (p.s[p.i] == 'e' || p.s[p.i] == 'E') {
		p.i++
		if p.i < len(p.s) && (p.s[p.i] == '+' || p.s[p.i] == '-') {
			p.i++
		}
		if p.digits() == 0 {
			return nil, errInvalid
		}
	}
	return &value{kind: kNumber, text: p.s[start:p.i]}, nil
}

func hex4(s string) (rune, bool) {
	if len(s) < 4 {
		return 0, false
	}
	var r rune
	for k := 0; k < 4; k++ {
		c := s[k]
		switch {
		case c >= '0' && c <= '9':
			r = r<<4 | rune(c-'0')
		case c >= 'a' && c <= 'f':
			r = r<<4 | rune(c-'a'+10)
		case c >= 'A' && c <= 'F':
			r = r<<4 | rune(c-'A'+10)
		default:
			return 0, false
		}
	}
	return r, true
}

// str parses a string literal at p.i (which is '"') and returns its content.
func (p *parser) str() (string, error) {
	p.i++
	var b strings.Builder
	for {
		if p.i >= len(p.s) {
			return "", errInvalid
		}
		c := p.s[p.i]
		switch {
		case c == '"':
			p.i++
			return b.String(), nil
		case c < 0x20:
			return "", errInvalid
		case c == '\\':
			p.i++
			if p.i >= len(p.s) {
				return "", errInvalid
			}
			e := p.s[p.i]
			p.i++
			switch e {
			case '"', '\\', '/':
				b.WriteByte(e)
			case 'b':
				b.WriteByte('\b')
			case 'f':
				b.WriteByte('\f')
			case 'n':
				b.WriteByte('\n')
			case 'r':
				b.WriteByte('\r')
			case 't':
				b.WriteByte('\t')
			case 'u':
				r, ok := hex4(p.s[p.i:])
				if !ok {
					return "", errInvalid
				}
				p.i += 4
				if r >= 0xD800 && r < 0xDC00 {
					if strings.HasPrefix(p.s[p.i:], "\\u") {
						if r2, ok := hex4(p.s[p.i+2:]); ok && r2 >= 0xDC00 && r2 < 0xE000 {
							p.i += 6
							r = 0x10000 + (r-0xD800)<<10 + (r2 - 0xDC00)
						} else {
							r = utf8.RuneError
						}
					} else {
						r = utf8.RuneError
					}
				} else if r >= 0xDC00 && r < 0xE000 {
					r = utf8.RuneError
				}
				b.WriteRune(r)
			default:
				return "", errInvalid
			}
		case c < 0x80:
			b.WriteByte(c)
			p.i++
		default:
			r, n := utf8.DecodeRuneInString(p.s[p.i:])
			if r == utf8.RuneError && n == 1 {
				return "", errInvalid
			}
			b.WriteString(p.s[p.i : p.i+n])
			p.i += n
		}
	}
}

func (p *parser) object() (*value, error) {
	p.i++
	v := &value{kind: kObject}
	p.ws()
	if p.i < len(p.s) && p.s[p.i] == '}' {
		p.i++
		return v, nil
	}
	for {
		p.ws()
		if p.i >= len(p.s) || p.s[p.i] != '"' {
			return nil, errInvalid
		}
		k, err := p.str()
		if err != nil {
			return nil, err
		}
		p.ws()
		if p.i >= len(p.s) || p.s[p.i] != ':' {
			return nil, errInvalid
		}
		p.i++
		p.ws()
		val, err := p.value()
		if err != nil {
			return nil, err
		}
		v.members = append(v.members, member{k, val})
		p.ws()
		if p.i >= len(p.s) {
			return nil, errInvalid
		}
		switch p.s[p.i] {
		case ',':
			p.i++
		case '}':
			p.i++
			return v, nil
		default:
			return nil, errInvalid
		}
	}
}

func (p *parser) array() (*value, error) {
	p.i++
	v := &value{kind: kArray}
	p.ws()
	if p.i < len(p.s) && p.s[p.i] == ']' {
		p.i++
		return v, nil
	}
	for {
		p.ws()
		e, err := p.value()
		if err != nil {
			return nil, err
		}
		v.elems = append(v.elems, e)
		p.ws()
		if p.i >= len(p.s) {
			return nil, errInvalid
		}
		switch p.s[p.i] {
		case ',':
			p.i++
		case ']':
			p.i++
			return v, nil
		default:
			return nil, errInvalid
		}
	}
}

const hexdigits = "0123456789abcdef"

func writeString(b *strings.Builder, s string) {
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"':
			b.WriteString(`\"`)
		case c == '\\':
			b.WriteString(`\\`)
		case c == '\b':
			b.WriteString(`\b`)
		case c == '\t':
			b.WriteString(`\t`)
		case c == '\n':
			b.WriteString(`\n`)
		case c == '\f':
			b.WriteString(`\f`)
		case c == '\r':
			b.WriteString(`\r`)
		case c < 0x20:
			b.WriteString(`\u00`)
			b.WriteByte(hexdigits[c>>4])
			b.WriteByte(hexdigits[c&15])
		default:
			b.WriteByte(c)
		}
	}
	b.WriteByte('"')
}

func writeValue(b *strings.Builder, v *value) {
	switch v.kind {
	case kString:
		writeString(b, v.text)
	case kNumber, kLiteral:
		b.WriteString(v.text)
	case kObject:
		b.WriteByte('{')
		for i, m := range v.members {
			if i > 0 {
				b.WriteByte(',')
			}
			writeString(b, m.key)
			b.WriteByte(':')
			writeValue(b, m.val)
		}
		b.WriteByte('}')
	case kArray:
		b.WriteByte('[')
		for i, e := range v.elems {
			if i > 0 {
				b.WriteByte(',')
			}
			writeValue(b, e)
		}
		b.WriteByte(']')
	}
}

package main

import (
	"errors"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// Value kinds. Numbers keep their source text.
type kind int

const (
	kNull kind = iota
	kBool
	kNumber
	kString
	kArray
	kObject
)

type value struct {
	kind kind
	b    bool
	s    string // string contents, or number text
	arr  []*value
	obj  map[string]*value
}

var errSyntax = errors.New("syntax error")

type parser struct {
	s   string
	pos int
}

// parseJSON parses a complete RFC 8259 JSON text (surrounding whitespace allowed).
func parseJSON(s string) (*value, error) {
	if !utf8.ValidString(s) {
		return nil, errSyntax
	}
	p := &parser{s: s}
	p.ws()
	v, err := p.value()
	if err != nil {
		return nil, err
	}
	p.ws()
	if p.pos != len(p.s) {
		return nil, errSyntax
	}
	return v, nil
}

func (p *parser) ws() {
	for p.pos < len(p.s) {
		switch p.s[p.pos] {
		case ' ', '\t', '\n', '\r':
			p.pos++
		default:
			return
		}
	}
}

func (p *parser) lit(word string) bool {
	if strings.HasPrefix(p.s[p.pos:], word) {
		p.pos += len(word)
		return true
	}
	return false
}

func (p *parser) value() (*value, error) {
	if p.pos >= len(p.s) {
		return nil, errSyntax
	}
	switch c := p.s[p.pos]; {
	case c == '{':
		return p.object()
	case c == '[':
		return p.array()
	case c == '"':
		s, err := p.str()
		if err != nil {
			return nil, err
		}
		return &value{kind: kString, s: s}, nil
	case c == 't':
		if p.lit("true") {
			return &value{kind: kBool, b: true}, nil
		}
	case c == 'f':
		if p.lit("false") {
			return &value{kind: kBool}, nil
		}
	case c == 'n':
		if p.lit("null") {
			return &value{kind: kNull}, nil
		}
	case c == '-' || (c >= '0' && c <= '9'):
		return p.number()
	}
	return nil, errSyntax
}

func isDigit(c byte) bool { return c >= '0' && c <= '9' }

func (p *parser) digits() bool {
	start := p.pos
	for p.pos < len(p.s) && isDigit(p.s[p.pos]) {
		p.pos++
	}
	return p.pos > start
}

func (p *parser) number() (*value, error) {
	start := p.pos
	if p.s[p.pos] == '-' {
		p.pos++
	}
	if p.pos >= len(p.s) {
		return nil, errSyntax
	}
	if p.s[p.pos] == '0' {
		p.pos++
	} else if !p.digits() {
		return nil, errSyntax
	}
	if p.pos < len(p.s) && p.s[p.pos] == '.' {
		p.pos++
		if !p.digits() {
			return nil, errSyntax
		}
	}
	if p.pos < len(p.s) && (p.s[p.pos] == 'e' || p.s[p.pos] == 'E') {
		p.pos++
		if p.pos < len(p.s) && (p.s[p.pos] == '+' || p.s[p.pos] == '-') {
			p.pos++
		}
		if !p.digits() {
			return nil, errSyntax
		}
	}
	return &value{kind: kNumber, s: p.s[start:p.pos]}, nil
}

func hex4(s string) (rune, bool) {
	if len(s) < 4 {
		return 0, false
	}
	var r rune
	for i := 0; i < 4; i++ {
		c := s[i]
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

func (p *parser) str() (string, error) {
	p.pos++ // opening quote
	var sb strings.Builder
	for {
		if p.pos >= len(p.s) {
			return "", errSyntax
		}
		c := p.s[p.pos]
		switch {
		case c == '"':
			p.pos++
			return sb.String(), nil
		case c < 0x20:
			return "", errSyntax
		case c == '\\':
			p.pos++
			if p.pos >= len(p.s) {
				return "", errSyntax
			}
			e := p.s[p.pos]
			p.pos++
			switch e {
			case '"', '\\', '/':
				sb.WriteByte(e)
			case 'b':
				sb.WriteByte('\b')
			case 'f':
				sb.WriteByte('\f')
			case 'n':
				sb.WriteByte('\n')
			case 'r':
				sb.WriteByte('\r')
			case 't':
				sb.WriteByte('\t')
			case 'u':
				r, ok := hex4(p.s[p.pos:])
				if !ok {
					return "", errSyntax
				}
				p.pos += 4
				if utf16.IsSurrogate(r) {
					// Try to combine a surrogate pair; a lone surrogate
					// cannot be written in UTF-8 and becomes U+FFFD.
					if r < 0xDC00 && strings.HasPrefix(p.s[p.pos:], "\\u") {
						if r2, ok := hex4(p.s[p.pos+2:]); ok && r2 >= 0xDC00 && r2 < 0xE000 {
							p.pos += 6
							r = utf16.DecodeRune(r, r2)
						}
					}
					if utf16.IsSurrogate(r) {
						r = utf8.RuneError
					}
				}
				sb.WriteRune(r)
			default:
				return "", errSyntax
			}
		default:
			sb.WriteByte(c)
			p.pos++
		}
	}
}

func (p *parser) array() (*value, error) {
	p.pos++
	v := &value{kind: kArray, arr: []*value{}}
	p.ws()
	if p.pos < len(p.s) && p.s[p.pos] == ']' {
		p.pos++
		return v, nil
	}
	for {
		p.ws()
		e, err := p.value()
		if err != nil {
			return nil, err
		}
		v.arr = append(v.arr, e)
		p.ws()
		if p.pos >= len(p.s) {
			return nil, errSyntax
		}
		switch p.s[p.pos] {
		case ',':
			p.pos++
		case ']':
			p.pos++
			return v, nil
		default:
			return nil, errSyntax
		}
	}
}

func (p *parser) object() (*value, error) {
	p.pos++
	v := &value{kind: kObject, obj: map[string]*value{}}
	p.ws()
	if p.pos < len(p.s) && p.s[p.pos] == '}' {
		p.pos++
		return v, nil
	}
	for {
		p.ws()
		if p.pos >= len(p.s) || p.s[p.pos] != '"' {
			return nil, errSyntax
		}
		k, err := p.str()
		if err != nil {
			return nil, err
		}
		p.ws()
		if p.pos >= len(p.s) || p.s[p.pos] != ':' {
			return nil, errSyntax
		}
		p.pos++
		p.ws()
		e, err := p.value()
		if err != nil {
			return nil, err
		}
		v.obj[k] = e // a repeated key: the last one wins
		p.ws()
		if p.pos >= len(p.s) {
			return nil, errSyntax
		}
		switch p.s[p.pos] {
		case ',':
			p.pos++
		case '}':
			p.pos++
			return v, nil
		default:
			return nil, errSyntax
		}
	}
}

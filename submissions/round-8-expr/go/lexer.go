package main

import (
	"strconv"
	"strings"
	"unicode/utf16"
)

type tokKind int

const (
	tEOF tokKind = iota
	tInt
	tString
	tIdent
	tTrue
	tFalse
	tNull

	tLParen
	tRParen
	tLBracket
	tRBracket
	tComma
	tQuestion
	tColon

	tPlus
	tMinus
	tStar
	tSlash
	tPercent
	tBang
	tAndAnd
	tOrOr
	tEqEq
	tNotEq
	tLt
	tLtEq
	tGt
	tGtEq
)

type token struct {
	kind tokKind
	pos  int // 0-based rune offset in the source
	text string
	ival int64
}

func isIdentStart(c rune) bool {
	return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
}

func isIdentPart(c rune) bool {
	return isIdentStart(c) || (c >= '0' && c <= '9')
}

func isDigit(c rune) bool { return c >= '0' && c <= '9' }

func isHexDigit(c rune) bool {
	return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

func hexVal(c rune) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	default:
		return int(c-'A') + 10
	}
}

// scan tokenizes the whole source up front. Integer literal range errors are
// reported while scanning.
func scan(src []rune) ([]token, *ExprError) {
	var toks []token
	n := len(src)
	i := 0
	for i < n {
		c := src[i]
		if c == ' ' || c == '\t' || c == '\n' || c == '\r' {
			i++
			continue
		}
		pos := i
		switch {
		case isDigit(c):
			j := i
			for j < n && isDigit(src[j]) {
				j++
			}
			text := string(src[i:j])
			v, err := strconv.ParseInt(text, 10, 64)
			if err != nil {
				return nil, newErr(codeOverflow, pos, "integer literal out of i64 range")
			}
			toks = append(toks, token{kind: tInt, pos: pos, ival: v})
			i = j
		case c == '"':
			s, next, err := scanString(src, i)
			if err != nil {
				return nil, err
			}
			toks = append(toks, token{kind: tString, pos: pos, text: s})
			i = next
		case isIdentStart(c):
			j := i + 1
			for j < n && isIdentPart(src[j]) {
				j++
			}
			name := string(src[i:j])
			kind := tIdent
			switch name {
			case "true":
				kind = tTrue
			case "false":
				kind = tFalse
			case "null":
				kind = tNull
			}
			toks = append(toks, token{kind: kind, pos: pos, text: name})
			i = j
		case c == '(':
			toks = append(toks, token{kind: tLParen, pos: pos})
			i++
		case c == ')':
			toks = append(toks, token{kind: tRParen, pos: pos})
			i++
		case c == '[':
			toks = append(toks, token{kind: tLBracket, pos: pos})
			i++
		case c == ']':
			toks = append(toks, token{kind: tRBracket, pos: pos})
			i++
		case c == ',':
			toks = append(toks, token{kind: tComma, pos: pos})
			i++
		case c == '?':
			toks = append(toks, token{kind: tQuestion, pos: pos})
			i++
		case c == ':':
			toks = append(toks, token{kind: tColon, pos: pos})
			i++
		case c == '+':
			toks = append(toks, token{kind: tPlus, pos: pos})
			i++
		case c == '-':
			toks = append(toks, token{kind: tMinus, pos: pos})
			i++
		case c == '*':
			toks = append(toks, token{kind: tStar, pos: pos})
			i++
		case c == '/':
			toks = append(toks, token{kind: tSlash, pos: pos})
			i++
		case c == '%':
			toks = append(toks, token{kind: tPercent, pos: pos})
			i++
		case c == '!':
			if i+1 < n && src[i+1] == '=' {
				toks = append(toks, token{kind: tNotEq, pos: pos})
				i += 2
			} else {
				toks = append(toks, token{kind: tBang, pos: pos})
				i++
			}
		case c == '&':
			if i+1 < n && src[i+1] == '&' {
				toks = append(toks, token{kind: tAndAnd, pos: pos})
				i += 2
			} else {
				return nil, newErr(codeParse, pos, "unexpected character '&'")
			}
		case c == '|':
			if i+1 < n && src[i+1] == '|' {
				toks = append(toks, token{kind: tOrOr, pos: pos})
				i += 2
			} else {
				return nil, newErr(codeParse, pos, "unexpected character '|'")
			}
		case c == '=':
			if i+1 < n && src[i+1] == '=' {
				toks = append(toks, token{kind: tEqEq, pos: pos})
				i += 2
			} else {
				return nil, newErr(codeParse, pos, "unexpected character '='")
			}
		case c == '<':
			if i+1 < n && src[i+1] == '=' {
				toks = append(toks, token{kind: tLtEq, pos: pos})
				i += 2
			} else {
				toks = append(toks, token{kind: tLt, pos: pos})
				i++
			}
		case c == '>':
			if i+1 < n && src[i+1] == '=' {
				toks = append(toks, token{kind: tGtEq, pos: pos})
				i += 2
			} else {
				toks = append(toks, token{kind: tGt, pos: pos})
				i++
			}
		default:
			return nil, newErr(codeParse, pos, "unexpected character %q", string(c))
		}
	}
	toks = append(toks, token{kind: tEOF, pos: n})
	return toks, nil
}

// scanString scans a double-quoted string starting at src[start] == '"'.
func scanString(src []rune, start int) (string, int, *ExprError) {
	var b strings.Builder
	n := len(src)
	i := start + 1
	for {
		if i >= n {
			return "", n, newErr(codeParse, n, "unterminated string literal")
		}
		c := src[i]
		if c == '"' {
			return b.String(), i + 1, nil
		}
		if c != '\\' {
			b.WriteRune(c)
			i++
			continue
		}
		// Escape sequence.
		if i+1 >= n {
			return "", n, newErr(codeParse, n, "unterminated escape sequence")
		}
		e := src[i+1]
		switch e {
		case '"':
			b.WriteRune('"')
			i += 2
		case '\\':
			b.WriteRune('\\')
			i += 2
		case '/':
			b.WriteRune('/')
			i += 2
		case 'n':
			b.WriteRune('\n')
			i += 2
		case 't':
			b.WriteRune('\t')
			i += 2
		case 'r':
			b.WriteRune('\r')
			i += 2
		case 'b':
			b.WriteRune('\b')
			i += 2
		case 'f':
			b.WriteRune('\f')
			i += 2
		case 'u':
			var u rune
			for k := 0; k < 4; k++ {
				idx := i + 2 + k
				if idx >= n {
					return "", n, newErr(codeParse, n, "truncated \\u escape")
				}
				if !isHexDigit(src[idx]) {
					return "", start, newErr(codeParse, start, "invalid \\u escape")
				}
				u = u<<4 | rune(hexVal(src[idx]))
			}
			if utf16.IsSurrogate(u) {
				if u >= 0xD800 && u <= 0xDBFF {
					// A high surrogate must be followed by a low surrogate escape.
					if i+12 <= n && src[i+6] == '\\' && src[i+7] == 'u' &&
						isHexDigit(src[i+8]) && isHexDigit(src[i+9]) &&
						isHexDigit(src[i+10]) && isHexDigit(src[i+11]) {
						lo := rune(hexVal(src[i+8])<<12 | hexVal(src[i+9])<<8 | hexVal(src[i+10])<<4 | hexVal(src[i+11]))
						if lo >= 0xDC00 && lo <= 0xDFFF {
							b.WriteRune(utf16.DecodeRune(u, lo))
							i += 12
							continue
						}
					}
				}
				return "", start, newErr(codeParse, start, "invalid surrogate pair in string literal")
			}
			b.WriteRune(u)
			i += 6
		default:
			return "", start, newErr(codeParse, start, "invalid escape sequence '\\%c'", e)
		}
	}
}

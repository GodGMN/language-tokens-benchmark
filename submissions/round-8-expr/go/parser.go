package main

// AST node definitions.

type Node interface{ node() }

type IntLit struct {
	Val int64
	Pos int
}

type StrLit struct {
	Val string
	Pos int
}

type BoolLit struct {
	Val bool
	Pos int
}

type NullLit struct {
	Pos int
}

type Var struct {
	Name string
	Pos  int
}

type Unary struct {
	Op      tokKind
	Pos     int
	Operand Node
}

type Binary struct {
	Op    tokKind
	Pos   int
	Left  Node
	Right Node
}

type Ternary struct {
	Cond Node
	Then Node
	Else Node
	Pos  int // position of '?'
}

type ArrayLit struct {
	Elems []Node
}

type Call struct {
	Name string
	Pos  int
	Args []Node
}

func (*IntLit) node()   {}
func (*StrLit) node()   {}
func (*BoolLit) node()  {}
func (*NullLit) node()  {}
func (*Var) node()      {}
func (*Unary) node()    {}
func (*Binary) node()   {}
func (*Ternary) node()  {}
func (*ArrayLit) node() {}
func (*Call) node()     {}

// parser is a recursive-descent parser over the token slice.
type parser struct {
	toks   []token
	i      int
	srcLen int
}

func (p *parser) cur() token { return p.toks[p.i] }

func (p *parser) advance() token {
	t := p.toks[p.i]
	if p.i < len(p.toks)-1 {
		p.i++
	}
	return t
}

// unexpected builds a PARSE_ERROR for the current token (EOF uses len(source)).
func (p *parser) unexpected() *ExprError {
	t := p.cur()
	if t.kind == tEOF {
		return newErr(codeParse, t.pos, "unexpected end of input")
	}
	return newErr(codeParse, t.pos, "unexpected token")
}

func parseSource(source string) (Node, *ExprError) {
	src := []rune(source)
	toks, err := scan(src)
	if err != nil {
		return nil, err
	}
	p := &parser{toks: toks, srcLen: len(src)}
	n, err := p.parseExpr()
	if err != nil {
		return nil, err
	}
	if p.cur().kind != tEOF {
		return nil, p.unexpected()
	}
	return n, nil
}

// expr := ternary
func (p *parser) parseExpr() (Node, *ExprError) {
	cond, err := p.parseOr()
	if err != nil {
		return nil, err
	}
	if p.cur().kind != tQuestion {
		return cond, nil
	}
	q := p.advance()
	then, err := p.parseExpr()
	if err != nil {
		return nil, err
	}
	if p.cur().kind != tColon {
		return nil, p.unexpected()
	}
	p.advance()
	els, err := p.parseExpr()
	if err != nil {
		return nil, err
	}
	return &Ternary{Cond: cond, Then: then, Else: els, Pos: q.pos}, nil
}

// or := and ( "||" and )*
func (p *parser) parseOr() (Node, *ExprError) {
	left, err := p.parseAnd()
	if err != nil {
		return nil, err
	}
	for p.cur().kind == tOrOr {
		op := p.advance()
		right, err := p.parseAnd()
		if err != nil {
			return nil, err
		}
		left = &Binary{Op: op.kind, Pos: op.pos, Left: left, Right: right}
	}
	return left, nil
}

// and := equality ( "&&" equality )*
func (p *parser) parseAnd() (Node, *ExprError) {
	left, err := p.parseEquality()
	if err != nil {
		return nil, err
	}
	for p.cur().kind == tAndAnd {
		op := p.advance()
		right, err := p.parseEquality()
		if err != nil {
			return nil, err
		}
		left = &Binary{Op: op.kind, Pos: op.pos, Left: left, Right: right}
	}
	return left, nil
}

// equality := comparison ( ("==" | "!=") comparison )*
func (p *parser) parseEquality() (Node, *ExprError) {
	left, err := p.parseComparison()
	if err != nil {
		return nil, err
	}
	for p.cur().kind == tEqEq || p.cur().kind == tNotEq {
		op := p.advance()
		right, err := p.parseComparison()
		if err != nil {
			return nil, err
		}
		left = &Binary{Op: op.kind, Pos: op.pos, Left: left, Right: right}
	}
	return left, nil
}

// comparison := additive ( ("<" | "<=" | ">" | ">=") additive )*
func (p *parser) parseComparison() (Node, *ExprError) {
	left, err := p.parseAdditive()
	if err != nil {
		return nil, err
	}
	for p.cur().kind == tLt || p.cur().kind == tLtEq || p.cur().kind == tGt || p.cur().kind == tGtEq {
		op := p.advance()
		right, err := p.parseAdditive()
		if err != nil {
			return nil, err
		}
		left = &Binary{Op: op.kind, Pos: op.pos, Left: left, Right: right}
	}
	return left, nil
}

// additive := multiplicative ( ("+" | "-") multiplicative )*
func (p *parser) parseAdditive() (Node, *ExprError) {
	left, err := p.parseMultiplicative()
	if err != nil {
		return nil, err
	}
	for p.cur().kind == tPlus || p.cur().kind == tMinus {
		op := p.advance()
		right, err := p.parseMultiplicative()
		if err != nil {
			return nil, err
		}
		left = &Binary{Op: op.kind, Pos: op.pos, Left: left, Right: right}
	}
	return left, nil
}

// multiplicative := unary ( ("*" | "/" | "%") unary )*
func (p *parser) parseMultiplicative() (Node, *ExprError) {
	left, err := p.parseUnary()
	if err != nil {
		return nil, err
	}
	for p.cur().kind == tStar || p.cur().kind == tSlash || p.cur().kind == tPercent {
		op := p.advance()
		right, err := p.parseUnary()
		if err != nil {
			return nil, err
		}
		left = &Binary{Op: op.kind, Pos: op.pos, Left: left, Right: right}
	}
	return left, nil
}

// unary := ("-" | "!") unary | primary
func (p *parser) parseUnary() (Node, *ExprError) {
	switch p.cur().kind {
	case tMinus, tBang:
		op := p.advance()
		operand, err := p.parseUnary()
		if err != nil {
			return nil, err
		}
		return &Unary{Op: op.kind, Pos: op.pos, Operand: operand}, nil
	default:
		return p.parsePrimary()
	}
}

// primary := literal | call | identifier | "(" expr ")" | array
func (p *parser) parsePrimary() (Node, *ExprError) {
	t := p.cur()
	switch t.kind {
	case tInt:
		p.advance()
		return &IntLit{Val: t.ival, Pos: t.pos}, nil
	case tString:
		p.advance()
		return &StrLit{Val: t.text, Pos: t.pos}, nil
	case tTrue:
		p.advance()
		return &BoolLit{Val: true, Pos: t.pos}, nil
	case tFalse:
		p.advance()
		return &BoolLit{Val: false, Pos: t.pos}, nil
	case tNull:
		p.advance()
		return &NullLit{Pos: t.pos}, nil
	case tIdent:
		p.advance()
		if p.cur().kind != tLParen {
			return &Var{Name: t.text, Pos: t.pos}, nil
		}
		p.advance()
		var args []Node
		if p.cur().kind != tRParen {
			for {
				arg, err := p.parseExpr()
				if err != nil {
					return nil, err
				}
				args = append(args, arg)
				if p.cur().kind == tComma {
					p.advance()
					continue
				}
				break
			}
		}
		if p.cur().kind != tRParen {
			return nil, p.unexpected()
		}
		p.advance()
		return &Call{Name: t.text, Pos: t.pos, Args: args}, nil
	case tLParen:
		p.advance()
		inner, err := p.parseExpr()
		if err != nil {
			return nil, err
		}
		if p.cur().kind != tRParen {
			return nil, p.unexpected()
		}
		p.advance()
		return inner, nil
	case tLBracket:
		p.advance()
		var elems []Node
		if p.cur().kind != tRBracket {
			for {
				elem, err := p.parseExpr()
				if err != nil {
					return nil, err
				}
				elems = append(elems, elem)
				if p.cur().kind == tComma {
					p.advance()
					continue
				}
				break
			}
		}
		if p.cur().kind != tRBracket {
			return nil, p.unexpected()
		}
		p.advance()
		return &ArrayLit{Elems: elems}, nil
	default:
		return nil, p.unexpected()
	}
}

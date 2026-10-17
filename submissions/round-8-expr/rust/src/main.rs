//! expr: a small typed expression language CLI.
//!
//! Implementation is deliberately dependency-free: the JSON envelope parser,
//! expression lexer/parser, and evaluator are all written in this file.

use std::collections::HashMap;
use std::env;
use std::fs;
use std::io::{self, Read};
use std::process;

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
enum Value {
    Null,
    Bool(bool),
    Int(i64),
    Str(String),
    Array(Vec<Value>),
}

impl Value {
    fn type_name(&self) -> &'static str {
        match self {
            Value::Null => "null",
            Value::Bool(_) => "boolean",
            Value::Int(_) => "integer",
            Value::Str(_) => "string",
            Value::Array(_) => "array",
        }
    }
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
struct EError {
    code: &'static str,
    position: Option<usize>,
    message: String,
}

fn err(code: &'static str, position: usize, message: impl Into<String>) -> EError {
    EError {
        code,
        position: Some(position),
        message: message.into(),
    }
}

fn type_err(position: usize) -> EError {
    err("TYPE_ERROR", position, "wrong operand or argument type")
}

fn invalid_request(message: impl Into<String>) -> EError {
    EError {
        code: "INVALID_REQUEST",
        position: None,
        message: message.into(),
    }
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq)]
enum Tok {
    Int(i64),
    Str(String),
    Ident(String),
    True,
    False,
    Null,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    EqEq,
    NotEq,
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,
    OrOr,
    Bang,
    Question,
    Colon,
    Comma,
    LParen,
    RParen,
    LBracket,
    RBracket,
    Eof,
}

#[derive(Clone, Debug)]
struct Token {
    kind: Tok,
    pos: usize, // 0-based character offset (not byte offset)
}

fn is_ident_start(c: char) -> bool {
    c.is_ascii_alphabetic() || c == '_'
}

fn is_ident_continue(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '_'
}

fn hex_val(c: char) -> Option<u32> {
    match c {
        '0'..='9' => Some(c as u32 - '0' as u32),
        'a'..='f' => Some(c as u32 - 'a' as u32 + 10),
        'A'..='F' => Some(c as u32 - 'A' as u32 + 10),
        _ => None,
    }
}

fn read_hex4(chars: &[char], start: usize) -> Option<u32> {
    if start + 4 > chars.len() {
        return None;
    }
    let mut v = 0u32;
    for k in 0..4 {
        v = (v << 4) | hex_val(chars[start + k])?;
    }
    Some(v)
}

/// Lex a double-quoted string beginning at `start` (which indexes the opening
/// quote). Returns the decoded string and the index just after the closing
/// quote.
fn lex_string(chars: &[char], start: usize) -> Result<(String, usize), EError> {
    let n = chars.len();
    let perr = |msg: &str| err("PARSE_ERROR", start, msg);
    let mut i = start + 1;
    let mut out = String::new();

    loop {
        if i >= n {
            return Err(perr("unterminated string literal"));
        }
        let c = chars[i];
        if c == '"' {
            return Ok((out, i + 1));
        }
        if c != '\\' {
            out.push(c);
            i += 1;
            continue;
        }

        // Escape sequence.
        i += 1;
        if i >= n {
            return Err(perr("unterminated escape sequence"));
        }
        let e = chars[i];
        match e {
            '"' => {
                out.push('"');
                i += 1;
            }
            '\\' => {
                out.push('\\');
                i += 1;
            }
            '/' => {
                out.push('/');
                i += 1;
            }
            'n' => {
                out.push('\n');
                i += 1;
            }
            't' => {
                out.push('\t');
                i += 1;
            }
            'r' => {
                out.push('\r');
                i += 1;
            }
            'b' => {
                out.push('\u{0008}');
                i += 1;
            }
            'f' => {
                out.push('\u{000C}');
                i += 1;
            }
            'u' => {
                i += 1;
                let cu = match read_hex4(chars, i) {
                    Some(v) => v,
                    None => return Err(perr("invalid \\u escape")),
                };
                i += 4;

                if (0xD800..=0xDBFF).contains(&cu) {
                    // High surrogate: require a matching low surrogate.
                    if i + 1 < n && chars[i] == '\\' && chars[i + 1] == 'u' {
                        i += 2;
                        let lo = match read_hex4(chars, i) {
                            Some(v) => v,
                            None => return Err(perr("invalid surrogate pair")),
                        };
                        i += 4;
                        if !(0xDC00..=0xDFFF).contains(&lo) {
                            return Err(perr("invalid surrogate pair"));
                        }
                        let cp = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                        match char::from_u32(cp) {
                            Some(ch) => out.push(ch),
                            None => return Err(perr("invalid unicode code point")),
                        }
                    } else {
                        return Err(perr("unpaired high surrogate"));
                    }
                } else if (0xDC00..=0xDFFF).contains(&cu) {
                    return Err(perr("unpaired low surrogate"));
                } else {
                    match char::from_u32(cu) {
                        Some(ch) => out.push(ch),
                        None => return Err(perr("invalid unicode code point")),
                    }
                }
            }
            _ => return Err(perr("invalid escape sequence")),
        }
    }
}

fn tokenize(src: &str) -> Result<Vec<Token>, EError> {
    let chars: Vec<char> = src.chars().collect();
    let n = chars.len();
    let mut i = 0usize;
    let mut tokens: Vec<Token> = Vec::new();

    while i < n {
        let c = chars[i];
        // Source whitespace is exactly space, tab, newline, carriage return.
        if c == ' ' || c == '\t' || c == '\n' || c == '\r' {
            i += 1;
            continue;
        }

        let pos = i;
        match c {
            '0'..='9' => {
                let mut val: i64 = 0;
                while i < n && chars[i].is_ascii_digit() {
                    let d = chars[i] as i64 - '0' as i64;
                    val = match val.checked_mul(10).and_then(|v| v.checked_add(d)) {
                        Some(v) => v,
                        None => {
                            return Err(err(
                                "OVERFLOW",
                                pos,
                                "integer literal is outside the i64 range",
                            ))
                        }
                    };
                    i += 1;
                }
                tokens.push(Token {
                    kind: Tok::Int(val),
                    pos,
                });
            }
            '"' => {
                let (s, next) = lex_string(&chars, i)?;
                tokens.push(Token {
                    kind: Tok::Str(s),
                    pos,
                });
                i = next;
            }
            c if is_ident_start(c) => {
                let start = i;
                while i < n && is_ident_continue(chars[i]) {
                    i += 1;
                }
                let name: String = chars[start..i].iter().collect();
                let kind = match name.as_str() {
                    "true" => Tok::True,
                    "false" => Tok::False,
                    "null" => Tok::Null,
                    _ => Tok::Ident(name),
                };
                tokens.push(Token { kind, pos: start });
            }
            '+' => {
                tokens.push(Token {
                    kind: Tok::Plus,
                    pos,
                });
                i += 1;
            }
            '-' => {
                tokens.push(Token {
                    kind: Tok::Minus,
                    pos,
                });
                i += 1;
            }
            '*' => {
                tokens.push(Token {
                    kind: Tok::Star,
                    pos,
                });
                i += 1;
            }
            '/' => {
                tokens.push(Token {
                    kind: Tok::Slash,
                    pos,
                });
                i += 1;
            }
            '%' => {
                tokens.push(Token {
                    kind: Tok::Percent,
                    pos,
                });
                i += 1;
            }
            '!' => {
                if i + 1 < n && chars[i + 1] == '=' {
                    tokens.push(Token {
                        kind: Tok::NotEq,
                        pos,
                    });
                    i += 2;
                } else {
                    tokens.push(Token {
                        kind: Tok::Bang,
                        pos,
                    });
                    i += 1;
                }
            }
            '=' => {
                if i + 1 < n && chars[i + 1] == '=' {
                    tokens.push(Token {
                        kind: Tok::EqEq,
                        pos,
                    });
                    i += 2;
                } else {
                    return Err(err("PARSE_ERROR", pos, "unexpected '='"));
                }
            }
            '<' => {
                if i + 1 < n && chars[i + 1] == '=' {
                    tokens.push(Token {
                        kind: Tok::Le,
                        pos,
                    });
                    i += 2;
                } else {
                    tokens.push(Token {
                        kind: Tok::Lt,
                        pos,
                    });
                    i += 1;
                }
            }
            '>' => {
                if i + 1 < n && chars[i + 1] == '=' {
                    tokens.push(Token {
                        kind: Tok::Ge,
                        pos,
                    });
                    i += 2;
                } else {
                    tokens.push(Token {
                        kind: Tok::Gt,
                        pos,
                    });
                    i += 1;
                }
            }
            '&' => {
                if i + 1 < n && chars[i + 1] == '&' {
                    tokens.push(Token {
                        kind: Tok::AndAnd,
                        pos,
                    });
                    i += 2;
                } else {
                    return Err(err("PARSE_ERROR", pos, "unexpected '&'"));
                }
            }
            '|' => {
                if i + 1 < n && chars[i + 1] == '|' {
                    tokens.push(Token {
                        kind: Tok::OrOr,
                        pos,
                    });
                    i += 2;
                } else {
                    return Err(err("PARSE_ERROR", pos, "unexpected '|'"));
                }
            }
            '?' => {
                tokens.push(Token {
                    kind: Tok::Question,
                    pos,
                });
                i += 1;
            }
            ':' => {
                tokens.push(Token {
                    kind: Tok::Colon,
                    pos,
                });
                i += 1;
            }
            ',' => {
                tokens.push(Token {
                    kind: Tok::Comma,
                    pos,
                });
                i += 1;
            }
            '(' => {
                tokens.push(Token {
                    kind: Tok::LParen,
                    pos,
                });
                i += 1;
            }
            ')' => {
                tokens.push(Token {
                    kind: Tok::RParen,
                    pos,
                });
                i += 1;
            }
            '[' => {
                tokens.push(Token {
                    kind: Tok::LBracket,
                    pos,
                });
                i += 1;
            }
            ']' => {
                tokens.push(Token {
                    kind: Tok::RBracket,
                    pos,
                });
                i += 1;
            }
            _ => {
                return Err(err(
                    "PARSE_ERROR",
                    pos,
                    format!("unexpected character '{}'", c),
                ));
            }
        }
    }

    tokens.push(Token {
        kind: Tok::Eof,
        pos: n,
    });
    Ok(tokens)
}

// ---------------------------------------------------------------------------
// AST and parser
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug)]
enum UnOp {
    Neg,
    Not,
}

#[derive(Clone, Copy, Debug)]
enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    And,
    Or,
}

#[derive(Clone, Debug)]
enum Expr {
    Lit(Value),
    Var(String, usize),
    Array(Vec<Expr>),
    Unary(UnOp, Box<Expr>, usize),
    Binary(BinOp, Box<Expr>, Box<Expr>, usize),
    Ternary {
        cond: Box<Expr>,
        then_branch: Box<Expr>,
        else_branch: Box<Expr>,
        qpos: usize,
    },
    Call {
        name: String,
        args: Vec<Expr>,
        pos: usize,
    },
}

struct Parser {
    tokens: Vec<Token>,
    idx: usize,
}

impl Parser {
    fn new(tokens: Vec<Token>) -> Parser {
        Parser { tokens, idx: 0 }
    }

    fn peek(&self) -> &Tok {
        &self.tokens[self.idx].kind
    }

    fn peek_pos(&self) -> usize {
        self.tokens[self.idx].pos
    }

    fn parse_error_here(&self, msg: &str) -> EError {
        err("PARSE_ERROR", self.peek_pos(), msg)
    }

    fn expect(&mut self, kind: Tok, what: &str) -> Result<(), EError> {
        if *self.peek() == kind {
            self.idx += 1;
            Ok(())
        } else {
            Err(self.parse_error_here(&format!("expected {}", what)))
        }
    }

    fn parse(&mut self) -> Result<Expr, EError> {
        let e = self.parse_expr()?;
        if *self.peek() != Tok::Eof {
            return Err(self.parse_error_here("unexpected token after expression"));
        }
        Ok(e)
    }

    fn parse_expr(&mut self) -> Result<Expr, EError> {
        self.parse_ternary()
    }

    // ternary := or ( "?" expr ":" expr )?
    fn parse_ternary(&mut self) -> Result<Expr, EError> {
        let cond = self.parse_or()?;
        if *self.peek() == Tok::Question {
            let qpos = self.peek_pos();
            self.idx += 1;
            let then_branch = self.parse_expr()?;
            self.expect(Tok::Colon, "':'")?;
            let else_branch = self.parse_expr()?;
            Ok(Expr::Ternary {
                cond: Box::new(cond),
                then_branch: Box::new(then_branch),
                else_branch: Box::new(else_branch),
                qpos,
            })
        } else {
            Ok(cond)
        }
    }

    // or := and ( "||" and )*
    fn parse_or(&mut self) -> Result<Expr, EError> {
        let mut left = self.parse_and()?;
        while *self.peek() == Tok::OrOr {
            let pos = self.peek_pos();
            self.idx += 1;
            let right = self.parse_and()?;
            left = Expr::Binary(BinOp::Or, Box::new(left), Box::new(right), pos);
        }
        Ok(left)
    }

    // and := equality ( "&&" equality )*
    fn parse_and(&mut self) -> Result<Expr, EError> {
        let mut left = self.parse_equality()?;
        while *self.peek() == Tok::AndAnd {
            let pos = self.peek_pos();
            self.idx += 1;
            let right = self.parse_equality()?;
            left = Expr::Binary(BinOp::And, Box::new(left), Box::new(right), pos);
        }
        Ok(left)
    }

    // equality := comparison ( ("==" | "!=") comparison )*
    fn parse_equality(&mut self) -> Result<Expr, EError> {
        let mut left = self.parse_comparison()?;
        loop {
            let op = match self.peek() {
                Tok::EqEq => BinOp::Eq,
                Tok::NotEq => BinOp::Ne,
                _ => break,
            };
            let pos = self.peek_pos();
            self.idx += 1;
            let right = self.parse_comparison()?;
            left = Expr::Binary(op, Box::new(left), Box::new(right), pos);
        }
        Ok(left)
    }

    // comparison := additive ( ("<" | "<=" | ">" | ">=") additive )*
    fn parse_comparison(&mut self) -> Result<Expr, EError> {
        let mut left = self.parse_additive()?;
        loop {
            let op = match self.peek() {
                Tok::Lt => BinOp::Lt,
                Tok::Le => BinOp::Le,
                Tok::Gt => BinOp::Gt,
                Tok::Ge => BinOp::Ge,
                _ => break,
            };
            let pos = self.peek_pos();
            self.idx += 1;
            let right = self.parse_additive()?;
            left = Expr::Binary(op, Box::new(left), Box::new(right), pos);
        }
        Ok(left)
    }

    // additive := multiplicative ( ("+" | "-") multiplicative )*
    fn parse_additive(&mut self) -> Result<Expr, EError> {
        let mut left = self.parse_multiplicative()?;
        loop {
            let op = match self.peek() {
                Tok::Plus => BinOp::Add,
                Tok::Minus => BinOp::Sub,
                _ => break,
            };
            let pos = self.peek_pos();
            self.idx += 1;
            let right = self.parse_multiplicative()?;
            left = Expr::Binary(op, Box::new(left), Box::new(right), pos);
        }
        Ok(left)
    }

    // multiplicative := unary ( ("*" | "/" | "%") unary )*
    fn parse_multiplicative(&mut self) -> Result<Expr, EError> {
        let mut left = self.parse_unary()?;
        loop {
            let op = match self.peek() {
                Tok::Star => BinOp::Mul,
                Tok::Slash => BinOp::Div,
                Tok::Percent => BinOp::Rem,
                _ => break,
            };
            let pos = self.peek_pos();
            self.idx += 1;
            let right = self.parse_unary()?;
            left = Expr::Binary(op, Box::new(left), Box::new(right), pos);
        }
        Ok(left)
    }

    // unary := ("-" | "!") unary | primary
    fn parse_unary(&mut self) -> Result<Expr, EError> {
        match self.peek() {
            Tok::Minus => {
                let pos = self.peek_pos();
                self.idx += 1;
                let inner = self.parse_unary()?;
                Ok(Expr::Unary(UnOp::Neg, Box::new(inner), pos))
            }
            Tok::Bang => {
                let pos = self.peek_pos();
                self.idx += 1;
                let inner = self.parse_unary()?;
                Ok(Expr::Unary(UnOp::Not, Box::new(inner), pos))
            }
            _ => self.parse_primary(),
        }
    }

    // primary := integer | string | "true" | "false" | "null"
    //          | identifier "(" arglist? ")" | identifier
    //          | "(" expr ")" | "[" (expr ("," expr)*)? "]"
    fn parse_primary(&mut self) -> Result<Expr, EError> {
        let pos = self.peek_pos();
        let kind = self.peek().clone();
        match kind {
            Tok::Int(i) => {
                self.idx += 1;
                Ok(Expr::Lit(Value::Int(i)))
            }
            Tok::Str(s) => {
                self.idx += 1;
                Ok(Expr::Lit(Value::Str(s)))
            }
            Tok::True => {
                self.idx += 1;
                Ok(Expr::Lit(Value::Bool(true)))
            }
            Tok::False => {
                self.idx += 1;
                Ok(Expr::Lit(Value::Bool(false)))
            }
            Tok::Null => {
                self.idx += 1;
                Ok(Expr::Lit(Value::Null))
            }
            Tok::Ident(name) => {
                self.idx += 1;
                if *self.peek() != Tok::LParen {
                    return Ok(Expr::Var(name, pos));
                }
                // Function call.
                self.idx += 1;
                let mut args = Vec::new();
                if *self.peek() == Tok::RParen {
                    self.idx += 1;
                } else {
                    loop {
                        args.push(self.parse_expr()?);
                        if *self.peek() == Tok::Comma {
                            self.idx += 1;
                            continue;
                        }
                        if *self.peek() == Tok::RParen {
                            self.idx += 1;
                            break;
                        }
                        return Err(self.parse_error_here("expected ',' or ')'"));
                    }
                }
                Ok(Expr::Call { name, args, pos })
            }
            Tok::LParen => {
                self.idx += 1;
                let inner = self.parse_expr()?;
                self.expect(Tok::RParen, "')'")?;
                Ok(inner)
            }
            Tok::LBracket => {
                self.idx += 1;
                let mut items = Vec::new();
                if *self.peek() == Tok::RBracket {
                    self.idx += 1;
                } else {
                    loop {
                        items.push(self.parse_expr()?);
                        if *self.peek() == Tok::Comma {
                            self.idx += 1;
                            continue;
                        }
                        if *self.peek() == Tok::RBracket {
                            self.idx += 1;
                            break;
                        }
                        return Err(self.parse_error_here("expected ',' or ']'"));
                    }
                }
                Ok(Expr::Array(items))
            }
            Tok::Eof => Err(self.parse_error_here("unexpected end of input")),
            _ => Err(self.parse_error_here("expected an expression")),
        }
    }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

fn eval(expr: &Expr, vars: &HashMap<String, Value>) -> Result<Value, EError> {
    match expr {
        Expr::Lit(v) => Ok(v.clone()),
        Expr::Var(name, pos) => match vars.get(name) {
            Some(v) => Ok(v.clone()),
            None => Err(err(
                "UNKNOWN_VARIABLE",
                *pos,
                format!("unknown variable '{}'", name),
            )),
        },
        Expr::Array(items) => {
            let mut out = Vec::with_capacity(items.len());
            for item in items {
                out.push(eval(item, vars)?);
            }
            Ok(Value::Array(out))
        }
        Expr::Unary(op, inner, pos) => {
            let v = eval(inner, vars)?;
            match op {
                UnOp::Neg => match v {
                    Value::Int(i) => match i.checked_neg() {
                        Some(r) => Ok(Value::Int(r)),
                        None => Err(err("OVERFLOW", *pos, "integer overflow")),
                    },
                    _ => Err(type_err(*pos)),
                },
                UnOp::Not => match v {
                    Value::Bool(b) => Ok(Value::Bool(!b)),
                    _ => Err(type_err(*pos)),
                },
            }
        }
        Expr::Binary(op, left, right, pos) => eval_binary(*op, left, right, *pos, vars),
        Expr::Ternary {
            cond,
            then_branch,
            else_branch,
            qpos,
        } => {
            let c = eval(cond, vars)?;
            match c {
                Value::Bool(true) => eval(then_branch, vars),
                Value::Bool(false) => eval(else_branch, vars),
                _ => Err(err(
                    "TYPE_ERROR",
                    *qpos,
                    "ternary condition must be boolean",
                )),
            }
        }
        Expr::Call { name, args, pos } => {
            let mut values = Vec::with_capacity(args.len());
            for a in args {
                values.push(eval(a, vars)?);
            }
            call_function(name, &values, *pos)
        }
    }
}

fn eval_binary(
    op: BinOp,
    left: &Expr,
    right: &Expr,
    pos: usize,
    vars: &HashMap<String, Value>,
) -> Result<Value, EError> {
    let lv = eval(left, vars)?;

    // Logical operators short-circuit and check boolean operands lazily.
    match op {
        BinOp::And => {
            let lb = match lv {
                Value::Bool(b) => b,
                _ => return Err(type_err(pos)),
            };
            if !lb {
                return Ok(Value::Bool(false));
            }
            return match eval(right, vars)? {
                Value::Bool(rb) => Ok(Value::Bool(rb)),
                _ => Err(type_err(pos)),
            };
        }
        BinOp::Or => {
            let lb = match lv {
                Value::Bool(b) => b,
                _ => return Err(type_err(pos)),
            };
            if lb {
                return Ok(Value::Bool(true));
            }
            return match eval(right, vars)? {
                Value::Bool(rb) => Ok(Value::Bool(rb)),
                _ => Err(type_err(pos)),
            };
        }
        _ => {}
    }

    let rv = eval(right, vars)?;

    match op {
        BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Rem => {
            let a = match lv {
                Value::Int(i) => i,
                _ => return Err(type_err(pos)),
            };
            let b = match rv {
                Value::Int(i) => i,
                _ => return Err(type_err(pos)),
            };
            let result = match op {
                BinOp::Add => a.checked_add(b),
                BinOp::Sub => a.checked_sub(b),
                BinOp::Mul => a.checked_mul(b),
                BinOp::Div => {
                    if b == 0 {
                        return Err(err("DIVISION_BY_ZERO", pos, "division by zero"));
                    }
                    a.checked_div(b)
                }
                BinOp::Rem => {
                    if b == 0 {
                        return Err(err("DIVISION_BY_ZERO", pos, "modulo by zero"));
                    }
                    a.checked_rem(b)
                }
                _ => unreachable!(),
            };
            match result {
                Some(v) => Ok(Value::Int(v)),
                None => Err(err("OVERFLOW", pos, "integer overflow")),
            }
        }
        BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => match (&lv, &rv) {
            (Value::Int(a), Value::Int(b)) => {
                let r = match op {
                    BinOp::Lt => a < b,
                    BinOp::Le => a <= b,
                    BinOp::Gt => a > b,
                    BinOp::Ge => a >= b,
                    _ => unreachable!(),
                };
                Ok(Value::Bool(r))
            }
            (Value::Str(a), Value::Str(b)) => {
                let r = match op {
                    BinOp::Lt => a.as_bytes() < b.as_bytes(),
                    BinOp::Le => a.as_bytes() <= b.as_bytes(),
                    BinOp::Gt => a.as_bytes() > b.as_bytes(),
                    BinOp::Ge => a.as_bytes() >= b.as_bytes(),
                    _ => unreachable!(),
                };
                Ok(Value::Bool(r))
            }
            _ => Err(type_err(pos)),
        },
        BinOp::Eq => Ok(Value::Bool(lv == rv)),
        BinOp::Ne => Ok(Value::Bool(lv != rv)),
        BinOp::And | BinOp::Or => unreachable!(),
    }
}

// ---------------------------------------------------------------------------
// Built-in functions
// ---------------------------------------------------------------------------

fn need_arity(args: &[Value], n: usize, pos: usize) -> Result<(), EError> {
    if args.len() == n {
        Ok(())
    } else {
        Err(err(
            "ARITY_ERROR",
            pos,
            format!("expected {} argument(s), got {}", n, args.len()),
        ))
    }
}

fn call_function(name: &str, args: &[Value], pos: usize) -> Result<Value, EError> {
    match name {
        "len" => {
            need_arity(args, 1, pos)?;
            match &args[0] {
                Value::Str(s) => Ok(Value::Int(s.chars().count() as i64)),
                Value::Array(a) => Ok(Value::Int(a.len() as i64)),
                _ => Err(type_err(pos)),
            }
        }
        "sum" => {
            need_arity(args, 1, pos)?;
            match &args[0] {
                Value::Array(items) => {
                    if !items.iter().all(|v| matches!(v, Value::Int(_))) {
                        return Err(type_err(pos));
                    }
                    let mut total: i64 = 0;
                    for v in items {
                        if let Value::Int(i) = v {
                            total = match total.checked_add(*i) {
                                Some(t) => t,
                                None => return Err(err("OVERFLOW", pos, "integer overflow")),
                            };
                        }
                    }
                    Ok(Value::Int(total))
                }
                _ => Err(type_err(pos)),
            }
        }
        "min" => {
            need_arity(args, 1, pos)?;
            match &args[0] {
                Value::Array(items) => {
                    if items.is_empty() {
                        return Err(err("EMPTY_ARRAY", pos, "min of an empty array"));
                    }
                    if !items.iter().all(|v| matches!(v, Value::Int(_))) {
                        return Err(type_err(pos));
                    }
                    let mut best: Option<i64> = None;
                    for v in items {
                        if let Value::Int(i) = v {
                            best = Some(match best {
                                None => *i,
                                Some(b) => {
                                    if *i < b {
                                        *i
                                    } else {
                                        b
                                    }
                                }
                            });
                        }
                    }
                    Ok(Value::Int(best.unwrap()))
                }
                _ => Err(type_err(pos)),
            }
        }
        "max" => {
            need_arity(args, 1, pos)?;
            match &args[0] {
                Value::Array(items) => {
                    if items.is_empty() {
                        return Err(err("EMPTY_ARRAY", pos, "max of an empty array"));
                    }
                    if !items.iter().all(|v| matches!(v, Value::Int(_))) {
                        return Err(type_err(pos));
                    }
                    let mut best: Option<i64> = None;
                    for v in items {
                        if let Value::Int(i) = v {
                            best = Some(match best {
                                None => *i,
                                Some(b) => {
                                    if *i > b {
                                        *i
                                    } else {
                                        b
                                    }
                                }
                            });
                        }
                    }
                    Ok(Value::Int(best.unwrap()))
                }
                _ => Err(type_err(pos)),
            }
        }
        "abs" => {
            need_arity(args, 1, pos)?;
            match args[0] {
                Value::Int(i) => match i.checked_abs() {
                    Some(v) => Ok(Value::Int(v)),
                    None => Err(err("OVERFLOW", pos, "integer overflow")),
                },
                _ => Err(type_err(pos)),
            }
        }
        "neg" => {
            need_arity(args, 1, pos)?;
            match args[0] {
                Value::Int(i) => match i.checked_neg() {
                    Some(v) => Ok(Value::Int(v)),
                    None => Err(err("OVERFLOW", pos, "integer overflow")),
                },
                _ => Err(type_err(pos)),
            }
        }
        "upper" => {
            need_arity(args, 1, pos)?;
            match &args[0] {
                Value::Str(s) => Ok(Value::Str(s.to_ascii_uppercase())),
                _ => Err(type_err(pos)),
            }
        }
        "lower" => {
            need_arity(args, 1, pos)?;
            match &args[0] {
                Value::Str(s) => Ok(Value::Str(s.to_ascii_lowercase())),
                _ => Err(type_err(pos)),
            }
        }
        "trim" => {
            need_arity(args, 1, pos)?;
            match &args[0] {
                Value::Str(s) => Ok(Value::Str(
                    s.trim_matches(|c| {
                        matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{000C}' | '\u{000B}')
                    })
                    .to_string(),
                )),
                _ => Err(type_err(pos)),
            }
        }
        "contains" => {
            need_arity(args, 2, pos)?;
            match (&args[0], &args[1]) {
                (Value::Str(s), Value::Str(sub)) => Ok(Value::Bool(s.contains(sub.as_str()))),
                _ => Err(type_err(pos)),
            }
        }
        "starts_with" => {
            need_arity(args, 2, pos)?;
            match (&args[0], &args[1]) {
                (Value::Str(s), Value::Str(p)) => Ok(Value::Bool(s.starts_with(p.as_str()))),
                _ => Err(type_err(pos)),
            }
        }
        "ends_with" => {
            need_arity(args, 2, pos)?;
            match (&args[0], &args[1]) {
                (Value::Str(s), Value::Str(p)) => Ok(Value::Bool(s.ends_with(p.as_str()))),
                _ => Err(type_err(pos)),
            }
        }
        "substr" => {
            need_arity(args, 3, pos)?;
            let s = match &args[0] {
                Value::Str(s) => s,
                _ => return Err(type_err(pos)),
            };
            let start = match &args[1] {
                Value::Int(i) => *i,
                _ => return Err(type_err(pos)),
            };
            let len = match &args[2] {
                Value::Int(i) => *i,
                _ => return Err(type_err(pos)),
            };
            if len < 0 {
                return Err(type_err(pos));
            }
            let chars: Vec<char> = s.chars().collect();
            let n = chars.len() as i128;
            let mut st = start as i128;
            if st < 0 {
                st = n + st;
            }
            if st < 0 {
                st = 0;
            }
            if st > n {
                st = n;
            }
            let mut en = st + (len as i128);
            if en > n {
                en = n;
            }
            if en < st {
                en = st;
            }
            let out: String = chars[st as usize..en as usize].iter().collect();
            Ok(Value::Str(out))
        }
        "index_of" => {
            need_arity(args, 2, pos)?;
            match (&args[0], &args[1]) {
                (Value::Str(s), Value::Str(sub)) => match s.find(sub.as_str()) {
                    Some(byte_idx) => Ok(Value::Int(s[..byte_idx].chars().count() as i64)),
                    None => Ok(Value::Int(-1)),
                },
                _ => Err(type_err(pos)),
            }
        }
        "join" => {
            need_arity(args, 2, pos)?;
            let items = match &args[0] {
                Value::Array(a) => a,
                _ => return Err(type_err(pos)),
            };
            let sep = match &args[1] {
                Value::Str(s) => s,
                _ => return Err(type_err(pos)),
            };
            let mut parts: Vec<&str> = Vec::with_capacity(items.len());
            for item in items {
                match item {
                    Value::Str(s) => parts.push(s.as_str()),
                    _ => return Err(type_err(pos)),
                }
            }
            Ok(Value::Str(parts.join(sep)))
        }
        "split" => {
            need_arity(args, 2, pos)?;
            let s = match &args[0] {
                Value::Str(s) => s,
                _ => return Err(type_err(pos)),
            };
            let sep = match &args[1] {
                Value::Str(s) => s,
                _ => return Err(type_err(pos)),
            };
            if sep.is_empty() {
                Ok(Value::Array(
                    s.chars().map(|c| Value::Str(c.to_string())).collect(),
                ))
            } else {
                Ok(Value::Array(
                    s.split(sep.as_str()).map(|p| Value::Str(p.to_string())).collect(),
                ))
            }
        }
        "type" => {
            need_arity(args, 1, pos)?;
            Ok(Value::Str(args[0].type_name().to_string()))
        }
        "to_string" => {
            need_arity(args, 1, pos)?;
            Ok(Value::Str(value_to_string(&args[0])))
        }
        _ => Err(err(
            "UNKNOWN_FUNCTION",
            pos,
            format!("unknown function '{}'", name),
        )),
    }
}

fn value_to_string(v: &Value) -> String {
    match v {
        Value::Null => "null".to_string(),
        Value::Bool(b) => {
            if *b {
                "true".to_string()
            } else {
                "false".to_string()
            }
        }
        Value::Int(i) => i.to_string(),
        Value::Str(s) => s.clone(),
        Value::Array(_) => {
            let mut out = String::new();
            write_json_value(v, &mut out);
            out
        }
    }
}

// ---------------------------------------------------------------------------
// JSON writing (compact, deterministic)
// ---------------------------------------------------------------------------

fn write_json_string(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{0008}' => out.push_str("\\b"),
            '\u{000C}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                out.push_str("\\u");
                let code = c as u32;
                for shift in [12, 8, 4, 0] {
                    let digit = ((code >> shift) & 0xF) as u8;
                    let ch = if digit < 10 {
                        (b'0' + digit) as char
                    } else {
                        (b'a' + digit - 10) as char
                    };
                    out.push(ch);
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn write_json_value(v: &Value, out: &mut String) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(true) => out.push_str("true"),
        Value::Bool(false) => out.push_str("false"),
        Value::Int(i) => out.push_str(&i.to_string()),
        Value::Str(s) => write_json_string(s, out),
        Value::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i != 0 {
                    out.push(',');
                }
                write_json_value(item, out);
            }
            out.push(']');
        }
    }
}

fn emit_success(v: &Value) {
    let mut out = String::from("{\"status\":\"ok\",\"value\":");
    write_json_value(v, &mut out);
    out.push('}');
    println!("{}", out);
}

fn emit_error(e: &EError) {
    let pos = match e.position {
        Some(p) => p.to_string(),
        None => "null".to_string(),
    };
    let mut out = String::new();
    out.push_str("{\"status\":\"error\",\"code\":\"");
    out.push_str(e.code);
    out.push_str("\",\"position\":");
    out.push_str(&pos);
    out.push_str(",\"message\":");
    write_json_string(&e.message, &mut out);
    out.push('}');
    println!("{}", out);
}

// ---------------------------------------------------------------------------
// Minimal JSON reader for the request envelope
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
enum Json {
    Null,
    Bool(bool),
    Number(Option<i64>), // Some => integral and fits i64
    Str(String),
    Array(Vec<Json>),
    Object(Vec<(String, Json)>),
}

struct JsonParser {
    chars: Vec<char>,
    i: usize,
}

impl JsonParser {
    fn new(src: &str) -> JsonParser {
        JsonParser {
            chars: src.chars().collect(),
            i: 0,
        }
    }

    fn skip_ws(&mut self) {
        while self.i < self.chars.len() {
            match self.chars[self.i] {
                ' ' | '\t' | '\n' | '\r' => self.i += 1,
                _ => break,
            }
        }
    }

    fn peek(&self) -> Option<char> {
        self.chars.get(self.i).copied()
    }

    fn parse_document(&mut self) -> Result<Json, ()> {
        self.skip_ws();
        let v = self.parse_value()?;
        self.skip_ws();
        if self.i != self.chars.len() {
            return Err(());
        }
        Ok(v)
    }

    fn parse_value(&mut self) -> Result<Json, ()> {
        self.skip_ws();
        let c = self.peek().ok_or(())?;
        match c {
            '{' => self.parse_object(),
            '[' => self.parse_array(),
            '"' => Ok(Json::Str(self.parse_string()?)),
            't' => {
                self.expect_word("true")?;
                Ok(Json::Bool(true))
            }
            'f' => {
                self.expect_word("false")?;
                Ok(Json::Bool(false))
            }
            'n' => {
                self.expect_word("null")?;
                Ok(Json::Null)
            }
            '-' | '0'..='9' => self.parse_number(),
            _ => Err(()),
        }
    }

    fn expect_word(&mut self, word: &str) -> Result<(), ()> {
        for expected in word.chars() {
            if self.peek() != Some(expected) {
                return Err(());
            }
            self.i += 1;
        }
        Ok(())
    }

    fn parse_object(&mut self) -> Result<Json, ()> {
        if self.peek() != Some('{') {
            return Err(());
        }
        self.i += 1;
        let mut fields: Vec<(String, Json)> = Vec::new();
        self.skip_ws();
        if self.peek() == Some('}') {
            self.i += 1;
            return Ok(Json::Object(fields));
        }
        loop {
            self.skip_ws();
            if self.peek() != Some('"') {
                return Err(());
            }
            let key = self.parse_string()?;
            self.skip_ws();
            if self.peek() != Some(':') {
                return Err(());
            }
            self.i += 1;
            let value = self.parse_value()?;
            fields.push((key, value));
            self.skip_ws();
            match self.peek() {
                Some(',') => {
                    self.i += 1;
                }
                Some('}') => {
                    self.i += 1;
                    return Ok(Json::Object(fields));
                }
                _ => return Err(()),
            }
        }
    }

    fn parse_array(&mut self) -> Result<Json, ()> {
        if self.peek() != Some('[') {
            return Err(());
        }
        self.i += 1;
        let mut items = Vec::new();
        self.skip_ws();
        if self.peek() == Some(']') {
            self.i += 1;
            return Ok(Json::Array(items));
        }
        loop {
            let value = self.parse_value()?;
            items.push(value);
            self.skip_ws();
            match self.peek() {
                Some(',') => {
                    self.i += 1;
                }
                Some(']') => {
                    self.i += 1;
                    return Ok(Json::Array(items));
                }
                _ => return Err(()),
            }
        }
    }

    fn parse_string(&mut self) -> Result<String, ()> {
        if self.peek() != Some('"') {
            return Err(());
        }
        self.i += 1;
        let mut out = String::new();
        loop {
            let c = self.peek().ok_or(())?;
            self.i += 1;
            match c {
                '"' => return Ok(out),
                '\\' => {
                    let e = self.peek().ok_or(())?;
                    self.i += 1;
                    match e {
                        '"' => out.push('"'),
                        '\\' => out.push('\\'),
                        '/' => out.push('/'),
                        'b' => out.push('\u{0008}'),
                        'f' => out.push('\u{000C}'),
                        'n' => out.push('\n'),
                        'r' => out.push('\r'),
                        't' => out.push('\t'),
                        'u' => {
                            let cu = self.read_hex4()?;
                            if (0xD800..=0xDBFF).contains(&cu) {
                                if self.peek() != Some('\\') {
                                    return Err(());
                                }
                                self.i += 1;
                                if self.peek() != Some('u') {
                                    return Err(());
                                }
                                self.i += 1;
                                let lo = self.read_hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&lo) {
                                    return Err(());
                                }
                                let cp = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                                out.push(char::from_u32(cp).ok_or(())?);
                            } else if (0xDC00..=0xDFFF).contains(&cu) {
                                return Err(());
                            } else {
                                out.push(char::from_u32(cu).ok_or(())?);
                            }
                        }
                        _ => return Err(()),
                    }
                }
                c if (c as u32) < 0x20 => return Err(()),
                c => out.push(c),
            }
        }
    }

    fn read_hex4(&mut self) -> Result<u32, ()> {
        let mut v = 0u32;
        for _ in 0..4 {
            let c = self.peek().ok_or(())?;
            self.i += 1;
            v = (v << 4) | hex_val(c).ok_or(())?;
        }
        Ok(v)
    }

    fn parse_number(&mut self) -> Result<Json, ()> {
        let start = self.i;
        let mut is_integer = true;
        if self.peek() == Some('-') {
            self.i += 1;
        }
        match self.peek() {
            Some('0') => {
                self.i += 1;
            }
            Some('1'..='9') => {
                self.i += 1;
                while matches!(self.peek(), Some('0'..='9')) {
                    self.i += 1;
                }
            }
            _ => return Err(()),
        }
        if self.peek() == Some('.') {
            is_integer = false;
            self.i += 1;
            if !matches!(self.peek(), Some('0'..='9')) {
                return Err(());
            }
            while matches!(self.peek(), Some('0'..='9')) {
                self.i += 1;
            }
        }
        if matches!(self.peek(), Some('e') | Some('E')) {
            is_integer = false;
            self.i += 1;
            if matches!(self.peek(), Some('+') | Some('-')) {
                self.i += 1;
            }
            if !matches!(self.peek(), Some('0'..='9')) {
                return Err(());
            }
            while matches!(self.peek(), Some('0'..='9')) {
                self.i += 1;
            }
        }
        if !is_integer {
            return Ok(Json::Number(None));
        }
        let raw: String = self.chars[start..self.i].iter().collect();
        match raw.parse::<i64>() {
            Ok(v) => Ok(Json::Number(Some(v))),
            Err(_) => Ok(Json::Number(None)),
        }
    }
}

fn json_to_value(j: Json) -> Result<Value, ()> {
    match j {
        Json::Null => Ok(Value::Null),
        Json::Bool(b) => Ok(Value::Bool(b)),
        Json::Number(Some(i)) => Ok(Value::Int(i)),
        Json::Number(None) => Err(()),
        Json::Str(s) => Ok(Value::Str(s)),
        Json::Array(items) => {
            let mut out = Vec::with_capacity(items.len());
            for item in items {
                out.push(json_to_value(item)?);
            }
            Ok(Value::Array(out))
        }
        Json::Object(_) => Err(()),
    }
}

/// Parse the request envelope. Returns (source, vars).
fn parse_request(src: &str) -> Result<(String, HashMap<String, Value>), EError> {
    let mut p = JsonParser::new(src);
    let doc = p
        .parse_document()
        .map_err(|_| invalid_request("request is not valid JSON"))?;
    let fields = match doc {
        Json::Object(fields) => fields,
        _ => return Err(invalid_request("request must be a JSON object")),
    };

    let mut source: Option<String> = None;
    let mut vars: HashMap<String, Value> = HashMap::new();

    for (key, value) in fields {
        match key.as_str() {
            "source" => match value {
                Json::Str(s) => source = Some(s),
                _ => return Err(invalid_request("'source' must be a string")),
            },
            "vars" => match value {
                Json::Object(entries) => {
                    for (name, jv) in entries {
                        let v = json_to_value(jv).map_err(|_| {
                            invalid_request("'vars' values must be null, boolean, integer, string, or array")
                        })?;
                        vars.insert(name, v);
                    }
                }
                _ => return Err(invalid_request("'vars' must be an object")),
            },
            _ => {
                return Err(invalid_request(format!(
                    "unknown request field '{}'",
                    key
                )))
            }
        }
    }

    let source = source.ok_or_else(|| invalid_request("missing required field 'source'"))?;
    Ok((source, vars))
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn run() -> i32 {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.len() != 2 || args[0] != "eval" {
        emit_error(&invalid_request("usage: expr eval <request.json|->"));
        return 2;
    }

    let input = if args[1] == "-" {
        let mut s = String::new();
        match io::stdin().read_to_string(&mut s) {
            Ok(_) => s,
            Err(_) => {
                emit_error(&invalid_request("could not read request from stdin"));
                return 2;
            }
        }
    } else {
        match fs::read_to_string(&args[1]) {
            Ok(s) => s,
            Err(_) => {
                emit_error(&invalid_request("could not read request file"));
                return 2;
            }
        }
    };

    let (source, vars) = match parse_request(&input) {
        Ok(v) => v,
        Err(e) => {
            emit_error(&e);
            return 2;
        }
    };

    let tokens = match tokenize(&source) {
        Ok(t) => t,
        Err(e) => {
            emit_error(&e);
            return 1;
        }
    };
    let mut parser = Parser::new(tokens);
    let ast = match parser.parse() {
        Ok(a) => a,
        Err(e) => {
            emit_error(&e);
            return 1;
        }
    };
    match eval(&ast, &vars) {
        Ok(v) => {
            emit_success(&v);
            0
        }
        Err(e) => {
            emit_error(&e);
            1
        }
    }
}

fn main() {
    // Use a generous stack so deeply nested expressions do not abort the
    // process, and turn unexpected panics into exit code 3.
    let handle = std::thread::Builder::new()
        .stack_size(64 * 1024 * 1024)
        .spawn(run);
    match handle {
        Ok(h) => match h.join() {
            Ok(code) => process::exit(code),
            Err(_) => {
                eprintln!("expr: internal error");
                process::exit(3);
            }
        },
        Err(_) => {
            eprintln!("expr: internal error");
            process::exit(3);
        }
    }
}

//! Just enough JSON: a tree parser and a writer. Numbers keep their text.

use alloc::string::String;
use alloc::vec::Vec;

#[derive(Clone)]
pub enum Value {
    Null,
    Bool(bool),
    Num(String),
    Str(String),
    Arr(Vec<Value>),
    Obj(Vec<(String, Value)>),
}

impl Value {
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Obj(m) => m.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }
    pub fn str(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Some(Value::Str(s)) => Some(s),
            _ => None,
        }
    }
    pub fn int(&self, key: &str) -> Option<i64> {
        match self.get(key) {
            Some(Value::Num(n)) => {
                let t = n.split(['.', 'e', 'E']).next().unwrap_or("");
                t.parse().ok()
            }
            Some(Value::Str(s)) => s.trim().parse().ok(),
            _ => None,
        }
    }
    pub fn boolean(&self, key: &str) -> bool {
        matches!(self.get(key), Some(Value::Bool(true)))
    }
}

pub fn parse(src: &[u8]) -> Option<Value> {
    let mut p = Parser { s: src, i: 0, depth: 0 };
    let v = p.value()?;
    p.ws();
    if p.i == src.len() { Some(v) } else { None }
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
    depth: u32,
}

impl Parser<'_> {
    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }
    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }
    fn eat(&mut self, c: u8) -> bool {
        if self.peek() == Some(c) {
            self.i += 1;
            true
        } else {
            false
        }
    }
    fn lit(&mut self, word: &[u8], v: Value) -> Option<Value> {
        if self.s[self.i..].starts_with(word) {
            self.i += word.len();
            Some(v)
        } else {
            None
        }
    }
    fn value(&mut self) -> Option<Value> {
        self.ws();
        match self.peek()? {
            b'{' => self.object(),
            b'[' => self.array(),
            b'"' => self.string().map(Value::Str),
            b't' => self.lit(b"true", Value::Bool(true)),
            b'f' => self.lit(b"false", Value::Bool(false)),
            b'n' => self.lit(b"null", Value::Null),
            b'-' | b'0'..=b'9' => self.number(),
            _ => None,
        }
    }
    fn nest(&mut self) -> Option<()> {
        self.depth += 1;
        if self.depth > 64 { None } else { Some(()) }
    }
    fn object(&mut self) -> Option<Value> {
        self.nest()?;
        self.i += 1;
        let mut m = Vec::new();
        self.ws();
        if self.eat(b'}') {
            self.depth -= 1;
            return Some(Value::Obj(m));
        }
        loop {
            self.ws();
            if self.peek()? != b'"' {
                return None;
            }
            let k = self.string()?;
            self.ws();
            if !self.eat(b':') {
                return None;
            }
            let v = self.value()?;
            m.push((k, v));
            self.ws();
            if self.eat(b',') {
                continue;
            }
            if self.eat(b'}') {
                self.depth -= 1;
                return Some(Value::Obj(m));
            }
            return None;
        }
    }
    fn array(&mut self) -> Option<Value> {
        self.nest()?;
        self.i += 1;
        let mut a = Vec::new();
        self.ws();
        if self.eat(b']') {
            self.depth -= 1;
            return Some(Value::Arr(a));
        }
        loop {
            a.push(self.value()?);
            self.ws();
            if self.eat(b',') {
                continue;
            }
            if self.eat(b']') {
                self.depth -= 1;
                return Some(Value::Arr(a));
            }
            return None;
        }
    }
    fn number(&mut self) -> Option<Value> {
        let start = self.i;
        while self.i < self.s.len()
            && matches!(self.s[self.i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9')
        {
            self.i += 1;
        }
        Some(Value::Num(String::from_utf8_lossy(&self.s[start..self.i]).into_owned()))
    }
    fn hex4(&mut self) -> Option<u32> {
        let h = self.s.get(self.i..self.i + 4)?;
        self.i += 4;
        let mut v = 0u32;
        for &c in h {
            v = (v << 4) | (c as char).to_digit(16)?;
        }
        Some(v)
    }
    fn string(&mut self) -> Option<String> {
        self.i += 1; // opening quote
        let mut out: Vec<u8> = Vec::new();
        loop {
            let c = self.peek()?;
            self.i += 1;
            match c {
                b'"' => break,
                b'\\' => {
                    let e = self.peek()?;
                    self.i += 1;
                    match e {
                        b'"' => out.push(b'"'),
                        b'\\' => out.push(b'\\'),
                        b'/' => out.push(b'/'),
                        b'b' => out.push(8),
                        b'f' => out.push(12),
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xD800..0xDC00).contains(&cp) {
                                if !(self.eat(b'\\') && self.eat(b'u')) {
                                    return None;
                                }
                                let lo = self.hex4()?;
                                if !(0xDC00..0xE000).contains(&lo) {
                                    return None;
                                }
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            let ch = char::from_u32(cp).unwrap_or('\u{FFFD}');
                            let mut buf = [0u8; 4];
                            out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                        }
                        _ => return None,
                    }
                }
                _ => out.push(c),
            }
        }
        Some(String::from_utf8(out).unwrap_or_else(|e| String::from_utf8_lossy(e.as_bytes()).into_owned()))
    }
}

pub fn write_str(s: &str, out: &mut Vec<u8>) {
    out.push(b'"');
    for b in s.bytes() {
        match b {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            0..=0x1f | 0x7f => {
                out.extend_from_slice(b"\\u00");
                out.push(b"0123456789abcdef"[(b >> 4) as usize]);
                out.push(b"0123456789abcdef"[(b & 15) as usize]);
            }
            _ => out.push(b),
        }
    }
    out.push(b'"');
}

pub fn write(v: &Value, out: &mut Vec<u8>) {
    match v {
        Value::Null => out.extend_from_slice(b"null"),
        Value::Bool(true) => out.extend_from_slice(b"true"),
        Value::Bool(false) => out.extend_from_slice(b"false"),
        Value::Num(n) => out.extend_from_slice(n.as_bytes()),
        Value::Str(s) => write_str(s, out),
        Value::Arr(a) => {
            out.push(b'[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                write(x, out);
            }
            out.push(b']');
        }
        Value::Obj(m) => {
            out.push(b'{');
            for (i, (k, x)) in m.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                write_str(k, out);
                out.push(b':');
                write(x, out);
            }
            out.push(b'}');
        }
    }
}

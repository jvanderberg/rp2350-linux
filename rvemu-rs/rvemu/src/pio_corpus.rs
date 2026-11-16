//! libpio's assembler is checked against pio-parser on one corpus
//! (guest/br-external/package/libpio/test: `make test` compares libpio's
//! output with corpus.expect). This makes corpus.expect again from
//! pio-parser, so the file can't drift from what pio-parser says.

const CORPUS: &str = include_str!("../../../guest/br-external/package/libpio/test/corpus.pio");
const EXPECT: &str = include_str!("../../../guest/br-external/package/libpio/test/corpus.expect");

#[test]
fn corpus_expect_is_pio_parsers_output() {
    let progs = pio_parser::Parser::<32>::parse_file(CORPUS).expect("corpus.pio parses");
    let mut names: Vec<_> = progs.keys().cloned().collect();
    names.sort();
    let mut out = String::new();
    for n in names {
        let p = &progs[&n].program;
        let ss = p.side_set;
        out += &format!(
            "{n} wrap {} {} side {} {} {} :",
            p.wrap.target,
            p.wrap.source,
            ss.bits() as u32 - ss.optional() as u32,
            ss.optional() as u8,
            ss.pindirs() as u8
        );
        for w in &p.code {
            out += &format!(" {w:04x}");
        }
        out += "\n";
    }
    assert_eq!(out, EXPECT, "corpus.expect differs from pio-parser's output");
}

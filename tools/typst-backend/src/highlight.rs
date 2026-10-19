use std::ops::Range;
use std::sync::LazyLock;

use syntect::easy::ScopeRangeIterator;
use syntect::parsing::{ParseState, Scope, ScopeStack};
use typst::text::RAW_SYNTAXES;
use typst_syntax::{LinkedNode, Source, SyntaxKind, Tag, ast};

pub fn highlight_raw(
    source: &Source,
    raw: &LinkedNode<'_>,
    emit: &mut impl FnMut(Range<usize>, &'static str) -> Result<(), String>,
) -> Result<(), String> {
    let Some(typed) = raw.get().cast::<ast::Raw>() else { return Ok(()); };
    let Some(lang) = typed.lang().map(|lang| lang.get().to_lowercase()) else { return Ok(()); };
    if matches!(lang.as_str(), "typ" | "typst" | "typc" | "typm") {
        return highlight_typst(source, raw, &lang, emit);
    }
    let Some(syntax) = RAW_SYNTAXES.find_syntax_by_token(&lang) else { return Ok(()); };
    let mut parser = ParseState::new(syntax);
    let mut stack = ScopeStack::new();
    for child in raw.children().filter(|child| child.kind() == SyntaxKind::Text) {
        let source_range = child.range();
        let line = &source.text()[source_range.clone()];
        let ops = parser.parse_line(line, &RAW_SYNTAXES)
            .map_err(|error| format!("raw syntax highlighting failed: {error}"))?;
        let mut pending: Option<(Range<usize>, RawKind)> = None;
        for (range, op) in ScopeRangeIterator::new(&ops, line) {
            // Empty and end-of-line operations still change the next segment's scope.
            stack.apply(op).map_err(|error| format!("raw syntax highlighting failed: {error}"))?;
            if range.is_empty() { continue; }
            let kind = scope_kind(&stack.scopes);
            if let Some((previous, previous_kind)) = pending.as_mut() {
                if kind == Some(*previous_kind) && previous.end == range.start {
                    previous.end = range.end;
                    continue;
                }
            }
            if let Some((previous, previous_kind)) = pending.take() {
                emit(source_range.start + previous.start..source_range.start + previous.end,
                     raw_kind(previous_kind))?;
            }
            pending = kind.map(|kind| (range, kind));
        }
        if let Some((range, kind)) = pending {
            emit(source_range.start + range.start..source_range.start + range.end, raw_kind(kind))?;
        }
    }
    Ok(())
}

struct SnippetLine {
    snippet_start: usize,
    source: Range<usize>,
}

fn highlight_typst(
    source: &Source,
    raw: &LinkedNode<'_>,
    lang: &str,
    emit: &mut impl FnMut(Range<usize>, &'static str) -> Result<(), String>,
) -> Result<(), String> {
    let mut lines = Vec::new();
    let mut size = 0;
    for child in raw.children().filter(|child| child.kind() == SyntaxKind::Text) {
        let range = child.range();
        if !lines.is_empty() { size += 1; }
        lines.push(SnippetLine { snippet_start: size, source: range.clone() });
        size += range.len();
    }
    if size == 0 { return Ok(()); }
    let mut snippet = String::with_capacity(size);
    for (index, line) in lines.iter().enumerate() {
        if index != 0 { snippet.push('\n'); }
        snippet.push_str(&source.text()[line.source.clone()]);
    }
    let root = match lang {
        "typc" => typst_syntax::parse_code(&snippet),
        "typm" => typst_syntax::parse_math(&snippet),
        _ => typst_syntax::parse(&snippet),
    };
    let mut nodes = vec![LinkedNode::new(&root)];
    while let Some(node) = nodes.pop() {
        if let Some(tag) = typst_syntax::highlight(&node) {
            let kind = typst_raw_kind(&node, tag);
            let range = node.range();
            let first = lines.partition_point(|line| line.snippet_start + line.source.len() <= range.start);
            for line in &lines[first..] {
                if line.snippet_start >= range.end { break; }
                let start = range.start.max(line.snippet_start);
                let end = range.end.min(line.snippet_start + line.source.len());
                if start < end {
                    emit(line.source.start + start - line.snippet_start..
                         line.source.start + end - line.snippet_start, raw_kind(kind))?;
                }
            }
        }
        // Native traversal only: nested examples remain Raw, without recursive extraction.
        nodes.extend(node.children().rev());
    }
    Ok(())
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum RawKind {
    Native(Tag),
    ControlFlow,
    Import,
    DataType,
    BuiltIn,
    Constant,
    DecVal,
    BaseN,
    Float,
    Char,
    VerbatimString,
    SpecialString,
    Documentation,
    Attribute,
    Preprocessor,
}

fn typst_raw_kind(node: &LinkedNode<'_>, tag: Tag) -> RawKind {
    use RawKind::*;
    if tag == Tag::Error { return Native(tag); }
    match node.kind() {
        SyntaxKind::If | SyntaxKind::Else | SyntaxKind::For | SyntaxKind::In |
        SyntaxKind::While | SyntaxKind::Break | SyntaxKind::Continue | SyntaxKind::Return => ControlFlow,
        SyntaxKind::Import | SyntaxKind::Include | SyntaxKind::As => Import,
        SyntaxKind::Bool | SyntaxKind::None | SyntaxKind::Auto => Constant,
        SyntaxKind::Not | SyntaxKind::And | SyntaxKind::Or => Native(Tag::Operator),
        SyntaxKind::Int => {
            let text = node.leaf_text();
            if text.starts_with("0b") || text.starts_with("0o") || text.starts_with("0x") {
                BaseN
            } else {
                DecVal
            }
        }
        SyntaxKind::Float => Float,
        SyntaxKind::Numeric => Native(Tag::Number),
        SyntaxKind::Hash => node.next_sibling().and_then(|next| {
            let expr = next.cast::<ast::Expr>()?;
            if !expr.hash() { return None; }
            let leaf = next.leftmost_leaf()?;
            Some(typst_raw_kind(&leaf, typst_syntax::highlight(&leaf)?))
        }).unwrap_or(Native(tag)),
        _ => Native(tag),
    }
}

#[derive(Clone, Copy)]
enum ScopeRule {
    Kind(RawKind),
    Boundary(&'static [RawKind], RawKind),
    Import,
}

fn scope_kind(scopes: &[Scope]) -> Option<RawKind> {
    use RawKind::*;
    use ScopeRule::{Boundary, Kind};
    struct Prefixes {
        rules: Vec<(Scope, ScopeRule)>,
        preprocessor: Scope,
    }
    static PREFIXES: LazyLock<Prefixes> = LazyLock::new(|| {
        const COMMENTS: &[RawKind] = &[Native(Tag::Comment), Documentation];
        const STRINGS: &[RawKind] = &[Native(Tag::String), Char, VerbatimString, Documentation];
        const NUMBERS: &[RawKind] = &[Native(Tag::Number), DecVal, BaseN, Float];
        let preprocessor = Scope::new("meta.preprocessor").expect("valid static scope prefix");
        let rules = [
            ("invalid", Kind(Native(Tag::Error))),
            ("constant.character.escape", Kind(Native(Tag::Escape))),
            ("comment.line.documentation", Kind(Documentation)),
            ("comment.block.documentation", Kind(Documentation)),
            ("comment.documentation", Kind(Documentation)),
            ("comment", Kind(Native(Tag::Comment))),
            ("string.quoted.single.rust", Kind(Char)),
            ("string.quoted.double.raw", Kind(VerbatimString)),
            ("string.quoted.single.block.python", Kind(VerbatimString)),
            ("string.quoted.double.block.python", Kind(VerbatimString)),
            ("constant.other.placeholder", Kind(SpecialString)),
            ("constant.other.format-spec", Kind(SpecialString)),
            ("string", Kind(Native(Tag::String))),
            ("constant.character", Kind(Char)),
            ("constant.numeric.float", Kind(Float)),
            ("constant.numeric.imaginary", Kind(Float)),
            ("constant.numeric.integer.hexadecimal", Kind(BaseN)),
            ("constant.numeric.integer.binary", Kind(BaseN)),
            ("constant.numeric.integer.octal", Kind(BaseN)),
            ("constant.numeric.integer.decimal", Kind(DecVal)),
            ("constant.numeric.integer", Kind(DecVal)),
            ("constant.numeric", Kind(Native(Tag::Number))),
            ("constant.language", Kind(Constant)),
            ("constant.other", Kind(Constant)),
            ("support.constant", Kind(Constant)),
            ("variable.other.constant", Kind(Constant)),
            ("entity.name.constant", Kind(Constant)),
            ("keyword.control.import", ScopeRule::Import),
            ("keyword.operator", Kind(Native(Tag::Operator))),
            ("keyword.control", Kind(ControlFlow)),
            ("keyword", Kind(Native(Tag::Keyword))),
            ("variable.annotation", Kind(Attribute)),
            ("entity.name.type", Kind(DataType)),
            ("entity.name.class", Kind(DataType)),
            ("entity.name.struct", Kind(DataType)),
            ("entity.name.enum", Kind(DataType)),
            ("entity.name.trait", Kind(DataType)),
            ("support.type", Kind(DataType)),
            ("support.class", Kind(DataType)),
            ("support.function.builtin", Kind(BuiltIn)),
            ("support.function.magic", Kind(BuiltIn)),
            ("entity.name.function", Kind(Native(Tag::Function))),
            ("entity.name.macro", Kind(Native(Tag::Function))),
            ("support.function", Kind(Native(Tag::Function))),
            ("support.macro", Kind(Native(Tag::Function))),
            ("variable.function", Kind(Native(Tag::Function))),
            // Only these literal boundaries inherit a precise enclosing family.
            ("punctuation.definition.comment", Boundary(COMMENTS, Native(Tag::Comment))),
            ("punctuation.definition.string", Boundary(STRINGS, Native(Tag::String))),
            ("storage.type.string", Boundary(STRINGS, Native(Tag::String))),
            ("punctuation.definition.numeric.base", Boundary(NUMBERS, BaseN)),
            ("punctuation.separator.decimal", Boundary(NUMBERS, Float)),
            ("storage.type.numeric", Boundary(NUMBERS, DataType)),
            ("punctuation.definition.placeholder", Kind(SpecialString)),
            ("punctuation.definition.annotation", Kind(Attribute)),
            ("storage", Kind(Native(Tag::Keyword))),
            ("entity.name.namespace", Kind(Native(Tag::Keyword))),
            ("entity.name.module", Kind(Native(Tag::Keyword))),
            ("entity.name.tag", Kind(Native(Tag::Keyword))),
            ("variable", Kind(Native(Tag::Interpolated))),
            ("markup.bold", Kind(Native(Tag::Strong))),
            ("markup.italic", Kind(Native(Tag::Emph))),
            ("markup.heading", Kind(Native(Tag::Heading))),
            ("markup.underline.link", Kind(Native(Tag::Link))),
            ("markup.raw", Kind(Native(Tag::Raw))),
            ("meta.annotation", Kind(Attribute)),
            ("meta.preprocessor", Kind(Preprocessor)),
            ("punctuation", Kind(Native(Tag::Punctuation))),
        ].into_iter().map(|(prefix, rule)| {
            let scope = if prefix == "meta.preprocessor" { preprocessor }
                else { Scope::new(prefix).expect("valid static scope prefix") };
            (scope, rule)
        }).collect();
        Prefixes { rules, preprocessor }
    });
    let prefixes = &*PREFIXES;
    for (index, scope) in scopes.iter().enumerate().rev() {
        for &(prefix, rule) in &prefixes.rules {
            if !prefix.is_prefix_of(*scope) { continue; }
            return Some(match rule {
                Kind(kind) => kind,
                ScopeRule::Import => {
                    if scopes[..index].iter().any(|scope| prefixes.preprocessor.is_prefix_of(*scope)) {
                        Preprocessor
                    } else {
                        Import
                    }
                }
                Boundary(family, fallback) => {
                    // Look only outside this boundary, nearest first; unrelated outer
                    // meta scopes must not replace a recognized inner token.
                    scopes[..index].iter().rev().find_map(|outer| {
                        prefixes.rules.iter().find_map(|&(prefix, rule)| match rule {
                            Kind(kind) if family.contains(&kind) && prefix.is_prefix_of(*outer) => Some(kind),
                            _ => None,
                        })
                    }).unwrap_or(fallback)
                }
            });
        }
    }
    None
}

fn raw_kind(kind: RawKind) -> &'static str {
    match kind {
        RawKind::ControlFlow => "raw.controlflow",
        RawKind::Import => "raw.import",
        RawKind::DataType => "raw.datatype",
        RawKind::BuiltIn => "raw.builtin",
        RawKind::Constant => "raw.constant",
        RawKind::DecVal => "raw.decval",
        RawKind::BaseN => "raw.basen",
        RawKind::Float => "raw.float",
        RawKind::Char => "raw.char",
        RawKind::VerbatimString => "raw.verbatimstring",
        RawKind::SpecialString => "raw.specialstring",
        RawKind::Documentation => "raw.documentation",
        RawKind::Attribute => "raw.attribute",
        RawKind::Preprocessor => "raw.preprocessor",
        RawKind::Native(tag) => match tag {
            Tag::Comment => "raw.comment",
            Tag::Punctuation => "raw.punctuation",
            Tag::Escape => "raw.escape",
            Tag::Strong => "raw.strong",
            Tag::Emph => "raw.emph",
            Tag::Link => "raw.link",
            Tag::Raw => "raw.raw",
            Tag::Label => "raw.label",
            Tag::Ref => "raw.ref",
            Tag::Heading => "raw.heading",
            Tag::ListMarker => "raw.listmarker",
            Tag::ListTerm => "raw.listterm",
            Tag::MathDelimiter => "raw.mathdelimiter",
            Tag::MathOperator => "raw.mathoperator",
            Tag::MathGroupingParens => "raw.mathgroupingparens",
            Tag::Keyword => "raw.keyword",
            Tag::Operator => "raw.operator",
            Tag::Number => "raw.number",
            Tag::String => "raw.string",
            Tag::Function => "raw.function",
            Tag::Interpolated => "raw.interpolated",
            Tag::Error => "raw.error",
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tokens(text: &str) -> Vec<(Range<usize>, &'static str)> {
        let source = Source::detached(text);
        let mut tokens = Vec::new();
        let mut nodes = vec![LinkedNode::new(source.root())];
        while let Some(node) = nodes.pop() {
            if node.kind() == SyntaxKind::Raw {
                let lines: Vec<_> = node.children().filter(|n| n.kind() == SyntaxKind::Text)
                    .map(|n| n.range()).collect();
                highlight_raw(&source, &node, &mut |range, kind| {
                    assert!(range.start < range.end);
                    assert!(text.is_char_boundary(range.start) && text.is_char_boundary(range.end));
                    assert!(lines.iter().any(|line| line.start <= range.start && range.end <= line.end),
                            "token includes a fence, trimmed indentation or line separator: {range:?}");
                    tokens.push((range, kind));
                    Ok(())
                }).unwrap();
            }
            nodes.extend(node.children().rev());
        }
        tokens
    }

    fn assert_token(text: &str, tokens: &[(Range<usize>, &str)], needle: &str, kind: &str) {
        let start = text.find(needle).unwrap();
        assert!(tokens.iter().any(|(range, tag)| *tag == kind && range.start <= start &&
                range.end >= start + needle.len()), "missing {kind} for {needle:?}: {tokens:?}");
    }

    #[test]
    fn rust_maps_authored_bytes_and_retains_multiline_state() {
        let text = "é😀\n  ````RuSt\n  fn main() {\n  \tlet s = \"é😀\";\n    /* first\n\n    second */ let n = 42;\n    // ``` inside a longer fence\n  }\n  ````\n";
        let result = tokens(text);
        for (needle, kind) in [("fn", "raw.keyword"), ("let", "raw.keyword"),
            ("\"é😀\"", "raw.string"), ("42", "raw.decval"),
            ("/* first", "raw.comment"), ("second */", "raw.comment"),
            ("// ``` inside a longer fence", "raw.comment")] {
            assert_token(text, &result, needle, kind);
        }
        // Consecutive string/comment scope fragments are coalesced on each authored line.
        let string = result.iter().find(|(range, kind)| *kind == "raw.string" &&
            text[range.clone()].contains('é')).unwrap();
        assert_eq!(&text[string.0.clone()], "\"é😀\"");
        let inline = "```rust fn main() { let n = 42; }```";
        assert_token(inline, &tokens(inline), "fn", "raw.keyword");
        assert_token(inline, &tokens(inline), "42", "raw.decval");
    }

    #[test]
    fn python_dedent_blank_lines_and_block_isolation() {
        let text = "  ```python\n  def greet():\n      text = \"\"\"first\n\n      é😀 last\"\"\"\n      return 42\n  ```\n\n```python\n\"\"\"unclosed string\n```\n\n```python\ndef fresh():\n    return \"ok\"\n```";
        let result = tokens(text);
        for (needle, kind) in [("def", "raw.keyword"), ("é😀 last", "raw.verbatimstring"),
            ("return", "raw.controlflow"), ("42", "raw.decval"), ("\"ok\"", "raw.string")] {
            assert_token(text, &result, needle, kind);
        }
        let fresh = text.find("def fresh").unwrap();
        assert!(result.iter().any(|(range, kind)| *kind == "raw.keyword" && range.start == fresh &&
            &text[range.clone()] == "def"));
        assert_token(text, &result, "first", "raw.verbatimstring");
    }

    #[test]
    fn line_end_scope_operations_do_not_leak_comments() {
        let text = "```rust\n// previous\n\nlet after = 7;\n```";
        let result = tokens(text);
        assert_token(text, &result, "// previous", "raw.comment");
        assert_token(text, &result, "let", "raw.keyword");
        assert_token(text, &result, "7", "raw.decval");
        let start = text.find("let").unwrap();
        assert!(!result.iter().any(|(range, kind)| *kind == "raw.comment" && range.contains(&start)));
    }

    #[test]
    fn foreign_categories_preserve_scope_distinctions() {
        let text = concat!(
            "```rust\n",
            "/// documentation\n",
            "#[derive(Debug)]\n",
            "struct Widget;\n",
            "fn user() { let ty = String; let n = 42; let hex = 0xff; let f = 1.5; ",
            "let c = 'a'; let escaped = '\\n'; let raw = r\"raw text\"; if true { user(); } }\n",
            "let typed: u32 = 7u32;\n",
            "let formatted = format!(\"{:04}\", 42);\n",
            "```",
        );
        let result = tokens(text);
        for (needle, kind) in [("/// documentation", "raw.documentation"),
            ("derive", "raw.attribute"), ("Widget", "raw.datatype"), ("String", "raw.datatype"),
            ("fn", "raw.keyword"), ("let", "raw.keyword"), ("u32", "raw.keyword"),
            ("if", "raw.controlflow"), ("true", "raw.constant"), ("42", "raw.decval"),
            ("0xff", "raw.basen"), ("1.5", "raw.float"), ("'a'", "raw.char"),
            ("\\n", "raw.escape"), ("r\"raw text\"", "raw.verbatimstring"),
            ("{:04}", "raw.specialstring")] {
            assert_token(text, &result, needle, kind);
        }
        // The grammar distinguishes a literal's type suffix, not `let` from an
        // explicitly written primitive type: both of the latter are storage.type.rust.
        let suffix = text.find("7u32").unwrap() + 1;
        assert!(result.iter().any(|(range, kind)| *kind == "raw.datatype" &&
            range.start == suffix && range.end == suffix + 3));
        let users: Vec<_> = text.match_indices("user").map(|(start, _)| start).collect();
        assert_eq!(users.len(), 2);
        for start in users {
            assert!(result.iter().any(|(range, kind)| *kind == "raw.function" &&
                range.start == start && range.end == start + 4));
            assert!(!result.iter().any(|(range, kind)| *kind == "raw.builtin" && range.contains(&start)));
        }
    }

    #[test]
    fn python_categories_preserve_literal_boundaries() {
        let text = concat!(
            "```python\n",
            "from math import pi\n",
            "class Widget:\n",
            "    \"\"\"documentation\"\"\"\n",
            "    def size(self):\n",
            "        value = 0xff + 1.5 + 42\n",
            "        if True:\n",
            "            return len('é😀')\n",
            "assigned = \"\"\"first line\n\nlast line\"\"\"\n",
            "unfinished = \"\"\"open string\n",
            "```\n\n```python\n",
            "def fresh():\n    return \"ok\"\n",
            "```",
        );
        let result = tokens(text);
        for (needle, kind) in [("from", "raw.import"), ("import", "raw.import"),
            ("Widget", "raw.datatype"), ("\"\"\"documentation\"\"\"", "raw.documentation"),
            ("def", "raw.keyword"), ("size", "raw.function"), ("if", "raw.controlflow"),
            ("return", "raw.controlflow"), ("True", "raw.constant"), ("len", "raw.builtin"),
            ("'é😀'", "raw.string"), ("0xff", "raw.basen"), ("1.5", "raw.float"),
            ("42", "raw.decval"), ("first line", "raw.verbatimstring"),
            ("last line", "raw.verbatimstring"), ("fresh", "raw.function"), ("\"ok\"", "raw.string")] {
            assert_token(text, &result, needle, kind);
        }
        let string = text.find("é😀").unwrap();
        assert!(!result.iter().any(|(range, kind)| *kind == "raw.char" && range.contains(&string)));
        for needle in ["first line", "last line"] {
            let start = text.find(needle).unwrap();
            assert!(!result.iter().any(|(range, kind)| *kind == "raw.documentation" && range.contains(&start)));
        }
        let fresh = text.find("def fresh").unwrap();
        assert!(result.iter().any(|(range, kind)| *kind == "raw.keyword" &&
            range.start == fresh && range.end == fresh + 3));
    }

    #[test]
    fn preprocessor_categories_preserve_inner_tokens() {
        let text = concat!(
            "```cpp\n",
            "#define COUNT 0xff\n",
            "#include <vector>\n",
            "#define MESSAGE \"macro text\" /* macro comment */\n",
            "double value() { if (true) return 1.5; }\n",
            "```",
        );
        let result = tokens(text);
        for (needle, kind) in [("#define", "raw.preprocessor"), ("#include", "raw.preprocessor"),
            ("COUNT", "raw.constant"), ("0xff", "raw.basen"), ("if", "raw.controlflow"),
            ("return", "raw.controlflow"), ("1.5", "raw.float"),
            ("\"macro text\"", "raw.string"), ("/* macro comment */", "raw.comment")] {
            assert_token(text, &result, needle, kind);
        }
        for needle in ["0xff", "macro text", "macro comment"] {
            let start = text.find(needle).unwrap();
            assert!(!result.iter().any(|(range, kind)| *kind == "raw.preprocessor" && range.contains(&start)));
        }
    }

    #[test]
    fn unsupported_empty_and_unfinished_raw_stays_native() {
        for text in ["`fn main()`", "```\nfn main()\n```", "```unknown-lang\nfn main()\n```",
            "```rust\n```", "```rust ```", "```rust\nfn main()",
            "#raw(\"fn main()\", lang: \"rust\")", "#set raw(lang: \"rust\")\n`let n = 42;`"] {
            assert!(tokens(text).is_empty(), "unexpected inner highlighting: {text}");
        }
        let unfinished = Source::detached("```rust\nfn main()");
        assert!(!unfinished.root().errors_and_warnings().0.is_empty());
    }

    #[test]
    fn typst_modes_map_every_overlapped_line_without_recursion() {
        for (lang, body, needle, kind) in [("typ", "#let n = 42", "let", "raw.keyword"),
            ("typst", "#let n = 42", "42", "raw.decval"),
            ("typc", "let n = 42", "let", "raw.keyword"),
            ("typm", "x^2", "^", "raw.mathoperator")] {
            let text = format!("```{lang}\n{body}\n```");
            assert_token(&text, &tokens(&text), needle, kind);
        }
        let text = "  ````typ\n  /* first\n\n    second é😀\n    third */\n  ```rust fn nested() {}```\n  #let broken = (\n  ````";
        let result = tokens(text);
        for line in ["/* first", "second é😀", "third */"] {
            assert_token(text, &result, line, "raw.comment");
        }
        assert_token(text, &result, "```rust fn nested() {}```", "raw.raw");
        let nested = text.find("fn nested").unwrap();
        assert!(!result.iter().any(|(range, kind)| *kind == "raw.keyword" && range.contains(&nested)));
        assert!(result.iter().any(|(_, kind)| *kind == "raw.error"));
    }

    #[test]
    fn typst_categories_refine_native_tags() {
        let markup = concat!(
            "#let n = 42\n",
            "#if true [yes] else [no]\n",
            "#import \"file.typ\": thing as alias\n",
            "#let base = 0xff\n",
            "#let f = 1.5\n",
            "#let size = 12pt\n",
            "#let binary = 0b10\n",
            "#let octal = 0o17\n",
            "#let empty = none\n",
            "#let setting = auto\n",
            "#let logical = not false and true or false\n",
            "#include \"other.typ\"\n",
            "#for item in (1,) { continue }\n",
            "#while false { break }\n",
            "#let answer() = { return 7 }\n",
        );
        for lang in ["typ", "typst", "typc"] {
            let body = if lang == "typc" { markup.replace('#', "") } else { markup.to_owned() };
            let text = format!("```{lang}\n{body}\n```");
            let result = tokens(&text);
            for (needle, kind) in [("let", "raw.keyword"), ("if", "raw.controlflow"),
                ("else", "raw.controlflow"), ("true", "raw.constant"),
                ("import", "raw.import"), ("as", "raw.import"), ("42", "raw.decval"),
                ("0xff", "raw.basen"), ("1.5", "raw.float"), ("12pt", "raw.number"),
                ("0b10", "raw.basen"), ("0o17", "raw.basen"), ("none", "raw.constant"),
                ("auto", "raw.constant"), ("not", "raw.operator"), ("and", "raw.operator"),
                ("or false", "raw.operator"), ("include", "raw.import"), ("for", "raw.controlflow"),
                ("in (", "raw.controlflow"), ("continue", "raw.controlflow"),
                ("while", "raw.controlflow"), ("break", "raw.controlflow"), ("return", "raw.controlflow")] {
                // Context disambiguates `or`/`in` from occurrences inside identifiers.
                let token = needle.split(' ').next().unwrap();
                let start = text.find(needle).unwrap();
                assert!(result.iter().any(|(range, tag)| *tag == kind && range.start <= start &&
                    range.end >= start + token.len()), "missing {kind} for {needle:?} in {lang}: {result:?}");
            }
            if lang != "typc" {
                for (needle, kind) in [("#let", "raw.keyword"), ("#if", "raw.controlflow"),
                    ("#import", "raw.import"), ("#include", "raw.import"),
                    ("#for", "raw.controlflow"), ("#while", "raw.controlflow")] {
                    let hash = text.find(needle).unwrap();
                    assert!(result.iter().any(|(range, tag)| *tag == kind &&
                        range.start == hash && range.end == hash + 1), "missing {kind} Hash in {lang}");
                }
            }
        }
    }

    #[test]
    fn callback_failures_propagate_unchanged() {
        for text in ["```rust let n = 42;```", "```typ #let n = 42```"] {
            let source = Source::detached(text);
            let root = LinkedNode::new(source.root());
            let raw = root.children().find(|node| node.kind() == SyntaxKind::Raw).unwrap();
            assert_eq!(highlight_raw(&source, &raw, &mut |_, _| Err("budget exhausted".into())),
                       Err("budget exhausted".into()));
        }
    }
}

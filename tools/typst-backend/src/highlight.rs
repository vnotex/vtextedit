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
        let mut pending: Option<(Range<usize>, Tag)> = None;
        for (range, op) in ScopeRangeIterator::new(&ops, line) {
            // Empty and end-of-line operations still change the next segment's scope.
            stack.apply(op).map_err(|error| format!("raw syntax highlighting failed: {error}"))?;
            if range.is_empty() { continue; }
            let tag = scope_tag(&stack.scopes);
            if let Some((previous, previous_tag)) = pending.as_mut() {
                if tag == Some(*previous_tag) && previous.end == range.start {
                    previous.end = range.end;
                    continue;
                }
            }
            if let Some((previous, previous_tag)) = pending.take() {
                emit(source_range.start + previous.start..source_range.start + previous.end,
                     raw_kind(previous_tag))?;
            }
            pending = tag.map(|tag| (range, tag));
        }
        if let Some((range, tag)) = pending {
            emit(source_range.start + range.start..source_range.start + range.end, raw_kind(tag))?;
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
            let range = node.range();
            let first = lines.partition_point(|line| line.snippet_start + line.source.len() <= range.start);
            for line in &lines[first..] {
                if line.snippet_start >= range.end { break; }
                let start = range.start.max(line.snippet_start);
                let end = range.end.min(line.snippet_start + line.source.len());
                if start < end {
                    emit(line.source.start + start - line.snippet_start..
                         line.source.start + end - line.snippet_start, raw_kind(tag))?;
                }
            }
        }
        // Native traversal only: nested examples remain Raw, without recursive extraction.
        nodes.extend(node.children().rev());
    }
    Ok(())
}

fn scope_tag(scopes: &[Scope]) -> Option<Tag> {
    static PREFIXES: LazyLock<Vec<(Scope, Tag)>> = LazyLock::new(|| {
        [
            ("invalid", Tag::Error),
            ("constant.character.escape", Tag::Escape),
            ("punctuation.definition.comment", Tag::Comment),
            ("comment", Tag::Comment),
            ("punctuation.definition.string", Tag::String),
            ("string", Tag::String),
            ("constant.character", Tag::String),
            ("constant.numeric", Tag::Number),
            ("constant.other", Tag::Number),
            ("support.constant", Tag::Number),
            ("constant.language", Tag::Keyword),
            ("keyword.operator", Tag::Operator),
            ("keyword", Tag::Keyword),
            ("storage", Tag::Keyword),
            ("entity.name.type", Tag::Keyword),
            ("entity.name.class", Tag::Keyword),
            ("entity.name.struct", Tag::Keyword),
            ("entity.name.enum", Tag::Keyword),
            ("entity.name.namespace", Tag::Keyword),
            ("entity.name.tag", Tag::Keyword),
            ("support.type", Tag::Keyword),
            ("support.class", Tag::Keyword),
            ("entity.name.function", Tag::Function),
            ("support.function", Tag::Function),
            ("variable.function", Tag::Function),
            ("variable", Tag::Interpolated),
            ("markup.bold", Tag::Strong),
            ("markup.italic", Tag::Emph),
            ("markup.heading", Tag::Heading),
            ("markup.underline.link", Tag::Link),
            ("markup.raw", Tag::Raw),
            ("punctuation", Tag::Punctuation),
        ].into_iter().map(|(prefix, tag)|
            (Scope::new(prefix).expect("valid static scope prefix"), tag)).collect()
    });
    for scope in scopes.iter().rev() {
        for &(prefix, tag) in PREFIXES.iter() {
            if prefix.is_prefix_of(*scope) { return Some(tag); }
        }
    }
    None
}

fn raw_kind(tag: Tag) -> &'static str {
    match tag {
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
            ("\"é😀\"", "raw.string"), ("42", "raw.number"),
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
        assert_token(inline, &tokens(inline), "42", "raw.number");
    }

    #[test]
    fn python_dedent_blank_lines_and_block_isolation() {
        let text = "  ```python\n  def greet():\n      text = \"\"\"first\n\n      é😀 last\"\"\"\n      return 42\n  ```\n\n```python\n\"\"\"unclosed string\n```\n\n```python\ndef fresh():\n    return \"ok\"\n```";
        let result = tokens(text);
        for (needle, kind) in [("def", "raw.keyword"), ("é😀 last", "raw.string"),
            ("return", "raw.keyword"), ("42", "raw.number"), ("\"ok\"", "raw.string")] {
            assert_token(text, &result, needle, kind);
        }
        let fresh = text.find("def fresh").unwrap();
        assert!(result.iter().any(|(range, kind)| *kind == "raw.keyword" && range.start == fresh &&
            &text[range.clone()] == "def"));
        assert_token(text, &result, "first", "raw.string");
    }

    #[test]
    fn line_end_scope_operations_do_not_leak_comments() {
        let text = "```rust\n// previous\n\nlet after = 7;\n```";
        let result = tokens(text);
        assert_token(text, &result, "// previous", "raw.comment");
        assert_token(text, &result, "let", "raw.keyword");
        assert_token(text, &result, "7", "raw.number");
        let start = text.find("let").unwrap();
        assert!(!result.iter().any(|(range, kind)| *kind == "raw.comment" && range.contains(&start)));
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
            ("typst", "#let n = 42", "42", "raw.number"),
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

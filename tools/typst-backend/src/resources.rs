//! Snapshot-owned resources, source edits, and parse-only image-reference safety scans.

use std::collections::{BTreeMap, HashMap, HashSet};

use serde_json::{Value, json};
use typst::{World, WorldExt};
use typst::foundations::{Bytes, Content, NativeElement, Smart};
use typst::introspection::Introspector;
use typst::layout::{Abs, Frame, FrameItem, Point, Sides, Size};
use typst::loading::{LoadSource, Loaded};
use typst::model::{Destination, LinkElem};
use typst::syntax::{Span, SyntaxKind, ast::{self, AstNode}};
use typst::utils::Scalar;
use typst::visualize::{ExchangeFormat, Image, ImageElem, ImageFormat, RasterFormat};
use typst_layout::{Page, PagedDocument};
use typst_syntax::{LinkedNode, Source};

use crate::world::{Config, wire_path, wire_range};

const MAX_IMAGE_BYTES: usize = 32 * 1024 * 1024;
const MAX_IMAGE_SIDE: f64 = 8192.0;
const MAX_IMAGE_PIXELS: usize = 16 * 1024 * 1024;
const MAX_REFERENCE_NODES: usize = crate::MAX_HEADER / std::mem::size_of::<LinkedNode<'_>>();

// Count the actual JSON encoding without allocating a second copy of the output.
struct ImageReferenceSize(usize);
impl std::io::Write for ImageReferenceSize {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        if bytes.len() > crate::MAX_HEADER.saturating_sub(self.0) {
            return Err(std::io::Error::other("image references exceed the 16 MiB header limit"));
        }
        self.0 += bytes.len();
        Ok(bytes.len())
    }
    fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
}

fn append_image_reference(values: &mut Vec<Value>, value: &str, size: &mut ImageReferenceSize) -> Result<(), String> {
    if !values.is_empty() {
        std::io::Write::write_all(size, b",").map_err(|error| error.to_string())?;
    }
    serde_json::to_writer(size, value).map_err(|error| error.to_string())?;
    values.push(Value::String(value.into()));
    Ok(())
}

/// Collect positively identified literals without evaluating code or accessing a project.
/// Only a complete final-source scan may be used to establish an image's absence.
pub fn image_references(text: &str) -> Result<Value, String> {
    if text.len() > crate::MAX_HEADER {
        return Err("image-reference source exceeds the 16 MiB header limit".into());
    }
    let source = Source::detached(text);
    let mut complete = !source.root().diagnosis().errors;
    let mut images = Vec::new();
    let mut strings = Vec::new();
    let mut seen_images = HashSet::new();
    let mut seen_strings = HashSet::new();
    let mut image_literals = HashSet::new();
    // `false` is the larger encoding; the two arrays are filled incrementally.
    let mut size = ImageReferenceSize(br#"{"images":[],"strings":[],"complete":false}"#.len());
    let mut nodes = vec![LinkedNode::new(source.root())];
    let mut remaining_nodes = MAX_REFERENCE_NODES - 1;
    while let Some(node) = nodes.pop() {
        // Raw contents are examples, even when their language is Typst.
        if node.kind() == SyntaxKind::Raw { continue; }
        remaining_nodes = remaining_nodes.checked_sub(node.get().children().len())
            .ok_or("image-reference traversal exceeds the protocol resource limit")?;

        if matches!(node.kind(), SyntaxKind::ModuleImport | SyntaxKind::ModuleInclude
            | SyntaxKind::FieldAccess | SyntaxKind::MathFieldAccess)
        {
            complete = false;
        }
        if matches!(node.kind(), SyntaxKind::Ident | SyntaxKind::MathIdent) {
            match node.leaf_text().as_str() {
                "eval" | "std" | "plugin" => complete = false,
                "image" => {
                    let direct_callee = node.parent().and_then(|parent| parent.cast::<ast::FuncCall>())
                        .is_some_and(|call| matches!(call.callee(), ast::Expr::Ident(name)
                            if name.span() == node.span()));
                    if !direct_callee { complete = false; }
                }
                _ => {}
            }
        }
        if let Some(call) = node.cast::<ast::FuncCall>() {
            match call.callee() {
                ast::Expr::Ident(name) if name.as_str() == "image" => {
                    let mut first = None;
                    for arg in call.args().items() {
                        match arg {
                            ast::Arg::Pos(expr) if first.is_none() => first = Some(expr),
                            ast::Arg::Spread(_) => complete = false,
                            _ => {}
                        }
                    }
                    if let Some(ast::Expr::Str(literal)) = first {
                        image_literals.insert(literal.span());
                    } else {
                        complete = false;
                    }
                }
                ast::Expr::Ident(_) => {}
                _ => complete = false,
            }
        }
        if let Some(literal) = node.cast::<ast::Str>() {
            let decoded = literal.get();
            // Emit when visiting the literal, not its call: a named argument may
            // contain an earlier nested image call before the first positional one.
            if image_literals.remove(&node.span()) && seen_images.insert(decoded.clone()) {
                append_image_reference(&mut images, decoded.as_str(), &mut size)?;
            }
            if seen_strings.insert(decoded.clone()) {
                append_image_reference(&mut strings, decoded.as_str(), &mut size)?;
            }
        }
        if node.get().children().len() != 0 {
            nodes.extend(node.children().rev());
        }
    }
    let mut result = json!({"complete":complete});
    result["images"] = Value::Array(images);
    result["strings"] = Value::Array(strings);
    Ok(result)
}

pub struct ImageResource {
    loaded: Loaded,
    image: Option<Image>,
    metadata: Value,
}

#[derive(Default)]
pub struct Resources {
    images: BTreeMap<u64, ImageResource>,
}

impl Resources {
    pub fn insert(&mut self, world: &dyn World, content: &Content, id: usize, image: Option<Image>) {
        let Some(element) = content.to_packed::<ImageElem>() else { return };
        let range = crate::preview::source_range(world, content)
            .unwrap_or_else(|| json!({"file":"", "start":-1, "end":-1}));
        let loaded = element.source.derived.clone();
        let path = match loaded.source.v { LoadSource::Path(file) => wire_path(file), LoadSource::Bytes => String::new() };
        let (width, height) = image.as_ref().and_then(natural_size).unwrap_or((0, 0));
        let resize = if range["start"].as_i64().is_some_and(|start| start >= 0) {
            resize_plan(world, content).unwrap_or(Value::Null)
        } else { Value::Null };
        self.images.insert(id as u64, ImageResource {
            loaded, image,
            metadata: json!({"id":id, "file":range["file"], "start":range["start"], "end":range["end"],
                "path":path, "width":width, "height":height, "resize":resize}),
        });
    }

    // An occurrence remains individually readable on a preview, but a source
    // range must not arbitrarily pick one of several different evaluations.
    pub fn finish(&mut self) {
        let mut first: HashMap<(String, i64, i64), u64> = HashMap::new();
        let mut ambiguous = HashSet::new();
        for (&id, resource) in &self.images {
            let Some(key) = source_key(&resource.metadata) else { continue };
            if let Some(previous) = first.get(&key) {
                let other = &self.images[previous];
                if resource.loaded.source.v != other.loaded.source.v
                    || resource.loaded.data != other.loaded.data || resource.image != other.image
                {
                    ambiguous.insert(key);
                }
            } else { first.insert(key, id); }
        }
        for resource in self.images.values_mut() {
            if source_key(&resource.metadata).is_some_and(|key| ambiguous.contains(&key)) {
                resource.metadata["file"] = json!("");
                resource.metadata["start"] = json!(-1);
                resource.metadata["end"] = json!(-1);
                resource.metadata["resize"] = Value::Null;
            }
        }
    }

    pub fn take_metadata(&mut self) -> Value {
        Value::Array(self.images.values_mut().map(|resource| std::mem::take(&mut resource.metadata)).collect())
    }

    pub fn get(&self, id: u64) -> Result<&ImageResource, String> {
        self.images.get(&id).ok_or_else(|| "unknown image occurrence".into())
    }
}

fn source_key(value: &Value) -> Option<(String, i64, i64)> {
    let file = value["file"].as_str()?;
    let start = value["start"].as_i64()?;
    let end = value["end"].as_i64()?;
    (!file.is_empty() && start >= 0 && end > start).then(|| (file.into(), start, end))
}

fn natural_size(image: &Image) -> Option<(u32, u32)> {
    let (width, height) = (image.width().ceil(), image.height().ceil());
    (width.is_finite() && height.is_finite() && width > 0.0 && height > 0.0
        && width <= i32::MAX as f64 && height <= i32::MAX as f64)
        .then_some((width as u32, height as u32))
}

impl ImageResource {
    pub fn checked_size(&self, config: &Config) -> Result<(u32, u32, usize), String> {
        if self.loaded.data.len() > MAX_IMAGE_BYTES {
            return Err("image exceeds the 32 MiB encoded-byte limit".into());
        }
        let image = self.image.as_ref().ok_or("image has no unambiguous retained decoded representation")?;
        let (width, height) = natural_size(image).ok_or("image has invalid natural dimensions")?;
        let pixels = u64::from(width) * u64::from(height);
        if f64::from(width) > MAX_IMAGE_SIDE || f64::from(height) > MAX_IMAGE_SIDE
            || pixels > MAX_IMAGE_PIXELS.min(config.max_raster_pixels) as u64
            || pixels * 4 > config.max_cache_bytes.min(crate::MAX_BINARY) as u64
        {
            return Err("image exceeds dimension, pixel, or cache resource limits".into());
        }
        // Match typst-render's texture sizing too. Fractional SVG/PDF aspect
        // ratios must not amplify a checked canvas into an unchecked texture.
        let (native_width, native_height) = (image.width() as f32, image.height() as f32);
        let aspect = native_width / native_height;
        let texture_width = native_width.max(aspect * native_height).ceil();
        let texture_height = (texture_width / aspect).ceil();
        let texture_pixels = f64::from(texture_width) * f64::from(texture_height);
        if ![native_width, native_height, aspect, texture_width, texture_height].iter().all(|v| v.is_finite() && *v > 0.0)
            || texture_width > MAX_IMAGE_SIDE as f32 || texture_height > MAX_IMAGE_SIDE as f32
            || texture_pixels > MAX_IMAGE_PIXELS.min(config.max_raster_pixels) as f64
            || texture_pixels * 4.0 > config.max_cache_bytes.min(crate::MAX_BINARY) as f64
        {
            return Err("image texture exceeds renderer dimension, pixel, or cache limits".into());
        }
        Ok((width, height, pixels as usize * 4))
    }

    pub fn mime(&self) -> &'static str {
        match self.image.as_ref().map(Image::format) {
            Some(ImageFormat::Raster(RasterFormat::Exchange(ExchangeFormat::Png))) => "image/png",
            Some(ImageFormat::Raster(RasterFormat::Exchange(ExchangeFormat::Jpg))) => "image/jpeg",
            Some(ImageFormat::Raster(RasterFormat::Exchange(ExchangeFormat::Gif))) => "image/gif",
            Some(ImageFormat::Raster(RasterFormat::Exchange(ExchangeFormat::Webp))) => "image/webp",
            _ => "image/png",
        }
    }

    pub fn data(&self, config: &Config) -> Result<Bytes, String> {
        let (width, height, _) = self.checked_size(config)?;
        let image = self.image.as_ref().unwrap();
        let limit = MAX_IMAGE_BYTES.min(config.max_cache_bytes).min(crate::MAX_BINARY);
        if matches!(image.format(), ImageFormat::Raster(RasterFormat::Exchange(_))) {
            if self.loaded.data.len() > limit { return Err("image exceeds the current cache resource limit".into()); }
            return Ok(self.loaded.data.clone());
        }
        // Render the full original Image, not its clipped/transformed occurrence
        // or a page crop. The image and any SVG/PDF dependencies are already frozen.
        let size = Size::new(Abs::pt(f64::from(width)), Abs::pt(f64::from(height)));
        let mut frame = Frame::soft(size);
        let natural = Size::new(Abs::pt(image.width()), Abs::pt(image.height()));
        frame.push(Point::zero(), FrameItem::Image(image.clone(), natural, Span::detached()));
        let page = Page { frame, bleed: Sides::default(), fill: Smart::Custom(None),
            numbering: None, supplement: Content::empty(), number: 1 };
        let options = typst_render::RenderOptions { pixel_per_pt: Scalar::new(1.0), render_bleed: false };
        let pixels = typst_render::render(&page, &options);
        if pixels.width() != width || pixels.height() != height {
            return Err("image renderer dimensions differed from checked dimensions".into());
        }
        // tiny-skia owns the encoder output; validate before caching/transmitting.
        let encoded = pixels.encode_png().map_err(|error| format!("could not encode image: {error}"))?;
        if encoded.len() > limit { return Err("encoded image exceeds the byte or cache resource limit".into()); }
        Ok(Bytes::new(encoded))
    }
}

fn resize_plan(world: &dyn World, content: &Content) -> Option<Value> {
    let span = content.span();
    let file = span.id()?;
    let source = world.source(file).ok()?;
    let node = source.find(span)?;
    if node.span() != span || node.diagnosis().errors { return None; }
    let call = node.cast::<ast::FuncCall>()?;
    if !matches!(call.callee(), ast::Expr::Ident(name) if name.as_str() == "image") { return None; }
    let args = call.args();
    let element = content.to_packed::<ImageElem>()?;
    let first = args.items().find_map(|arg| match arg { ast::Arg::Pos(expr) => Some(expr), _ => None })?;
    if first.span() != element.source.derived.source.span { return None; }
    let close = args.to_untyped().children().last()?;
    if close.kind() != SyntaxKind::RightParen { return None; }
    let end = world.range(close.span())?.start;
    let mut plan = json!({"argumentsEnd":source.lines().byte_to_utf16(end)?, "trailingComma":args.trailing_comma(),
        "widthStart":-1, "widthEnd":-1, "heightStart":-1, "heightEnd":-1, "widthPoints":0, "heightPoints":0});
    let mut dimensions = HashSet::new();
    for arg in args.items() {
        match arg {
            ast::Arg::Spread(_) => return None,
            ast::Arg::Named(named) => {
                let name = named.name();
                let name = name.as_str();
                if name != "width" && name != "height" { continue; }
                if !dimensions.insert(name) { return None; }
                let expression = named.expr();
                let points = match expression {
                    ast::Expr::Auto(_) => 0.0,
                    ast::Expr::Numeric(number) => {
                        let (value, unit) = number.get();
                        let factor = match unit {
                            ast::Unit::Pt => 1.0, ast::Unit::Mm => 72.0 / 25.4,
                            ast::Unit::Cm => 72.0 / 2.54, ast::Unit::In => 72.0,
                            _ => return None,
                        };
                        let points = value * factor;
                        if !points.is_finite() || points <= 0.0 { return None; }
                        points
                    }
                    _ => return None,
                };
                let range = wire_range(world, file, world.range(expression.span())?)?;
                plan[format!("{name}Start")] = range["start"].clone();
                plan[format!("{name}End")] = range["end"].clone();
                plan[format!("{name}Points")] = json!(points);
            }
            _ => {}
        }
    }
    Some(plan)
}

pub(crate) fn set_destination(document: &PagedDocument, destination: &Destination, value: &mut Value) {
    let target = match destination {
        Destination::Url(url) => { value["url"] = json!(url.to_string()); None }
        Destination::Position(position) => Some(*position),
        Destination::Location(location) => document.introspector().position(*location),
    };
    if let Some(target) = target {
        if target.page.get() <= document.pages().len() {
            let x = target.point.x.to_pt();
            let y = (target.point.y.to_pt() - 10.0).max(0.0);
            if x.is_finite() && y.is_finite() {
                value["targetPage"] = json!(target.page.get() - 1);
                value["targetX"] = json!(x);
                value["targetY"] = json!(y);
            }
        }
    }
}

pub fn source_links(world: &dyn World, document: &PagedDocument) -> Result<Value, String> {
    let mut links = BTreeMap::new();
    let mut ambiguous = HashSet::new();
    for content in document.introspector().query(&LinkElem::ELEM.select()).iter() {
        let Some(mut range) = crate::preview::source_range(world, content) else { continue };
        let Some(key) = source_key(&range) else { continue };
        let link = content.to_packed::<LinkElem>().unwrap();
        let Ok(destination) = link.dest.resolve_late(document.introspector().as_ref()) else { continue };
        range["url"] = json!(""); range["targetPage"] = json!(-1);
        range["targetX"] = json!(0); range["targetY"] = json!(0);
        set_destination(document, &destination, &mut range);
        if range["url"] == "" && range["targetPage"] == -1 { continue; }
        if let Some(previous) = links.get(&key) {
            if previous != &range { ambiguous.insert(key); }
        } else {
            if links.len() >= crate::MAX_HEADER / 128 { return Err("source link metadata exceeds protocol resource limit".into()); }
            links.insert(key, range);
        }
    }
    Ok(Value::Array(links.into_iter().filter_map(|(key, value)| (!ambiguous.contains(&key)).then_some(value)).collect()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use crate::compile::Session;
    use crate::world::{Overlay, ProjectWorld};
    use typst::foundations::Derived;
    use typst::loading::DataSource;
    use typst::visualize::RasterImage;

    #[test]
    fn image_references_decode_literals_and_deduplicate_in_source_order() {
        let source = r##"#image("assets/caf\u{e9}-😀-\"quoted\"-%20.png")
#let path = "other.png"
#image("other.png")
#image("assets/café-😀-\"quoted\"-%20.png")
#link("linked.png")[label]
#image("back\\slash.png")"##;
        assert_eq!(image_references(source).unwrap(), json!({
            "images":["assets/café-😀-\"quoted\"-%20.png", "other.png", "back\\slash.png"],
            "strings":["assets/café-😀-\"quoted\"-%20.png", "other.png", "linked.png", "back\\slash.png"],
            "complete":true,
        }));
        let source = r#"#image(alt: image("earlier.png"), "later.png")"#;
        assert_eq!(image_references(source).unwrap(), json!({
            "images":["earlier.png", "later.png"],
            "strings":["earlier.png", "later.png"], "complete":true,
        }));
        let source = r##"#image("assets/literal%20name.png")
#image("assets/literal\u{25}20name.png")
#image("assets/literal name.png")
#image("assets/../images/hash#query?.png")"##;
        assert_eq!(image_references(source).unwrap(), json!({
            "images":["assets/literal%20name.png", "assets/literal name.png",
                "assets/../images/hash#query?.png"],
            "strings":["assets/literal%20name.png", "assets/literal name.png",
                "assets/../images/hash#query?.png"],
            "complete":true,
        }));
    }

    #[test]
    fn image_references_visit_unevaluated_branches_bodies_and_content() {
        let source = r#"#let unused() = { image("body.png") }
#if false { image("branch.png") }
#text[#image("nested.png")]
#page(fill: red)[#image("page.png")]"#;
        assert_eq!(image_references(source).unwrap(), json!({
            "images":["body.png", "branch.png", "nested.png", "page.png"],
            "strings":["body.png", "branch.png", "nested.png", "page.png"], "complete":true,
        }));
    }

    #[test]
    fn image_references_ignore_raw_comments_and_markup_text() {
        let source = r###"image("text.png")
// #image("comment.png")
/* #import "other.typ": *; #image("block.png") */
`#image("inline.png")`
```typ
#image("example.png")
#let f = image
#include "external.typ"
#image("unfinished.png"
```
#image("actual.png")"###;
        assert_eq!(image_references(source).unwrap(), json!({
            "images":["actual.png"], "strings":["actual.png"], "complete":true,
        }));
        assert_eq!(image_references("").unwrap(), json!({"images":[], "strings":[], "complete":true}));
        assert_eq!(image_references("#text(\"not an image\")").unwrap(),
            json!({"images":[], "strings":["not an image"], "complete":true}));
    }

    #[test]
    fn image_references_disarm_dynamic_indirect_imported_and_malformed_sources() {
        for uncertain in [
            "#image()",
            "#image(path)",
            "#image(\"dir/\" + \"dynamic.png\")",
            "#image((\"parenthesized.png\"))",
            "#image(\"spread.png\", ..options)",
            "#image(..options, \"spread.png\")",
            "#let f = image",
            "#let image(x) = x",
            "#text(image)",
            "#(image)(\"indirect.png\")",
            "#(f)(\"indirect.png\")",
            "#factory()(\"indirect.png\")",
            "#std.image(\"indirect.png\")",
            "#let library = std",
            "#properties.age",
            "$arrow.r$",
            "$image(\"math.png\")$",
            "#let run = eval",
            "#eval(\"code\")",
            "#let load = plugin",
            "#plugin(\"code.wasm\")",
            "#import \"external.typ\": *",
            "#include \"external.typ\"",
            "#image(\"unfinished.png\"",
            "#let invalid = (",
            "```typ\nunterminated raw",
        ] {
            let source = format!("#image(\"known.png\")\n{uncertain}");
            let result = image_references(&source).unwrap();
            assert_eq!(result["complete"], false, "{uncertain}: {result}");
            assert!(result["images"].as_array().unwrap().contains(&json!("known.png")), "{uncertain}: {result}");
            assert!(result["strings"].as_array().unwrap().contains(&json!("known.png")), "{uncertain}: {result}");
        }
        let result = image_references("#image(\"positive.png\", ..options)").unwrap();
        assert_eq!(result, json!({"images":["positive.png"], "strings":["positive.png"], "complete":false}));
        for (uncertain, uncertain_strings) in [
            (r#"#let path = "dynamic.png"; #image(path)"#, vec!["dynamic.png"]),
            (r#"#image("dir/" + "computed.png")"#, vec!["dir/", "computed.png"]),
            (r#"#import "missing.typ": *"#, vec!["missing.typ"]),
            (r#"#include "missing.typst""#, vec!["missing.typst"]),
            ("#let invalid = (", vec![]),
        ] {
            let source = format!("#image(\"known%20.png\")\n{uncertain}");
            let mut strings = vec!["known%20.png"];
            strings.extend(uncertain_strings);
            assert_eq!(image_references(&source).unwrap(), json!({
                "images":["known%20.png"], "strings":strings, "complete":false,
            }), "{uncertain}");
        }
    }

    #[test]
    fn image_references_enforce_source_traversal_and_encoded_output_limits() {
        assert!(image_references(&"x".repeat(crate::MAX_HEADER + 1)).unwrap_err().contains("source"));
        let source = format!("#image(\"{}\")", "x".repeat(crate::MAX_HEADER / 2));
        assert!(image_references(&source).unwrap_err().contains("references exceed"));
        let source = "#none\n".repeat(MAX_REFERENCE_NODES / 2 + 1);
        assert!(image_references(&source).unwrap_err().contains("traversal"));
    }

    const RAW: &str = "#image(bytes((255, 0, 0, 255, 0, 0, 0, 0)), format: (encoding: \"rgba8\", width: 2, height: 1))";
    const SVG: &str = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"4\"><rect x=\"2\" width=\"4\" height=\"4\" fill=\"red\"/></svg>";

    struct Fixture { root: PathBuf }
    impl Fixture {
        fn new() -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let root = std::env::temp_dir().join(format!("vtextedit-resources-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
            std::fs::create_dir(&root).unwrap();
            Self { root }
        }
        fn write(&self, file: &str, bytes: &[u8]) {
            let path = self.root.join(file);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, bytes).unwrap();
        }
        fn config(&self) -> Value { json!({"projectRoot":self.root, "mainFile":"main.typ"}) }
        fn session(&self) -> Session {
            let mut session = Session::default();
            let mut initialize = request("initialize"); initialize["config"] = self.config();
            session.handle(&initialize, &mut |reply, _| {
                assert!(reply["capabilities"].as_array().unwrap().contains(&json!("resources")));
                Ok(())
            }).unwrap();
            session
        }
        fn compile_reply(&self, session: &mut Session, text: &str, generation: u64, retained: Value) -> Value {
            let mut query = request("compile"); query["generation"] = json!(generation);
            query["config"] = self.config(); query["snapshots"] = retained;
            query["activeFile"] = json!("main.typ");
            query["overlays"] = json!([{"file":"main.typ", "text":text}]);
            let mut result = Value::Null;
            session.handle(&query, &mut |reply, _| { if reply["op"] == "compiled" { result = reply; } Ok(()) }).unwrap();
            result
        }
        fn compile(&self, session: &mut Session, text: &str, generation: u64, retained: Value) -> Value {
            let result = self.compile_reply(session, text, generation, retained);
            assert!(result["snapshot"].as_u64().is_some(), "{text}: {result}");
            result
        }
        fn png(&self) -> Bytes {
            let mut session = self.session();
            let compiled = self.compile(&mut session, RAW, 1, json!([]));
            image_data(&mut session, &compiled, 0).1
        }
    }
    impl Drop for Fixture { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.root); } }

    fn request(op: &str) -> Value { json!({"v":1,"op":op,"epoch":1,"generation":1,"request":1}) }
    fn image_data(session: &mut Session, compiled: &Value, index: usize) -> (Value, Bytes) {
        let mut query = request("imageData"); query["snapshot"] = compiled["snapshot"].clone();
        query["image"] = compiled["images"][index]["id"].clone();
        let mut result = (Value::Null, Bytes::new([]));
        session.handle(&query, &mut |reply, bytes| { result = (reply, bytes); Ok(()) }).unwrap();
        result
    }
    fn utf16_slice(text: &str, start: &Value, end: &Value) -> String {
        String::from_utf16(&text.encode_utf16().collect::<Vec<_>>()[start.as_u64().unwrap() as usize..end.as_u64().unwrap() as usize]).unwrap()
    }

    #[test]
    fn raw_pixels_and_svg_return_full_transparent_original_not_preview_crop() {
        let fixture = Fixture::new();
        let mut session = fixture.session();
        let compiled = fixture.compile(&mut session, RAW, 1, json!([]));
        let (reply, data) = image_data(&mut session, &compiled, 0);
        assert_eq!(reply["mime"], "image/png");
        assert_eq!((reply["width"].as_u64(), reply["height"].as_u64()), (Some(2), Some(1)));
        assert_eq!(compiled["images"][0]["path"], "");
        let decoded = RasterImage::plain(data, ExchangeFormat::Png).unwrap();
        let pixels = decoded.dynamic().as_rgba8().unwrap();
        assert_eq!(pixels.get_pixel(0, 0).0, [255, 0, 0, 255]);
        assert_eq!(pixels.get_pixel(1, 0).0[3], 0);

        let text = format!("#box(width: 3pt, height: 3pt, clip: true)[#image(bytes({}), width: 80pt, height: 80pt, fit: \"cover\")]", json!(SVG));
        let compiled = fixture.compile(&mut session, &text, 2, json!([]));
        let (reply, data) = image_data(&mut session, &compiled, 0);
        assert_eq!((reply["width"].as_u64(), reply["height"].as_u64()), (Some(8), Some(4)));
        let decoded = RasterImage::plain(data, ExchangeFormat::Png).unwrap();
        let pixels = decoded.dynamic().as_rgba8().unwrap();
        assert_eq!(pixels.get_pixel(0, 0).0[3], 0);
        assert_eq!(pixels.get_pixel(3, 2).0, [255, 0, 0, 255]);
    }

    #[test]
    fn project_and_imported_paths_are_evaluated_and_snapshot_bytes_survive_live_changes() {
        let fixture = Fixture::new();
        let png = fixture.png();
        fixture.write("图 # 25%.png", &png);
        fixture.write("chapter/picture.png", &png);
        fixture.write("chapter/chapter.typ", b"#image(\"picture.png\", width: 12pt)");
        let mut session = fixture.session();
        let text = "#let file = \"图 # 25%.png\"\n#for n in (1, 2) { image(file) }\n#include \"chapter/chapter.typ\"\n#image(read(file, encoding: none))";
        let compiled = fixture.compile(&mut session, text, 1, json!([]));
        let images = compiled["images"].as_array().unwrap();
        assert_eq!(images.len(), 4);
        assert_eq!(images[0]["path"], "图 # 25%.png");
        assert_eq!(images[1]["path"], images[0]["path"]);
        assert_ne!(images[1]["id"], images[0]["id"]);
        assert_eq!(images[1]["start"], images[0]["start"]);
        assert_eq!(images[2]["path"], "chapter/picture.png");
        assert_eq!(images[2]["file"], "chapter/chapter.typ");
        assert_eq!(images[3]["path"], "");
        for image in images {
            let preview = compiled["previews"].as_array().unwrap().iter().find(|preview| preview["id"] == image["id"]).unwrap();
            assert_eq!(preview["kind"], "image");
        }
        fixture.write("图 # 25%.png", b"changed after compile");
        fixture.write("chapter/picture.png", b"also changed");
        let _new = fixture.compile(&mut session, "New document", 2, json!([compiled["snapshot"]]));
        for i in 0..4 {
            let (reply, bytes) = image_data(&mut session, &compiled, i);
            assert_eq!(reply["mime"], "image/png");
            assert_eq!(bytes, png);
            let (_, cached) = image_data(&mut session, &compiled, i);
            assert_eq!(bytes.as_ptr(), cached.as_ptr(), "cached payload must share storage");
        }
        let mut query = request("imageData"); query["snapshot"] = compiled["snapshot"].clone(); query["image"] = json!(crate::MAX_ID);
        assert!(session.handle(&query, &mut |_, _| panic!("unknown image sent")).unwrap_err().contains("unknown image"));
        for invalid in [json!(0), json!(-1), json!(1.5)] {
            query["image"] = invalid;
            assert!(session.handle(&query, &mut |_, _| panic!("invalid image id sent")).is_err());
        }
        query["image"] = images[0]["id"].clone(); query["snapshot"] = json!(crate::MAX_ID);
        assert!(session.handle(&query, &mut |_, _| panic!("unknown snapshot sent")).unwrap_err().contains("released snapshot"));
        let mut retain = request("retain"); retain["snapshots"] = json!([]);
        session.handle(&retain, &mut |_, _| Ok(())).unwrap();
        query["snapshot"] = compiled["snapshot"].clone();
        assert!(session.handle(&query, &mut |_, _| panic!("released image sent")).unwrap_err().contains("released snapshot"));
    }

    #[test]
    fn resize_plan_uses_exact_utf16_named_values_and_argument_boundary() {
        let fixture = Fixture::new(); fixture.write("picture.png", &fixture.png());
        let mut session = fixture.session();
        let text = "// 😀 é\n#image(\"picture.png\", alt: \"😀\", width: /*keep*/ 1in, height: auto, /*tail*/)";
        let compiled = fixture.compile(&mut session, text, 1, json!([]));
        let image = &compiled["images"][0];
        let plan = &image["resize"];
        assert!(!plan.is_null());
        assert_eq!(utf16_slice(text, &image["start"], &image["end"]), text.split_once('\n').unwrap().1);
        assert_eq!(utf16_slice(text, &plan["widthStart"], &plan["widthEnd"]), "1in");
        assert_eq!(utf16_slice(text, &plan["heightStart"], &plan["heightEnd"]), "auto");
        assert_eq!(plan["widthPoints"], 72.0); assert_eq!(plan["heightPoints"], 0.0);
        assert_eq!(plan["trailingComma"], true);
        assert_eq!(plan["argumentsEnd"], text.encode_utf16().count() - 1);
        let compiled = fixture.compile(&mut session, "#image(\"picture.png\")", 2, json!([]));
        let plan = &compiled["images"][0]["resize"];
        assert_eq!(plan["widthStart"], -1); assert_eq!(plan["heightEnd"], -1);
        assert_eq!(plan["trailingComma"], false);
    }

    #[test]
    fn aliases_wrappers_relative_and_computed_dimensions_are_not_rewrite_targets() {
        let fixture = Fixture::new(); fixture.write("picture.png", &fixture.png());
        let mut session = fixture.session();
        for (index, text) in [
            "#let other = image\n#other(\"picture.png\")",
            "#let image(..args) = std.image(..args)\n#image(\"picture.png\")",
            "#image(\"picture.png\", width: 50%)",
            "#image(\"picture.png\", width: 1em)",
            "#image(\"picture.png\", width: 12pt + 3pt)",
            "#let w = 12pt\n#image(\"picture.png\", width: w)",
            "#let args = (width: 12pt)\n#image(\"picture.png\", ..args)",
        ].iter().enumerate() {
            let compiled = fixture.compile(&mut session, text, index as u64 + 1, json!([]));
            assert!(!compiled["images"].as_array().unwrap().is_empty(), "{text}");
            for image in compiled["images"].as_array().unwrap() { assert!(image["resize"].is_null(), "{text}: {image}"); }
        }
        let compiled = fixture.compile(&mut session, "// #image(\"picture.png\")\n`#image(\"picture.png\")`", 8, json!([]));
        assert_eq!(compiled["images"], json!([]));
    }

    #[test]
    fn differing_repeated_images_and_links_decline_only_ambiguous_source_contexts() {
        let fixture = Fixture::new();
        let mut session = fixture.session();
        let text = "#for n in (0, 255) { image(bytes((n, 100, 0)), format: (encoding: \"rgb8\", width: 1, height: 1)); link(\"https://example.test/\" + str(n))[go] }";
        let compiled = fixture.compile(&mut session, text, 1, json!([]));
        let images = compiled["images"].as_array().unwrap(); assert_eq!(images.len(), 2);
        for image in images { assert_eq!(image["file"], ""); assert_eq!(image["start"], -1); assert!(image["resize"].is_null()); }
        assert_ne!(image_data(&mut session, &compiled, 0).1, image_data(&mut session, &compiled, 1).1);
        assert_eq!(compiled["sourceLinks"], json!([]));
        assert_eq!(compiled["links"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn evaluated_links_include_dynamic_urls_imports_bare_links_and_internal_targets() {
        let fixture = Fixture::new();
        fixture.write("chapter.typ", b"#link(\"https://import.test\")[imported]");
        let mut session = fixture.session();
        let text = "// 😀\n#set heading(numbering: \"1.\")\n#let host = \"https://example.test\"\n#link(host + \"/dynamic\")[label]\nhttps://bare.test\n= Target <target>\n#link(<target>)[inside]\n@target\n#include \"chapter.typ\"";
        let compiled = fixture.compile(&mut session, text, 1, json!([]));
        let links = compiled["sourceLinks"].as_array().unwrap();
        let dynamic = links.iter().find(|link| link["url"] == "https://example.test/dynamic").unwrap();
        assert_eq!(utf16_slice(text, &dynamic["start"], &dynamic["end"]), "#link(host + \"/dynamic\")[label]");
        let bare = links.iter().find(|link| link["url"] == "https://bare.test").unwrap();
        assert_eq!(utf16_slice(text, &bare["start"], &bare["end"]), "https://bare.test");
        assert!(links.iter().any(|link| link["file"] == "chapter.typ" && link["url"] == "https://import.test"));
        let internal = links.iter().find(|link| link["targetPage"] == 0 && utf16_slice(text, &link["start"], &link["end"]).starts_with("#link")).unwrap();
        assert_eq!(internal["url"], "");
        assert!(compiled["links"].as_array().unwrap().iter().any(|link|
            link["targetPage"] == internal["targetPage"] && link["targetX"] == internal["targetX"] && link["targetY"] == internal["targetY"]));
    }

    #[test]
    fn retained_resources_enforce_encoded_dimension_pixel_and_cache_limits() {
        let fixture = Fixture::new();
        let config = Config { project_root: fixture.root.clone(), main_file: "main.typ".into(), ..Default::default() };
        let mut world = ProjectWorld::new(config.clone()).unwrap();
        world.update(config.clone(), vec![Overlay { file: "main.typ".into(), text: RAW.into() }]).unwrap();
        let document = typst::compile::<PagedDocument>(&world).output.unwrap();
        let (_, mut resources) = crate::preview::extract_with_resources(&world, &document).unwrap();
        let resource = resources.images.values_mut().next().unwrap();
        assert!(resource.checked_size(&config).is_ok());
        let small = Config { max_raster_pixels: 1, ..config.clone() };
        assert!(resource.data(&small).unwrap_err().contains("pixel"));
        let small = Config { max_cache_bytes: 7, ..config.clone() };
        assert!(resource.data(&small).unwrap_err().contains("cache"));
        resource.loaded.data = Bytes::new(vec![0; MAX_IMAGE_BYTES + 1]);
        assert!(resource.data(&config).unwrap_err().contains("32 MiB"));

        let mut session = fixture.session();
        let svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8193\" height=\"1\"><rect width=\"8193\" height=\"1\"/></svg>";
        let text = format!("#image(bytes({}), width: 10pt)", json!(svg));
        let compiled = fixture.compile(&mut session, &text, 1, json!([]));
        let mut query = request("imageData"); query["snapshot"] = compiled["snapshot"].clone(); query["image"] = compiled["images"][0]["id"].clone();
        assert!(session.handle(&query, &mut |_, _| panic!("oversized image sent")).unwrap_err().contains("dimension"));
    }

    #[test]
    fn passive_png_jpeg_gif_webp_bytes_are_not_reencoded() {
        let fixture = Fixture::new();
        let decoded = RasterImage::plain(fixture.png(), ExchangeFormat::Png).unwrap();
        let pixels = decoded.dynamic().to_rgb8();
        let mut session = fixture.session();
        for (index, (format, name, mime)) in [
            (ExchangeFormat::Png, "image.png", "image/png"),
            (ExchangeFormat::Jpg, "image.jpg", "image/jpeg"),
            (ExchangeFormat::Gif, "image.gif", "image/gif"),
            (ExchangeFormat::Webp, "image.webp", "image/webp"),
        ].into_iter().enumerate() {
            let mut output = std::io::Cursor::new(Vec::new());
            pixels.write_to(&mut output, format.into()).unwrap();
            let encoded = output.into_inner();
            fixture.write(name, &encoded);
            let text = format!("#image({})", json!(name));
            let compiled = fixture.compile(&mut session, &text, index as u64 + 1, json!([]));
            let (reply, bytes) = image_data(&mut session, &compiled, 0);
            assert_eq!(reply["mime"], mime);
            assert_eq!(bytes.as_slice(), encoded.as_slice());
        }
    }

    #[test]
    fn pdf_image_returns_full_native_image_as_png() {
        let fixture = Fixture::new();
        let mut session = fixture.session();
        let compiled = fixture.compile(&mut session, r##"#set page(width: 8pt, height: 4pt, margin: 0pt, fill: rgb("#ff0000"))"##, 1, json!([]));
        let mut query = request("pdf"); query["snapshot"] = compiled["snapshot"].clone();
        session.handle(&query, &mut |_, bytes| { fixture.write("original.pdf", &bytes); Ok(()) }).unwrap();
        let compiled = fixture.compile(&mut session, "#box(width: 1pt, height: 1pt, clip: true)[#image(\"original.pdf\", width: 80pt)]", 2, json!([]));
        let (reply, bytes) = image_data(&mut session, &compiled, 0);
        assert_eq!(reply["mime"], "image/png");
        assert_eq!(reply["width"], 8); assert_eq!(reply["height"], 4);
        let decoded = RasterImage::plain(bytes, ExchangeFormat::Png).unwrap();
        let pixel = decoded.dynamic().to_rgba8().get_pixel(4, 2).0;
        assert!(pixel[0] > 240 && pixel[1] < 20 && pixel[2] < 20 && pixel[3] == 255);
    }

    #[test]
    fn failed_next_compile_keeps_both_displayed_images_and_never_revives_dropped_newest() {
        let fixture = Fixture::new();
        let a_bytes = fixture.png();
        let decoded = RasterImage::plain(a_bytes.clone(), ExchangeFormat::Png).unwrap();
        let mut green = decoded.dynamic().to_rgba8();
        for pixel in green.pixels_mut() { pixel.0 = [0, 255, 0, 255]; }
        let mut encoded = std::io::Cursor::new(Vec::new());
        green.write_to(&mut encoded, ExchangeFormat::Png.into()).unwrap();
        let b_bytes = Bytes::new(encoded.into_inner());
        fixture.write("a.png", &a_bytes); fixture.write("b.png", &b_bytes);
        let mut session = fixture.session();
        let a = fixture.compile(&mut session, "#image(\"a.png\")", 1, json!([]));
        let b = fixture.compile(&mut session, "#image(\"b.png\")", 2, json!([a["snapshot"]]));
        let c = fixture.compile(&mut session, RAW, 3, json!([a["snapshot"], b["snapshot"]]));
        fixture.write("a.png", b"changed A"); fixture.write("b.png", b"changed B");
        let failed = fixture.compile_reply(&mut session, "#missing_name", 4, json!([a["snapshot"], b["snapshot"]]));
        assert!(failed.get("snapshot").is_none());
        assert!(failed["diagnostics"].as_array().unwrap().iter().any(|d| d["severity"] == "error"));
        assert_eq!(image_data(&mut session, &a, 0).1, a_bytes);
        assert_eq!(image_data(&mut session, &b, 0).1, b_bytes);
        let mut query = request("imageData"); query["snapshot"] = c["snapshot"].clone(); query["image"] = c["images"][0]["id"].clone();
        assert!(session.handle(&query, &mut |_, _| panic!("dropped newest revived")).unwrap_err().contains("released snapshot"));
    }

    #[test]
    fn duplicate_dimensions_never_publish_a_resize_plan() {
        let fixture = Fixture::new(); fixture.write("picture.png", &fixture.png());
        let mut session = fixture.session();
        let reply = fixture.compile_reply(&mut session, "#image(\"picture.png\", width: 10pt, width: 20pt)", 1, json!([]));
        assert!(reply["images"].as_array().unwrap().iter().all(|image| image["resize"].is_null()));
        if reply.get("snapshot").is_none() {
            assert!(reply["diagnostics"].as_array().unwrap().iter().any(|d| d["severity"] == "error"));
        }
    }

    #[test]
    fn fractional_svg_keeps_natural_aspect_inside_bounded_canvas() {
        let fixture = Fixture::new();
        let svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8000\" height=\"0.5\"><rect width=\"8000\" height=\"0.5\" fill=\"red\"/></svg>";
        let text = format!("#image(bytes({}), width: 10pt)", json!(svg));
        let mut session = fixture.session();
        let compiled = fixture.compile(&mut session, &text, 1, json!([]));
        let (reply, bytes) = image_data(&mut session, &compiled, 0);
        assert_eq!(reply["width"], 8000); assert_eq!(reply["height"], 1);
        let decoded = RasterImage::plain(bytes, ExchangeFormat::Png).unwrap();
        assert_eq!(decoded.width(), 8000); assert_eq!(decoded.height(), 1);
    }

    #[test]
    fn unmapped_native_images_keep_bytes_without_inventing_editable_source() {
        let fixture = Fixture::new(); fixture.write("picture.png", &fixture.png());
        let config = Config { project_root: fixture.root.clone(), main_file: "main.typ".into(), ..Default::default() };
        let mut world = ProjectWorld::new(config.clone()).unwrap();
        world.update(config.clone(), vec![Overlay { file: "main.typ".into(), text: "#image(\"picture.png\")".into() }]).unwrap();
        let document = typst::compile::<PagedDocument>(&world).output.unwrap();
        let (_, resources) = crate::preview::extract_with_resources(&world, &document).unwrap();
        let resource = resources.images.values().next().unwrap();
        assert_eq!(resource.data(&config).unwrap().as_ptr(), resource.loaded.data.as_ptr(), "original bytes must be shared, not copied");
        let mut loaded = resource.loaded.clone(); loaded.source.v = LoadSource::Bytes;
        let generated = Content::new(ImageElem::new(Derived::new(DataSource::Bytes(loaded.data.clone()), loaded)));
        let mut unmapped = Resources::default();
        unmapped.insert(&world, &generated, 1, resource.image.clone());
        let nonexpression = generated.spanned(world.source(world.main()).unwrap().root().span());
        unmapped.insert(&world, &nonexpression, 2, resource.image.clone());
        unmapped.finish();
        for image in unmapped.take_metadata().as_array().unwrap() {
            assert_eq!(image["file"], ""); assert_eq!(image["start"], -1); assert_eq!(image["end"], -1);
            assert_eq!(image["path"], ""); assert!(image["resize"].is_null());
        }
        assert_eq!(unmapped.get(1).unwrap().data(&config).unwrap(), resource.loaded.data);
    }
}

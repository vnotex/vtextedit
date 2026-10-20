use std::borrow::Cow;
use std::collections::{BTreeMap, HashSet, VecDeque};
use std::num::NonZeroUsize;

use serde::Deserialize;
use serde_json::{Value, json};
use typst::diag::{Severity, SourceDiagnostic};
use typst::foundations::{Bytes, NativeElement, StyleChain};
use typst::introspection::{Introspector, PagedPosition};
use typst::layout::{Abs, Frame, FrameItem, Point, Sides, Transform};
use typst::model::{Document, HeadingElem};
use typst::syntax::{LinkedNode, Source};
use typst::utils::Scalar;
use typst::{World, WorldExt};
use typst_layout::PagedDocument;

use crate::world::{Config, FrozenWorld, Overlay, ProjectWorld, byte_offset, project_id, wire_range};
use crate::{MAX_BINARY, MAX_HEADER, MAX_ID, id, index, text};

type SendReply<'a> = dyn FnMut(Value, Bytes) -> Result<(), String> + 'a;

struct Snapshot { document: PagedDocument, world: FrozenWorld, resources: crate::resources::Resources }

#[derive(Clone, PartialEq, Eq)]
enum CacheKey { Pdf(u64), Raster(u64, usize, u64), Svg(u64, usize), Image(u64, u64) }
impl CacheKey {
    fn snapshot(&self) -> u64 { match *self { Self::Pdf(id) | Self::Raster(id, ..) | Self::Svg(id, ..) | Self::Image(id, ..) => id } }
}
struct CacheEntry { key: CacheKey, bytes: Bytes }

#[derive(Default)]
pub struct Session {
    epoch: Option<u64>,
    generation: u64,
    world: Option<ProjectWorld>,
    snapshots: BTreeMap<u64, Snapshot>,
    next_snapshot: u64,
    cache: VecDeque<CacheEntry>,
    cache_bytes: usize,
}

impl Session {
    pub fn frame_limit(&self) -> usize {
        self.world.as_ref().map_or(MAX_HEADER, |w| w.config.max_cache_bytes.min(MAX_HEADER))
    }

    pub fn dependencies(&self) -> Value { self.world.as_ref().map_or(json!([]), ProjectWorld::dependencies) }

    pub fn handle(&mut self, request: &Value, send: &mut SendReply<'_>) -> Result<bool, String> {
        let op = text(request, "op")?;
        let epoch = id(request, "epoch")?;
        if op == "initialize" {
            if self.epoch.is_some() { return Err("session is already initialized".into()); }
            let config = Config::deserialize(&request["config"]).map_err(|e| format!("invalid config: {e}"))?;
            let world = ProjectWorld::new(config)?;
            self.epoch = Some(epoch);
            self.world = Some(world);
            send(json!({"op":"ready", "version":1,
                "capabilities":["pdf","raster","svg","navigation","previews","highlights","headings","resources"]}), Bytes::new([]))?;
            return Ok(false);
        }
        if self.epoch != Some(epoch) { return Err("session is not initialized for this epoch".into()); }
        // Advance memoization even after a failed operation or a cached raster
        // request, so historical edits/scales do not accumulate indefinitely.
        struct MemoizationEpoch;
        impl Drop for MemoizationEpoch { fn drop(&mut self) { typst::comemo::evict(10); } }
        let _memoization_epoch = MemoizationEpoch;
        match op {
            "compile" => self.compile(request, send)?,
            "retain" => {
                self.retain(request)?;
                send(json!({"op":"retained"}), Bytes::new([]))?;
            }
            "pdf" => self.pdf(request, send)?,
            "raster" => self.raster(request, send)?,
            "svg" => self.svg(request, send)?,
            "imageData" => self.image_data(request, send)?,
            "sourceToOutput" => self.source_to_output(request, send)?,
            "outputToSource" => self.output_to_source(request, send)?,
            "shutdown" => return Ok(true),
            _ => return Err(format!("unknown operation: {op}")),
        }
        Ok(false)
    }

    fn retain(&mut self, request: &Value) -> Result<(), String> {
        let values = request["snapshots"].as_array().ok_or("snapshots must be a complete retention array")?;
        if values.len() > 3 { return Err("at most three finished snapshots can be retained".into()); }
        let mut retained = HashSet::new();
        for value in values {
            let value = value.as_u64().filter(|n| (1..=MAX_ID).contains(n)).ok_or("invalid retained snapshot ID")?;
            retained.insert(value);
        }
        self.snapshots.retain(|id, _| retained.contains(id));
        self.cache.retain(|entry| retained.contains(&entry.key.snapshot()));
        self.cache_bytes = self.cache.iter().map(|entry| entry.bytes.len()).sum();
        Ok(())
    }

    fn compile(&mut self, request: &Value, send: &mut SendReply<'_>) -> Result<(), String> {
        self.retain(request)?;
        let generation = id(request, "generation")?;
        if generation < self.generation { return Err("compile generation moved backwards".into()); }
        let config = Config::deserialize(&request["config"]).map_err(|e| format!("invalid config: {e}"))?;
        let overlays = Vec::<Overlay>::deserialize(&request["overlays"]).map_err(|e| format!("invalid overlays: {e}"))?;
        let preview_metadata = request["previewMetadata"] != Value::Bool(false);
        let active_file = text(request, "activeFile")?;
        let active = project_id(active_file)?;
        self.generation = generation;
        let world = self.world.as_mut().unwrap();
        if let Err(error) = world.update(config, overlays) {
            send(json!({"op":"parsed", "activeFile":active_file, "highlights":[], "diagnostics":[]}), Bytes::new([]))?;
            return Err(error);
        }
        let mut highlights = Vec::new();
        let mut diagnostics = Vec::new();
        let parsed = (|| {
            if !preview_metadata { return Ok(()); }
            if let Ok(source) = world.source(active) {
                parse_metadata(world, &source, &mut highlights, &mut diagnostics)?;
            }
            if active != world.main() {
                if let Ok(source) = world.source(world.main()) {
                    parse_metadata(world, &source, &mut highlights, &mut diagnostics)?;
                }
            }
            Ok::<_, String>(())
        })();
        if let Err(error) = parsed {
            send(json!({"op":"parsed", "activeFile":active_file, "highlights":[], "diagnostics":[]}), Bytes::new([]))?;
            return Err(error);
        }
        send(json!({"op":"parsed", "activeFile":active_file,
            "highlights":highlights, "diagnostics":diagnostics}), Bytes::new([]))?;

        let result = typst::compile::<PagedDocument>(world);
        for diagnostic in result.warnings.iter() { push_diagnostic(world, diagnostic, &mut diagnostics)?; }
        let document = match result.output {
            Ok(document) => Some(document),
            Err(errors) => {
                for diagnostic in &errors { push_diagnostic(world, diagnostic, &mut diagnostics)?; }
                None
            }
        };
        let mut reply = json!({"op":"compiled", "activeFile":active_file, "highlights":highlights,
            "diagnostics":diagnostics, "dependencies":world.dependencies()});
        reply["images"] = json!([]);
        reply["sourceLinks"] = json!([]);
        if !preview_metadata {
            reply["previews"] = json!([]);
            reply["links"] = json!([]);
            reply["headings"] = json!([]);
        }
        if let Some(error) = world.resource_error() { return Err(error); }
        if let Err(error) = world.check_dependencies() {
            reply["invalidated"] = json!(true);
            reply["diagnostics"].as_array_mut().unwrap().push(unmapped_diagnostic(&error));
            send(reply, Bytes::new([]))?;
            return Ok(());
        }
        if let Some(document) = document {
            if self.snapshots.len() == 3 { return Err("three finished snapshots are retained; release the superseded newest before compiling".into()); }
            let mut pages = Vec::new();
            for page in document.pages() {
                metadata_room(pages.len())?;
                let size = page.frame.size();
                let width = size.x.to_pt();
                let height = size.y.to_pt();
                if !width.is_finite() || !height.is_finite() || width <= 0.0 || height <= 0.0 {
                    return Err("compiled page has invalid dimensions".into());
                }
                pages.push(json!([width, height]));
            }
            let (previews, mut resources, links, headings, source_links) = if preview_metadata {
                let (previews, resources) = crate::preview::extract_with_resources(world, &document)?;
                (previews, resources, extract_links(&document)?, extract_headings(world, &document)?,
                    crate::resources::source_links(world, &document)?)
            } else {
                (json!([]), crate::resources::Resources::default(), json!([]), json!([]), json!([]))
            };
            // Semantic extraction may need additional source files. Validate the
            // complete frozen read set at the actual publication boundary.
            if let Some(error) = world.resource_error() { return Err(error); }
            if let Err(error) = world.check_dependencies() {
                reply["invalidated"] = json!(true);
                reply["dependencies"] = world.dependencies();
                reply["diagnostics"].as_array_mut().unwrap().push(unmapped_diagnostic(&error));
                send(reply, Bytes::new([]))?;
                return Ok(());
            }
            self.next_snapshot = self.next_snapshot.checked_add(1).filter(|id| *id <= MAX_ID).ok_or("snapshot identifier space exhausted")?;
            let snapshot_id = self.next_snapshot;
            reply["snapshot"] = json!(snapshot_id);
            reply["pages"] = Value::Array(pages);
            reply["previews"] = previews;
            reply["links"] = links;
            reply["headings"] = headings;
            reply["dependencies"] = world.dependencies();
            reply["images"] = resources.take_metadata();
            reply["sourceLinks"] = source_links;
            // Serialization's bounded writer checks the full atomic metadata
            // before a finished slot becomes observable to the consumer.
            check_metadata_size(&reply)?;
            self.snapshots.insert(snapshot_id, Snapshot { document, world: world.freeze(), resources });
        }
        let created = reply["snapshot"].as_u64();
        if let Err(error) = send(reply, Bytes::new([])) {
            if let Some(id) = created { self.snapshots.remove(&id); }
            return Err(error);
        }
        self.trim_cache(0);
        Ok(())
    }

    fn cached(&mut self, key: &CacheKey) -> Option<Bytes> {
        let index = self.cache.iter().position(|entry| &entry.key == key)?;
        let entry = self.cache.remove(index)?;
        let bytes = entry.bytes.clone();
        self.cache.push_back(entry);
        Some(bytes)
    }

    fn trim_cache(&mut self, incoming: usize) {
        let limit = self.world.as_ref().map_or(0, |world| world.config.max_cache_bytes);
        while self.cache_bytes > limit.saturating_sub(incoming) {
            let Some(entry) = self.cache.pop_front() else { break; };
            self.cache_bytes -= entry.bytes.len();
        }
    }

    fn cache(&mut self, key: CacheKey, bytes: Bytes) {
        self.trim_cache(bytes.len());
        self.cache_bytes += bytes.len();
        self.cache.push_back(CacheEntry { key, bytes });
    }

    fn pdf(&mut self, request: &Value, send: &mut SendReply<'_>) -> Result<(), String> {
        let snapshot_id = id(request, "snapshot")?;
        let config = &self.world.as_ref().unwrap().config;
        let limit = config.max_pdf_bytes.min(config.max_cache_bytes).min(MAX_BINARY);
        let key = CacheKey::Pdf(snapshot_id);
        let bytes = if let Some(bytes) = self.cached(&key) {
            if bytes.len() > limit { return Err("PDF exceeds the current resource limit".into()); }
            bytes
        } else {
            self.trim_cache(limit);
            let snapshot = self.snapshots.get(&snapshot_id).ok_or("unknown or released snapshot")?;
            let document = pdf_document(&snapshot.document)?;
            let bytes = typst_pdf::pdf(&document, &typst_pdf::PdfOptions::default())
                .map_err(|errors| errors.iter().map(|e| e.message.as_str()).collect::<Vec<_>>().join("\n"))?;
            // typst-pdf owns its output Vec and has no bounded writer API.
            // Never copy, cache, or transmit an oversized export.
            if bytes.len() > limit { return Err("PDF exceeds maxPdfBytes or maxCacheBytes".into()); }
            let bytes = Bytes::new(bytes);
            self.cache(key, bytes.clone());
            bytes
        };
        send(json!({"op":"pdf", "snapshot":snapshot_id}), bytes)
    }

    fn svg(&mut self, request: &Value, send: &mut SendReply<'_>) -> Result<(), String> {
        let snapshot_id = id(request, "snapshot")?;
        let page = index(request, "page")?;
        let snapshot = self.snapshots.get(&snapshot_id).ok_or("unknown or released snapshot")?;
        snapshot.document.pages().get(page).ok_or("page is out of range")?;
        let limit = self.world.as_ref().unwrap().config.max_cache_bytes.min(MAX_BINARY);
        let key = CacheKey::Svg(snapshot_id, page);
        let bytes = if let Some(bytes) = self.cached(&key) {
            if bytes.len() > limit { return Err("SVG exceeds the current resource limit".into()); }
            bytes
        } else {
            self.trim_cache(limit);
            let original = &self.snapshots[&snapshot_id].document.pages()[page];
            let options = typst_svg::SvgOptions { render_bleed: false, pretty: false };
            let svg = typst_svg::svg(original, &options);
            // The native exporter owns its String and has no bounded writer API.
            // Check before transferring its buffer without copying into the cache.
            if svg.len() > limit { return Err("SVG exceeds maxCacheBytes or protocol resource limit".into()); }
            let bytes = Bytes::new(svg.into_bytes());
            self.cache(key, bytes.clone());
            bytes
        };
        send(json!({"op":"svg", "snapshot":snapshot_id, "page":page}), bytes)
    }

    fn raster(&mut self, request: &Value, send: &mut SendReply<'_>) -> Result<(), String> {
        let snapshot_id = id(request, "snapshot")?;
        let page = index(request, "page")?;
        let scale = finite(request, "scale")?;
        let snapshot = self.snapshots.get(&snapshot_id).ok_or("unknown or released snapshot")?;
        let original = snapshot.document.pages().get(page).ok_or("page is out of range")?;
        let config = &self.world.as_ref().unwrap().config;
        let (width, height, bytes_len) = raster_dimensions(original.frame.size().x.to_pt(), original.frame.size().y.to_pt(), scale, config)?;
        let key = CacheKey::Raster(snapshot_id, page, scale.to_bits());
        let bytes = if let Some(bytes) = self.cached(&key) { bytes } else {
            // Remove cached buffers before allocating their replacement.
            self.trim_cache(bytes_len);
            let original = &self.snapshots[&snapshot_id].document.pages()[page];
            let options = typst_render::RenderOptions { pixel_per_pt: Scalar::new(scale), render_bleed: false };
            let pixels = typst_render::render(original, &options);
            if pixels.width() != width || pixels.height() != height { return Err("renderer dimensions differed from checked dimensions".into()); }
            let bytes = Bytes::new(pixels.take());
            self.cache(key, bytes.clone());
            bytes
        };
        send(json!({"op":"raster", "snapshot":snapshot_id, "page":page, "scale":scale,
            "width":width, "height":height, "stride":width * 4}), bytes)
    }

    fn image_data(&mut self, request: &Value, send: &mut SendReply<'_>) -> Result<(), String> {
        let snapshot_id = id(request, "snapshot")?;
        let image_id = id(request, "image")?;
        let snapshot = self.snapshots.get(&snapshot_id).ok_or("unknown or released snapshot")?;
        let resource = snapshot.resources.get(image_id)?;
        let config = &self.world.as_ref().unwrap().config;
        let (width, height, raster_bytes) = resource.checked_size(config)?;
        let mime = resource.mime();
        let key = CacheKey::Image(snapshot_id, image_id);
        let bytes = if let Some(bytes) = self.cached(&key) {
            if bytes.len() > self.world.as_ref().unwrap().config.max_cache_bytes {
                return Err("image exceeds the current cache resource limit".into());
            }
            bytes
        } else {
            self.trim_cache(raster_bytes);
            let resource = self.snapshots[&snapshot_id].resources.get(image_id)?;
            let bytes = resource.data(&self.world.as_ref().unwrap().config)?;
            self.cache(key, bytes.clone());
            bytes
        };
        send(json!({"op":"imageData", "snapshot":snapshot_id, "image":image_id,
            "mime":mime, "width":width, "height":height}), bytes)
    }

    fn source_to_output(&self, request: &Value, send: &mut SendReply<'_>) -> Result<(), String> {
        let snapshot_id = id(request, "snapshot")?;
        let snapshot = self.snapshots.get(&snapshot_id).ok_or("unknown or released snapshot")?;
        let file = text(request, "file")?;
        let source = snapshot.world.source_named(file).ok_or("file is not part of the retained snapshot")?;
        let offset = byte_offset(&source, index(request, "offset")?).ok_or("source offset is out of range or splits a surrogate pair")?;
        let locations: Vec<Value> = source_locations(&snapshot.document, &source, offset).into_iter()
            .filter(|position| position.page.get() <= snapshot.document.pages().len())
            .map(|position| json!({"page":position.page.get() - 1, "x":position.point.x.to_pt(), "y":position.point.y.to_pt()})).collect();
        send(json!({"op":"sourceToOutput", "snapshot":snapshot_id, "locations":locations}), Bytes::new([]))
    }

    fn output_to_source(&self, request: &Value, send: &mut SendReply<'_>) -> Result<(), String> {
        let snapshot_id = id(request, "snapshot")?;
        let snapshot = self.snapshots.get(&snapshot_id).ok_or("unknown or released snapshot")?;
        let page = index(request, "page")?;
        let frame = &snapshot.document.pages().get(page).ok_or("page is out of range")?.frame;
        let x = finite(request, "x")?;
        let y = finite(request, "y")?;
        if x < 0.0 || y < 0.0 || x > frame.size().x.to_pt() || y > frame.size().y.to_pt() {
            return Err("output point is outside the page".into());
        }
        let position = PagedPosition { page: NonZeroUsize::new(page + 1).unwrap(), point: Point::new(Abs::pt(x), Abs::pt(y)) };
        let jump = typst_ide::jump_from_click(&snapshot.world, &snapshot.document, &position);
        let mut reply = json!({"op":"outputToSource", "snapshot":snapshot_id, "location":null});
        match jump {
            Some(typst_ide::Jump::File(file, byte)) => {
                reply["location"] = wire_range(&snapshot.world, file, byte..byte).unwrap_or(Value::Null);
            }
            Some(typst_ide::Jump::Url(url)) => reply["url"] = json!(url.to_string()),
            // The snapshot-owned link sidecar implements internal navigation.
            Some(typst_ide::Jump::Position(_)) | None => {},
        }
        send(reply, Bytes::new([]))
    }
}

fn source_locations(document: &PagedDocument, source: &Source, offset: usize) -> Vec<PagedPosition> {
    // Outline entries reuse the heading body's glyph spans. Resolve the semantic
    // heading instead: glyph lookup only returns the first match on each page.
    let mut positions = Vec::new();
    let mut best_range: Option<std::ops::Range<usize>> = None;
    for heading in document.introspector().query(&HeadingElem::ELEM.select()).iter() {
        let Some(node) = source.find(heading.span()) else { continue };
        let range = node.range();
        // Include a cursor at heading EOL, but prefer the next heading at an abutting start.
        if offset < range.start || offset > range.end { continue; }
        if let Some(best) = &best_range {
            if range.start < best.start || (range.start == best.start && range.end > best.end) {
                continue;
            }
        }
        let Some(position) = heading.location().and_then(|loc| document.introspector().position(loc)) else { continue };
        if best_range.as_ref() != Some(&range) {
            positions.clear();
            best_range = Some(range);
        }
        positions.push(position);
    }
    if positions.is_empty() {
        typst_ide::jump_from_cursor(document, source, offset)
    } else {
        positions
    }
}

fn pdf_document(document: &PagedDocument) -> Result<Cow<'_, PagedDocument>, String> {
    if document.pages().iter().any(|page| page.frame.width().to_pt() < 3.0 || page.frame.height().to_pt() < 3.0) {
        return Err("PDF preview requires page dimensions of at least 3pt; the compiled document and in-place preview remain available".into());
    }
    if document.pages().iter().all(|page| page.bleed.is_zero()) { return Ok(Cow::Borrowed(document)); }
    // The official exporter always adds printer bleed to its MediaBox. The
    // editor uses trim-page coordinates for PDF, raster, links and navigation.
    // Only export page descriptors are copied; finished frames are shared and
    // are never re-laid out or modified in the retained document.
    let pages = document.pages().iter().cloned().map(|mut page| {
        page.bleed = Sides::default();
        page
    }).collect();
    Ok(Cow::Owned(PagedDocument::new(pages, document.info().clone())))
}

fn finite(value: &Value, field: &str) -> Result<f64, String> {
    value[field].as_f64().filter(|n| n.is_finite()).ok_or_else(|| format!("{field} must be a finite number"))
}

fn raster_dimensions(width: f64, height: f64, scale: f64, config: &Config) -> Result<(u32, u32, usize), String> {
    let renderer_scale = scale as f32;
    if !scale.is_finite() || scale <= 0.0 || !renderer_scale.is_finite()
        || renderer_scale == 0.0 || !renderer_scale.recip().is_finite()
    {
        return Err("raster scale must be positive, finite and representable by the renderer".into());
    }
    // Match the pinned renderer's f32 multiplication and rounding exactly.
    let width = ((width as f32) * (scale as f32)).round().max(1.0);
    let height = ((height as f32) * (scale as f32)).round().max(1.0);
    if !width.is_finite() || !height.is_finite() || width > i32::MAX as f32 || height > i32::MAX as f32 {
        return Err("raster dimensions exceed the renderer limits".into());
    }
    let (width, height) = (width as u32, height as u32);
    let pixels = (width as usize).checked_mul(height as usize).ok_or("raster pixel overflow")?;
    let bytes = pixels.checked_mul(4).ok_or("raster byte overflow")?;
    if pixels > config.max_raster_pixels || bytes > config.max_cache_bytes || bytes > MAX_BINARY {
        return Err("raster exceeds maxRasterPixels or maxCacheBytes".into());
    }
    Ok((width, height, bytes))
}

fn metadata_room(length: usize) -> Result<(), String> {
    if length >= MAX_HEADER / 128 { Err("compiled metadata exceeds protocol resource limit".into()) } else { Ok(()) }
}
fn check_metadata_size(value: &Value) -> Result<(), String> {
    struct Count(usize);
    impl std::io::Write for Count {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            // Reserve the envelope added by the transport (IDs and version).
            if bytes.len() > (MAX_HEADER - 256).saturating_sub(self.0) {
                return Err(std::io::Error::other("compiled metadata exceeds protocol resource limit"));
            }
            self.0 += bytes.len();
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
    }
    serde_json::to_writer(Count(0), value).map_err(|e| e.to_string())
}

fn parse_metadata(world: &dyn World, source: &Source, highlights: &mut Vec<Value>, diagnostics: &mut Vec<Value>) -> Result<(), String> {
    let mut nodes = vec![LinkedNode::new(source.root())];
    while let Some(node) = nodes.pop() {
        if let Some(tag) = typst_syntax::highlight(&node) {
            if let Some(mut range) = wire_range(world, source.id(), node.range()) {
                metadata_room(highlights.len())?;
                range["kind"] = json!(format!("{tag:?}"));
                highlights.push(range);
            }
            if tag == typst_syntax::Tag::Raw {
                crate::highlight::highlight_raw(source, &node, &mut |bytes, kind| {
                    if let Some(mut range) = wire_range(world, source.id(), bytes) {
                        metadata_room(highlights.len())?;
                        range["kind"] = json!(kind);
                        highlights.push(range);
                    }
                    Ok(())
                })?;
            }
        }
        nodes.extend(node.children().rev());
    }
    let (errors, warnings) = source.root().errors_and_warnings();
    for diagnostic in errors.into_iter().chain(warnings) {
        push_diagnostic(world, &SourceDiagnostic::from(diagnostic), diagnostics)?;
    }
    Ok(())
}

fn push_diagnostic(world: &dyn World, diagnostic: &SourceDiagnostic, out: &mut Vec<Value>) -> Result<(), String> {
    let mut value = diagnostic.span.id().and_then(|id| {
        world.range(diagnostic.span).and_then(|bytes| wire_range(world, id, bytes))
    }).unwrap_or_else(|| json!({"file":"", "start":-1, "end":-1}));
    value["severity"] = json!(match diagnostic.severity { Severity::Error => "error", Severity::Warning => "warning" });
    let mut message = diagnostic.message.to_string();
    for hint in &diagnostic.hints { message.push_str("\n"); message.push_str(&hint.v); }
    value["message"] = json!(message);
    if !out.contains(&value) { metadata_room(out.len())?; out.push(value); }
    Ok(())
}
fn unmapped_diagnostic(message: &str) -> Value {
    json!({"file":"", "start":-1, "end":-1, "severity":"error", "message":message})
}

fn extract_headings(world: &dyn World, document: &PagedDocument) -> Result<Value, String> {
    let mut headings = Vec::new();
    for elem in document.introspector().query(&HeadingElem::ELEM.select()).iter() {
        let heading = elem.to_packed::<HeadingElem>().unwrap();
        if !heading.outlined.get(StyleChain::default()) { continue; }
        metadata_room(headings.len())?;
        let level = i32::try_from(heading.resolve_level(StyleChain::default()).get())
            .map_err(|_| "compiled heading level exceeds signed 32-bit protocol limit")?;
        let mut value = elem.span().id().and_then(|id| {
            world.range(elem.span()).and_then(|bytes| wire_range(world, id, bytes))
        }).unwrap_or_else(|| json!({"file":"", "start":-1, "end":-1}));
        for field in ["start", "end"] {
            if value[field].as_i64().and_then(|offset| i32::try_from(offset).ok()).is_none() {
                return Err("compiled heading source offset exceeds signed 32-bit protocol limit".into());
            }
        }
        value["title"] = json!(heading.body.plain_text().as_str());
        value["level"] = json!(level);
        headings.push(value);
    }
    Ok(Value::Array(headings))
}

fn extract_links(document: &PagedDocument) -> Result<Value, String> {
    let mut links = Vec::new();
    for (page, original) in document.pages().iter().enumerate() {
        collect_links(document, &original.frame, Transform::identity(), page, &mut links)?;
    }
    Ok(Value::Array(links))
}
fn collect_links(document: &PagedDocument, frame: &Frame, transform: Transform, page: usize, links: &mut Vec<Value>) -> Result<(), String> {
    for (position, item) in frame.items() {
        let transform = transform.pre_concat(Transform::translate(position.x, position.y));
        match item {
            FrameItem::Group(group) => collect_links(document, &group.frame, transform.pre_concat(group.transform), page, links)?,
            FrameItem::Link(destination, size) => {
                metadata_room(links.len())?;
                let points = [Point::zero(), Point::with_x(size.x), size.to_point(), Point::with_y(size.y)];
                let (mut left, mut top, mut right, mut bottom) = (f64::INFINITY, f64::INFINITY, f64::NEG_INFINITY, f64::NEG_INFINITY);
                for point in points {
                    let point = point.transform(transform);
                    // typst-pdf converts transformed points to f32 before its AABB.
                    let x = f64::from(point.x.to_pt() as f32);
                    let y = f64::from(point.y.to_pt() as f32);
                    if !x.is_finite() || !y.is_finite() { return Err("link rectangle has nonfinite coordinates".into()); }
                    left = left.min(x); top = top.min(y); right = right.max(x); bottom = bottom.max(y);
                }
                if right <= left || bottom <= top { continue; }
                let mut link = json!({"page":page,"x":left,"y":top,"width":right-left,"height":bottom-top,
                    "targetPage":-1,"targetX":0,"targetY":0,"url":""});
                crate::resources::set_destination(document, destination, &mut link);
                links.push(link);
            }
            _ => {},
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(op: &str) -> Value {
        json!({"v":1,"op":op,"epoch":1,"generation":1,"request":1})
    }
    fn configured() -> (Session, Value) {
        let mut session = Session::default();
        let config = json!({"projectRoot":std::env::temp_dir(),"mainFile":"main.typ"});
        let mut initialize = request("initialize"); initialize["config"] = config.clone();
        session.handle(&initialize, &mut |_, _| Ok(())).unwrap();
        (session, config)
    }
    fn compile_request(config: &Value, text: &str, retained: Value, generation: u64) -> Value {
        let mut request = request("compile");
        request["config"] = config.clone(); request["snapshots"] = retained;
        request["activeFile"] = json!("main.typ"); request["generation"] = json!(generation);
        request["overlays"] = json!([{"file":"main.typ","text":text}]);
        request
    }

    #[test]
    fn raw_highlights_survive_evaluation_errors() {
        let (mut session, config) = configured();
        let text = "é😀\n```rust\n/// documentation\nfn main() {\n  let s = \"é😀\";\n  // comment\n  if true { let n = 42; let hex = 0xff; let f = 1.5; }\n}\n```\n#missing_name";
        let mut responses = Vec::new();
        session.handle(&compile_request(&config, text, json!([]), 1), &mut |v, _| {
            responses.push(v);
            Ok(())
        }).unwrap();
        assert_eq!(responses.len(), 2);
        assert_eq!(responses[0]["op"], "parsed");
        assert_eq!(responses[0]["diagnostics"], json!([]));
        assert_eq!(responses[1]["op"], "compiled");
        assert!(responses[1].get("snapshot").is_none());
        assert!(responses[1]["diagnostics"].as_array().unwrap().iter().any(|d|
            d["severity"] == "error" && d["message"].as_str().unwrap().contains("missing_name")));
        assert_eq!(responses[0]["highlights"], responses[1]["highlights"]);
        let highlights = responses[0]["highlights"].as_array().unwrap();
        let outer = highlights.iter().position(|h| h["kind"] == "Raw").unwrap();
        for (token, kind) in [("fn", "raw.keyword"), ("let", "raw.keyword"),
            ("\"é😀\"", "raw.string"), ("// comment", "raw.comment"),
            ("/// documentation", "raw.documentation"), ("if", "raw.controlflow"),
            ("true", "raw.constant"), ("42", "raw.decval"), ("0xff", "raw.basen"),
            ("1.5", "raw.float")] {
            let byte = text.find(token).unwrap();
            let start = text[..byte].encode_utf16().count();
            let end = start + token.encode_utf16().count();
            let expected = json!({"file":"main.typ", "start":start, "end":end, "kind":kind});
            let index = highlights.iter().position(|h| h == &expected)
                .unwrap_or_else(|| panic!("missing {expected}: {highlights:?}"));
            assert!(index > outer);
        }
    }

    #[test]
    fn export_skips_preview_metadata_but_preserves_compilation_diagnostics() {
        let (mut session, config) = configured();
        // This heading exceeds preview metadata's level representation, not Typst's.
        let mut compile = compile_request(&config, "#heading(level: 2147483648, numbering: none)[Export]", json!([]), 1);
        compile["previewMetadata"] = json!(false);
        let mut responses = Vec::new();
        session.handle(&compile, &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert!(responses[1]["snapshot"].as_u64().is_some());
        assert_eq!(responses[0]["highlights"], json!([]));
        for key in ["highlights", "previews", "links", "headings", "images", "sourceLinks"] {
            assert_eq!(responses[1][key], json!([]));
        }
        compile = compile_request(&config, "#let x =", json!([]), 2);
        compile["previewMetadata"] = json!(false);
        responses.clear();
        session.handle(&compile, &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert!(responses[1].get("snapshot").is_none());
        assert!(responses[1]["diagnostics"].as_array().unwrap().iter().any(|d| d["severity"] == "error"));
        for key in ["highlights", "previews", "links", "headings", "images", "sourceLinks"] {
            assert_eq!(responses[1][key], json!([]));
        }
    }

    #[test]
    fn svg_rejects_oversized_outputs_and_invalid_pages() {
        let (mut session, config) = configured();
        let mut responses = Vec::new();
        let compile = compile_request(&config, "#set page(width:200pt,height:100pt,margin:0pt,bleed:12pt)\n#rect(width:20pt,height:10pt)", json!([]), 1);
        session.handle(&compile, &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        let snapshot = responses[1]["snapshot"].as_u64().unwrap();
        let mut svg = request("svg"); svg["snapshot"] = json!(snapshot); svg["page"] = json!(0);
        let mut first = Bytes::new([]);
        session.handle(&svg, &mut |v, bytes| {
            assert_eq!(v, json!({"op":"svg", "snapshot":snapshot, "page":0}));
            first = bytes;
            Ok(())
        }).unwrap();
        session.world.as_mut().unwrap().config.max_cache_bytes = first.len() - 1;
        assert!(session.handle(&svg, &mut |_, _| panic!("oversized cached SVG sent")).is_err());
        session.trim_cache(0);
        assert!(session.handle(&svg, &mut |_, _| panic!("oversized SVG sent")).is_err());
        svg["page"] = json!(1);
        assert!(session.handle(&svg, &mut |_, _| panic!("invalid page sent")).is_err());
        svg["page"] = json!(0); svg["snapshot"] = json!(MAX_ID);
        assert!(session.handle(&svg, &mut |_, _| panic!("invalid snapshot sent")).is_err());
        let mut retain = request("retain"); retain["snapshots"] = json!([]);
        session.handle(&retain, &mut |_, _| Ok(())).unwrap();
        svg["snapshot"] = json!(snapshot);
        assert!(session.handle(&svg, &mut |_, _| panic!("released snapshot sent")).is_err());
    }

    #[test]
    fn failed_compile_keeps_displayed_document_and_does_not_consume_slot() {
        let (mut session, config) = configured();
        let mut responses = Vec::new();
        session.handle(&compile_request(&config, "= First\n$ x^2 $", json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert_eq!(responses[0]["op"], "parsed");
        let first = responses[1]["snapshot"].as_u64().unwrap();
        responses.clear();
        session.handle(&compile_request(&config, "#let x =", json!([first]), 2), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert_eq!(responses.len(), 2);
        assert!(responses[1].get("snapshot").is_none());
        assert!(responses[1].get("headings").is_none());
        assert!(responses[1]["diagnostics"].as_array().unwrap().iter().any(|d| d["severity"] == "error"));
        assert_eq!(session.snapshots.len(), 1);
        assert!(session.snapshots.contains_key(&first));
        responses.clear();
        session.handle(&compile_request(&config, "= Recovered", json!([first]), 3), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        let second = responses[1]["snapshot"].as_u64().unwrap();
        assert!(second > first);
        assert_eq!(session.snapshots.len(), 2);
        responses.clear();
        session.handle(&compile_request(&config, "Third", json!([first, second]), 4), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        let third = responses[1]["snapshot"].as_u64().unwrap();
        assert_eq!(session.snapshots.len(), 3);
        assert!(session.handle(&compile_request(&config, "Fourth", json!([first, second, third]), 5), &mut |_, _| Ok(())).is_err());
        assert_eq!(session.snapshots.keys().copied().collect::<Vec<_>>(), vec![first, second, third]);
        let mut retain = request("retain"); retain["snapshots"] = json!([first, MAX_ID]);
        session.handle(&retain, &mut |_, _| Ok(())).unwrap();
        assert_eq!(session.snapshots.keys().copied().collect::<Vec<_>>(), vec![first]);
        let mut pdf = request("pdf"); pdf["snapshot"] = json!(second);
        assert!(session.handle(&pdf, &mut |_, _| Ok(())).is_err());
        responses.clear();
        session.handle(&compile_request(&config, "Fourth", json!([first]), 6), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert!(responses[1]["snapshot"].as_u64().unwrap() > third);
        assert_eq!(session.snapshots.len(), 2);
    }

    #[test]
    fn pixel_checks_match_renderer_and_reject_extremes() {
        let config = Config::default();
        assert_eq!(raster_dimensions(100.5, 40.0, 2.0, &config).unwrap(), (201, 80, 64320));
        assert!(raster_dimensions(1e20, 40.0, 2.0, &config).is_err());
        assert!(raster_dimensions(100.0, 100.0, f64::INFINITY, &config).is_err());
        assert!(raster_dimensions(100.0, 100.0, 0.0, &config).is_err());
        assert!(raster_dimensions(10000.0, 10000.0, 1.0, &config).is_err());
    }

    #[test]
    fn real_navigation_uses_retained_unicode_source() {
        let (mut session, config) = configured();
        let mut responses = Vec::new();
        session.handle(&compile_request(&config, "#set page(width:200pt,height:100pt,margin:10pt)\nA😀 readable text", json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        let snapshot = responses[1]["snapshot"].as_u64().unwrap();
        let source = session.snapshots[&snapshot].world.source_named("main.typ").unwrap();
        let emoji = source.text().find('😀').unwrap();
        let offset = source.lines().byte_to_utf16(emoji).unwrap();
        let mut query = request("sourceToOutput"); query["snapshot"] = json!(snapshot);
        query["file"] = json!("main.typ"); query["offset"] = json!(offset + 1);
        assert!(session.handle(&query, &mut |_, _| Ok(())).is_err());
        query["offset"] = json!(offset + 3);
        let mut locations = Value::Null;
        session.handle(&query, &mut |v, _| { locations = v["locations"].clone(); Ok(()) }).unwrap();
        assert_eq!(locations[0]["page"], 0);
        query["snapshot"] = json!(MAX_ID);
        assert!(session.handle(&query, &mut |_, _| Ok(())).is_err());
    }

    #[test]
    fn headings_use_evaluated_titles_and_outline_visibility() {
        let (mut session, config) = configured();
        let text = r#"#set heading(numbering: "1.")
= *Bold* #("evaluated") _title_
#heading(outlined: false, bookmarked: true)[Hidden]
#heading(outlined: true, bookmarked: false)[Visible]
#[
#set heading(outlined: false, bookmarked: true)
= Hidden by style
]
#set heading(offset: 6)
== Deep
#heading[]"#;
        let mut responses = Vec::new();
        session.handle(&compile_request(&config, text, json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert!(responses[1]["snapshot"].as_u64().is_some(), "{:?}", responses);
        let headings = responses[1]["headings"].as_array().unwrap();
        let titles: Vec<_> = headings.iter().map(|heading| heading["title"].as_str().unwrap()).collect();
        let levels: Vec<_> = headings.iter().map(|heading| heading["level"].as_i64().unwrap()).collect();
        assert_eq!(titles, ["Bold evaluated title", "Visible", "Deep", ""]);
        assert_eq!(levels, [1, 1, 8, 7]);
    }

    #[test]
    fn headings_preserve_generated_import_order_and_utf16_ranges() {
        let (mut session, config) = configured();
        let main = "// 😀\n= Before\n#import \"parts/chapter.typ\": chapter\n#for _ in range(2) { chapter }\n= After";
        let chapter = "// 😀\n#let chapter = [\n== *Imported* 😀\n]";
        let mut compile = compile_request(&config, main, json!([]), 1);
        compile["overlays"].as_array_mut().unwrap().push(json!({"file":"parts/chapter.typ","text":chapter}));
        let mut responses = Vec::new();
        session.handle(&compile, &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert!(responses[1]["snapshot"].as_u64().is_some(), "{:?}", responses);
        let headings = responses[1]["headings"].as_array().unwrap();
        assert_eq!(headings.len(), 4);
        for (heading, (file, source, span, title, level)) in headings.iter().zip([
            ("main.typ", main, "= Before", "Before", 1),
            ("parts/chapter.typ", chapter, "== *Imported* 😀", "Imported 😀", 2),
            ("parts/chapter.typ", chapter, "== *Imported* 😀", "Imported 😀", 2),
            ("main.typ", main, "= After", "After", 1),
        ]) {
            let byte = source.find(span).unwrap();
            let start = source[..byte].encode_utf16().count();
            let end = start + span.encode_utf16().count();
            assert_eq!(*heading, json!({"file":file, "start":start, "end":end, "title":title, "level":level}));
        }
        assert_eq!(headings[1], headings[2], "repeated source spans remain separate occurrences");
    }

    #[test]
    fn successful_compile_without_headings_publishes_empty_array() {
        let (mut session, config) = configured();
        let mut responses = Vec::new();
        session.handle(&compile_request(&config, "Body text.", json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        assert!(responses[1]["snapshot"].as_u64().is_some(), "{:?}", responses);
        assert_eq!(responses[1]["headings"], json!([]));
    }

    #[test]
    fn unrepresentable_heading_level_rejects_snapshot() {
        let (mut session, config) = configured();
        // No numbering or outline: Typst need not allocate a counter at this depth.
        let text = "#heading(level: 2147483648, numbering: none)[Too deep]";
        let mut responses = Vec::new();
        let error = session.handle(&compile_request(&config, text, json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap_err();
        assert!(error.contains("heading level exceeds signed 32-bit"), "{error}");
        assert_eq!(responses.len(), 1);
        assert_eq!(responses[0]["op"], "parsed");
        assert!(session.snapshots.is_empty());
    }

    #[test]
    fn heading_navigation_targets_heading_not_outline() {
        for (heading, marker) in [
            ("= Target", "= Target"),
            ("== *Target*", "=="),
            ("#heading(level: 2)[Target]", "heading"),
        ] {
            let (mut session, config) = configured();
            let text = format!("#set page(width:300pt,height:400pt,margin:20pt)\n#set heading(numbering: \"1.\")\n// 😀\n#outline()\n#pagebreak()\n{heading}\nBody text.");
            let mut responses = Vec::new();
            session.handle(&compile_request(&config, &text, json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
            let snapshot = responses[1]["snapshot"].as_u64().expect("heading compiles");
            for byte in [text.rfind(marker).unwrap(), text.find("Target").unwrap() + 3, text.find("\nBody text.").unwrap()] {
                let mut query = request("sourceToOutput"); query["snapshot"] = json!(snapshot);
                query["file"] = json!("main.typ"); query["offset"] = json!(text[..byte].encode_utf16().count());
                let mut reply = Value::Null;
                session.handle(&query, &mut |v, _| { reply = v; Ok(()) }).unwrap();
                assert_eq!(reply["locations"][0]["page"], 1, "{heading} at byte {byte}: {reply}");
                assert!(reply["locations"][0]["y"].as_f64().unwrap() < 100.0);
            }
        }
    }

    #[test]
    fn heading_navigation_ignores_same_page_outline_copies_in_either_order() {
        for outline_first in [true, false] {
            let (mut session, config) = configured();
            let body = if outline_first {
                "#outline()\n#v(80pt)\n= Target\nBody text."
            } else {
                "= Target\nBody text.\n#v(80pt)\n#outline()"
            };
            let text = format!("#set page(width:300pt,height:500pt,margin:20pt)\n{body}");
            let mut responses = Vec::new();
            session.handle(&compile_request(&config, &text, json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
            let snapshot = responses[1]["snapshot"].as_u64().expect("heading compiles");
            assert_eq!(responses[1]["pages"].as_array().unwrap().len(), 1);
            let mut query = request("sourceToOutput"); query["snapshot"] = json!(snapshot);
            query["file"] = json!("main.typ"); query["offset"] = json!(text.find("Target").unwrap() + 2);
            let mut reply = Value::Null;
            session.handle(&query, &mut |v, _| { reply = v; Ok(()) }).unwrap();
            let location = &reply["locations"][0];
            assert_eq!(location["page"], 0);
            let y = location["y"].as_f64().unwrap();
            let links = responses[1]["links"].as_array().unwrap();
            assert!(!links.is_empty(), "outline must contain heading links");
            for link in links {
                assert_eq!(link["targetPage"], 0);
                let top = link["y"].as_f64().unwrap();
                let bottom = top + link["height"].as_f64().unwrap();
                assert!(if outline_first { y > bottom } else { y < top }, "{body}: {reply}, {link}");
            }
        }
    }

    #[test]
    fn heading_navigation_uses_source_identity_not_title_text() {
        let (mut session, config) = configured();
        let main = "#set page(width:300pt,height:400pt,margin:20pt)\n#outline()\n#pagebreak()\n= Target\n#pagebreak()\n#include \"chapter.typ\"";
        let chapter = "// 😀\n= Target\nChapter body.";
        let mut compile = compile_request(&config, main, json!([]), 1);
        compile["overlays"].as_array_mut().unwrap().push(json!({"file":"chapter.typ","text":chapter}));
        let mut responses = Vec::new();
        session.handle(&compile, &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        let snapshot = responses[1]["snapshot"].as_u64().expect("headings compile");
        for (file, text, page) in [("main.typ", main, 1), ("chapter.typ", chapter, 2)] {
            let offset = text[..text.find("Target").unwrap() + 3].encode_utf16().count();
            let mut query = request("sourceToOutput"); query["snapshot"] = json!(snapshot);
            query["file"] = json!(file); query["offset"] = json!(offset);
            let mut reply = Value::Null;
            session.handle(&query, &mut |v, _| { reply = v; Ok(()) }).unwrap();
            assert_eq!(reply["locations"][0]["page"], page, "{file}: {reply}");
        }
    }

    #[test]
    fn heading_navigation_prefers_inner_and_adjacent_heading_ranges() {
        for body in ["#heading[Outer\n#heading[Inner]]", "#heading[Outer]#heading[Inner]"] {
            let (mut session, config) = configured();
            let text = format!("#set page(width:300pt,height:400pt,margin:20pt)\n{body}");
            let mut responses = Vec::new();
            session.handle(&compile_request(&config, &text, json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
            let snapshot = responses[1]["snapshot"].as_u64().expect("headings compile");
            let mut ys = Vec::new();
            for target in ["Outer", "heading[Inner]"] {
                let mut query = request("sourceToOutput"); query["snapshot"] = json!(snapshot);
                query["file"] = json!("main.typ"); query["offset"] = json!(text.find(target).unwrap());
                let mut reply = Value::Null;
                session.handle(&query, &mut |v, _| { reply = v; Ok(()) }).unwrap();
                assert_eq!(reply["locations"][0]["page"], 0);
                ys.push(reply["locations"][0]["y"].as_f64().unwrap());
            }
            assert!(ys[1] > ys[0], "inner/next heading must not target outer/previous: {body}: {ys:?}");
        }
    }

    #[test]
    fn printer_bleed_does_not_change_preview_navigation_coordinates() {
        let (mut session, config) = configured();
        let text = "#set page(width:200pt,height:100pt,margin:10pt,bleed:12pt)\n= Target <target>\n#link(<target>)[Go]";
        let mut responses = Vec::new();
        session.handle(&compile_request(&config, text, json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        let snapshot_id = responses[1]["snapshot"].as_u64().unwrap();
        let snapshot = &session.snapshots[&snapshot_id];
        let source = snapshot.world.source_named("main.typ").unwrap();
        let before = typst_ide::jump_from_cursor(&snapshot.document, &source, text.find("Target").unwrap());
        let export = pdf_document(&snapshot.document).unwrap();
        assert!(!before.is_empty());
        assert_eq!(before, typst_ide::jump_from_cursor(&*export, &source, text.find("Target").unwrap()));
        assert_eq!(responses[1]["pages"][0], json!([200.0, 100.0]));
        assert_eq!(export.pages()[0].frame.size(), snapshot.document.pages()[0].frame.size());
        assert!(export.pages()[0].bleed.is_zero());
        assert_eq!(snapshot.document.pages()[0].bleed.left.to_pt(), 12.0);
        assert_eq!(extract_links(&export).unwrap(), responses[1]["links"]);
    }

    #[test]
    fn too_small_pdf_page_does_not_destroy_compiled_snapshot() {
        let (mut session, config) = configured();
        let mut responses = Vec::new();
        session.handle(&compile_request(&config, "#set page(width:2pt,height:2pt,margin:0pt)", json!([]), 1), &mut |v, _| { responses.push(v); Ok(()) }).unwrap();
        let snapshot = responses[1]["snapshot"].as_u64().unwrap();
        let mut pdf = request("pdf"); pdf["snapshot"] = json!(snapshot);
        assert!(session.handle(&pdf, &mut |_, _| Ok(())).unwrap_err().contains("at least 3pt"));
        let mut raster = request("raster"); raster["snapshot"] = json!(snapshot);
        raster["page"] = json!(0); raster["scale"] = json!(1.0);
        let mut dimensions = Value::Null;
        session.handle(&raster, &mut |v, pixels| {
            dimensions = json!([v["width"], v["height"]]);
            assert_eq!(pixels.len(), 16);
            Ok(())
        }).unwrap();
        assert_eq!(dimensions, json!([2, 2]));
    }
}

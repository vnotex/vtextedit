//! Semantic ownership and conservative bounds, never a replacement page renderer.

use std::collections::{BTreeMap, HashMap};

use serde_json::{Value, json};
use typst::World;
use typst::foundations::{Content, Smart, StyleChain};
use typst::introspection::{Location, Tag};
use typst::layout::{Frame, FrameItem};
use typst::math::EquationElem;
use typst::syntax::{SyntaxKind, ast};
use typst_layout::PagedDocument;

use crate::world::wire_range;

// This pad is in points, independent of the later raster request's scale.
const ANTIALIAS_PAD: f32 = 0.5;

// Each construction phase gets at most one transport-header budget of metadata
// units. Counting before allocation also bounds intermediate frames that never
// become wire records. These are hard errors, not partial-success truncation.
const METADATA_BYTES: usize = 16 * 1024 * 1024;
const METADATA_ITEMS: usize = METADATA_BYTES / 128;
const MAX_DEPTH: usize = 128;

struct Budget {
    remaining: usize,
}

impl Default for Budget {
    fn default() -> Self {
        Self { remaining: METADATA_ITEMS }
    }
}

impl Budget {
    fn take(&mut self, count: usize, stage: &str) -> Result<(), String> {
        self.remaining = self.remaining.checked_sub(count).ok_or_else(|| resource_error(stage))?;
        Ok(())
    }
}

fn resource_error(stage: &str) -> String {
    format!("Typst preview resource limit exceeded while {stage}; reduce document complexity or split the document")
}

fn check_depth(depth: usize) -> Result<(), String> {
    if depth > MAX_DEPTH {
        Err(resource_error("traversing nested semantic or SVG groups"))
    } else {
        Ok(())
    }
}

type NodeId = usize;

#[derive(Clone, Copy)]
enum Scope {
    Node(NodeId),
    Logical(usize),
}

struct Element<'a> {
    content: &'a Content,
    parent: Option<Scope>,
    resolved_parent: Option<NodeId>,
    kind: Option<&'static str>,
    wire_id: usize,
    invalid: bool,
    // Nearest eligible owner first, including this element when eligible.
    owners: Vec<NodeId>,
}

struct LogicalParent {
    location: Location,
    preferred: Option<NodeId>,
    resolved: Option<NodeId>,
}

// Record ownership while indexing; the second pass does not need to guess which
// of several starts with the same Location a drawable belonged to.
enum Operation {
    Paint { item: usize, scope: Scope, region: usize, order: usize },
    Group { item: usize, plan: FramePlan },
}

type FramePlan = Vec<Operation>;

#[derive(Default)]
struct Index<'a> {
    elements: Vec<Element<'a>>,
    locations: HashMap<Location, Vec<NodeId>>,
    logical: Vec<LogicalParent>,
    // A detached group can itself continue on the next page. Its open tags do
    // not affect the physical page stream, nor disappear at a group boundary.
    detached: HashMap<Location, Vec<Scope>>,
    bad_scopes: Vec<Scope>,
    next_region: usize,
    next_paint: usize,
    next_wire_id: usize,
    visible_images: HashMap<typst::visualize::Image, bool>,
    budget: Budget,
}

// Typst has already decoded these pixels. Inspect alpha without allocating a
// converted bitmap; cache the result for repeated uses of the same image.
fn raster_has_paint(image: &typst::visualize::Image) -> bool {
    let typst::visualize::ImageKind::Raster(raster) = image.kind() else { return true };
    let pixels = raster.dynamic();
    if let Some(pixels) = pixels.as_rgba8() {
        return pixels.pixels().any(|pixel| pixel.0[3] != 0);
    }
    if let Some(pixels) = pixels.as_rgba16() {
        return pixels.pixels().any(|pixel| pixel.0[3] != 0);
    }
    if let Some(pixels) = pixels.as_rgba32f() {
        return pixels.pixels().any(|pixel| pixel.0[3] != 0.0);
    }
    if let Some(pixels) = pixels.as_luma_alpha8() {
        return pixels.pixels().any(|pixel| pixel.0[1] != 0);
    }
    if let Some(pixels) = pixels.as_luma_alpha16() {
        return pixels.pixels().any(|pixel| pixel.0[1] != 0);
    }
    true
}

fn kind(content: &Content) -> Option<&'static str> {
    if content.is::<EquationElem>() {
        return Some("equation");
    }
    match content.elem().name() {
        "image" => Some("image"),
        "table" => Some("table"),
        "figure" => Some("figure"),
        _ => None,
    }
}

impl<'a> Index<'a> {
    fn scan(&mut self, frame: &'a Frame, stack: &mut Vec<Scope>, region: usize) -> Result<FramePlan, String> {
        self.scan_nested(frame, stack, region, 0)
    }

    fn scan_nested(&mut self, frame: &'a Frame, stack: &mut Vec<Scope>, region: usize, depth: usize) -> Result<FramePlan, String> {
        check_depth(depth)?;
        let mut plan = Vec::new();
        for (item_index, (_, item)) in frame.items().enumerate() {
            self.budget.take(1, "indexing semantic tags and page paint")?;
            match item {
                FrameItem::Tag(Tag::Start(content, _)) => {
                    check_depth(stack.len() + 1)?;
                    let node = self.elements.len();
                    let eligible = kind(content);
                    let wire_id = if eligible.is_some() {
                        self.next_wire_id += 1;
                        self.next_wire_id
                    } else {
                        0
                    };
                    self.elements.push(Element {
                        content,
                        parent: stack.last().copied(),
                        resolved_parent: None,
                        kind: eligible,
                        wire_id,
                        invalid: content.location().is_none(),
                        owners: Vec::new(),
                    });
                    if let Some(location) = content.location() {
                        self.locations.entry(location).or_default().push(node);
                    }
                    // Every new start is an occurrence, even if a show rule has
                    // duplicated a Location. An open scope crossing pages is
                    // instead the same occurrence, with no new start required.
                    stack.push(Scope::Node(node));
                }
                FrameItem::Tag(Tag::End(location, _, _)) => {
                    let matching = stack.iter().rposition(|scope| match scope {
                        Scope::Node(node) => self.elements[*node].content.location() == Some(*location),
                        Scope::Logical(_) => false,
                    });
                    if let Some(position) = matching {
                        if position + 1 != stack.len() {
                            self.budget.take(stack.len() - position, "indexing malformed semantic scopes")?;
                            self.bad_scopes.extend_from_slice(&stack[position..]);
                        }
                        stack.truncate(position);
                    } else {
                        self.budget.take(stack.len(), "indexing unmatched semantic closes")?;
                        self.bad_scopes.extend_from_slice(stack);
                        if let Some(nodes) = self.locations.get(location) {
                            self.budget.take(nodes.len(), "indexing ambiguous semantic closes")?;
                            self.bad_scopes.extend(nodes.iter().copied().map(Scope::Node));
                        }
                    }
                }
                FrameItem::Group(group) => {
                    let child_plan = if let Some(parent) = group.parent {
                        let preferred = stack.iter().rev().find_map(|scope| match scope {
                            Scope::Node(node) if self.elements[*node].content.location() == Some(parent.location) => Some(*node),
                            _ => None,
                        });
                        let link = self.logical.len();
                        self.logical.push(LogicalParent {
                            location: parent.location,
                            preferred,
                            resolved: None,
                        });
                        self.next_region += 1;
                        let child_region = self.next_region;
                        let mut detached = self.detached.remove(&parent.location)
                            .unwrap_or_else(|| vec![Scope::Logical(link)]);
                        if detached.len() == 1 && matches!(detached[0], Scope::Logical(_)) {
                            detached[0] = Scope::Logical(link);
                        }
                        let result = self.scan_nested(&group.frame, &mut detached, child_region, depth + 1)?;
                        // Re-entrant logical groups with the same parent cannot
                        // safely share a stream. Mark both streams unresolved.
                        if let Some(other) = self.detached.insert(parent.location, detached) {
                            self.budget.take(other.len() + 1, "indexing re-entrant logical groups")?;
                            self.bad_scopes.extend(other);
                            self.bad_scopes.push(Scope::Logical(link));
                        }
                        result
                    } else {
                        self.scan_nested(&group.frame, stack, region, depth + 1)?
                    };
                    if !child_plan.is_empty() {
                        plan.push(Operation::Group { item: item_index, plan: child_plan });
                    }
                }
                FrameItem::Text(_) | FrameItem::Shape(..) | FrameItem::Image(..) => {
                    if let FrameItem::Image(image, ..) = item {
                        let visible = self.visible_images.entry(image.clone())
                            .or_insert_with(|| raster_has_paint(image));
                        if !*visible {
                            continue;
                        }
                    }
                    if let Some(scope) = stack.last().copied() {
                        plan.push(Operation::Paint {
                            item: item_index,
                            scope,
                            region,
                            order: self.next_paint,
                        });
                    }
                    self.next_paint += 1;
                }
                // Link hit boxes and semantic tags are not paint.
                FrameItem::Link(..) => {}
            }
        }
        Ok(plan)
    }

    fn node(&self, scope: Scope) -> Option<NodeId> {
        match scope {
            Scope::Node(node) => Some(node),
            Scope::Logical(link) => self.logical[link].resolved,
        }
    }

    fn finish(&mut self, stack: &[Scope]) -> Result<(), String> {
        self.budget.take(stack.len(), "resolving open semantic scopes")?;
        self.bad_scopes.extend_from_slice(stack);
        for detached in self.detached.values() {
            self.budget.take(detached.len(), "resolving detached semantic scopes")?;
            self.bad_scopes.extend(detached.iter().copied().filter(|s| matches!(s, Scope::Node(_))));
        }
        for link in &mut self.logical {
            link.resolved = link.preferred.or_else(|| {
                self.locations.get(&link.location)
                    .filter(|nodes| nodes.len() == 1)
                    .map(|nodes| nodes[0])
            });
            // An ambiguous detached edge must not quietly lose the detached
            // paint from any of its possible owners.
            if link.resolved.is_none() {
                if let Some(nodes) = self.locations.get(&link.location) {
                    self.budget.take(nodes.len(), "resolving ambiguous logical parents")?;
                    self.bad_scopes.extend(nodes.iter().copied().map(Scope::Node));
                }
            }
        }
        for node in 0..self.elements.len() {
            if let Some(scope) = self.elements[node].parent {
                self.elements[node].resolved_parent = self.node(scope);
                if self.elements[node].resolved_parent.is_none() {
                    self.elements[node].invalid = true;
                }
            }
        }
        for scope in &self.bad_scopes {
            if let Some(node) = self.node(*scope) {
                self.elements[node].invalid = true;
            }
        }
        // A malformed subtree also makes its outer preview unsafe. This walk
        // terminates at an already marked node, including ancestry cycles.
        let mut marked = vec![false; self.elements.len()];
        for start in 0..self.elements.len() {
            if !self.elements[start].invalid {
                continue;
            }
            let mut at = Some(start);
            while let Some(node) = at {
                if marked[node] {
                    break;
                }
                marked[node] = true;
                self.elements[node].invalid = true;
                at = self.elements[node].resolved_parent;
            }
        }
        // Resolve once per element, rather than walking the same ancestry for
        // every glyph/shape. Grey nodes detect cycles without a panic/limit.
        let mut state = vec![0u8; self.elements.len()];
        let mut depths = vec![0usize; self.elements.len()];
        for start in 0..self.elements.len() {
            if state[start] != 0 {
                continue;
            }
            let mut path: Vec<NodeId> = Vec::new();
            let mut at = Some(start);
            while let Some(node) = at {
                if state[node] != 0 {
                    if state[node] == 1 {
                        for member in &path {
                            self.elements[*member].invalid = true;
                        }
                    }
                    break;
                }
                check_depth(path.len() + 1)?;
                state[node] = 1;
                path.push(node);
                at = self.elements[node].resolved_parent;
            }
            for node in path.into_iter().rev() {
                depths[node] = self.elements[node].resolved_parent.map(|parent| depths[parent] + 1).unwrap_or(1);
                check_depth(depths[node])?;
                if let Some(parent) = self.elements[node].resolved_parent {
                    self.elements[node].invalid |= self.elements[parent].invalid;
                    if !self.elements[node].invalid {
                        self.budget.take(self.elements[parent].owners.len(), "collecting semantic ownership links")?;
                        self.elements[node].owners = self.elements[parent].owners.clone();
                    }
                }
                if !self.elements[node].invalid && self.elements[node].kind.is_some() {
                    self.budget.take(1, "collecting semantic ownership links")?;
                    self.elements[node].owners.insert(0, node);
                }
                state[node] = 2;
            }
        }
        Ok(())
    }

    fn owners(&self, scope: Scope) -> &[NodeId] {
        self.node(scope).map(|node| self.elements[node].owners.as_slice()).unwrap_or(&[])
    }
}

struct Part {
    frame: Frame,
    order: usize,
}

type Parts = BTreeMap<(NodeId, usize), Part>;

fn empty_frame(frame: &Frame) -> Frame {
    // clear() drops the shared item list. Size, baseline and hardness survive.
    let mut result = frame.clone();
    result.clear();
    result
}

fn prune(frame: &Frame, plan: &FramePlan, index: &Index<'_>, budget: &mut Budget) -> Result<Parts, String> {
    let mut parts = Parts::new();
    let items = frame.items().as_slice();
    for operation in plan {
        match operation {
            Operation::Paint { item, scope, region, order } => {
                let (position, paint) = &items[*item];
                for owner in index.owners(*scope) {
                    budget.take(1, "collecting pruned preview paint")?;
                    let part = parts.entry((*owner, *region)).or_insert_with(|| Part {
                        frame: empty_frame(frame),
                        order: *order,
                    });
                    part.frame.push(*position, paint.clone());
                }
            }
            Operation::Group { item, plan } => {
                let (position, FrameItem::Group(group)) = &items[*item] else { continue };
                for (key, child) in prune(&group.frame, plan, index, budget)? {
                    budget.take(1, "retaining pruned preview ancestry")?;
                    let part = parts.entry(key).or_insert_with(|| Part {
                        frame: empty_frame(frame),
                        order: child.order,
                    });
                    part.order = part.order.min(child.order);
                    let mut retained = group.clone();
                    retained.frame = child.frame;
                    // Keep transform, clip, size, hardness, parent and physical
                    // ancestry intact; never flatten/reflow selected paint.
                    part.frame.push(*position, FrameItem::Group(retained));
                }
            }
        }
    }
    Ok(parts)
}

fn source_range(world: &dyn World, content: &Content) -> Option<Value> {
    let span = content.span();
    let file = span.id()?;
    let source = world.source(file).ok()?;
    if source.id() != file {
        return None;
    }
    let node = source.find(span)?;
    if node.span() != span || node.diagnosis().errors {
        return None;
    }
    let expression = node.cast::<ast::Expr>()?;
    // A semantic source span must name an actual syntax expression. Never
    // search for its text elsewhere, or grow an arbitrary identifier into a
    // neighboring call. Generated/non-expression spans remain unmapped.
    match expression {
        ast::Expr::Equation(_) if content.is::<EquationElem>() => {}
        ast::Expr::FuncCall(_) | ast::Expr::MathCall(_) => {}
        _ => return None,
    }
    let mut bytes = node.range();
    if bytes.is_empty() || source.text().get(bytes.clone()).is_none() {
        return None;
    }
    if let Some(previous) = node.prev_sibling_with_trivia() {
        if previous.kind() == SyntaxKind::Hash {
            if !expression.hash() || previous.range().end != bytes.start
                || previous.span().id() != Some(file)
            {
                return None;
            }
            bytes.start = previous.range().start;
        } else if node.parent_kind() == Some(SyntaxKind::Markup)
            && !matches!(expression, ast::Expr::Equation(_))
        {
            return None;
        }
    } else if node.parent_kind() == Some(SyntaxKind::Markup)
        && !matches!(expression, ast::Expr::Equation(_))
    {
        return None;
    }
    wire_range(world, file, bytes)
}

// usvg's root layer box is NOT clipped, and is a fabricated 1x1 rectangle for
// an empty group. Walk painted children and intersect each group's clip bounds
// instead. Bounding-box intersections are conservative for curved clips.
#[derive(Debug)]
enum BoundsError {
    Geometry,
    Resource(String),
}

type Bounds = Result<Option<usvg::Rect>, BoundsError>;

fn union(left: Option<usvg::Rect>, right: Option<usvg::Rect>) -> Bounds {
    match (left, right) {
        (Some(left), Some(right)) => left.join(&right).map(Some).ok_or(BoundsError::Geometry),
        (left, right) => Ok(left.or(right)),
    }
}

fn clip_bounds(clip: &usvg::ClipPath, transform: usvg::Transform, budget: &mut Budget, depth: usize) -> Bounds {
    check_depth(depth).map_err(BoundsError::Resource)?;
    budget.take(1, "measuring SVG clip ancestry").map_err(BoundsError::Resource)?;
    if !clip.root().has_children() {
        return Ok(None);
    }
    let clip_transform = transform.pre_concat(clip.transform());
    let mut bounds = clip.root().abs_bounding_box().transform(clip_transform).ok_or(BoundsError::Geometry)?;
    if let Some(parent) = clip.clip_path() {
        let Some(other) = clip_bounds(parent, transform, budget, depth + 1)? else { return Ok(None) };
        let Some(intersection) = bounds.intersect(&other) else { return Ok(None) };
        bounds = intersection;
    }
    Ok((!bounds.is_empty()).then_some(bounds))
}

fn svg_bounds(group: &usvg::Group, budget: &mut Budget, depth: usize) -> Bounds {
    check_depth(depth).map_err(BoundsError::Resource)?;
    budget.take(1, "measuring SVG groups").map_err(BoundsError::Resource)?;
    if group.opacity().get() == 0.0 {
        return Ok(None);
    }
    let mut bounds = None;
    for node in group.children() {
        budget.take(1, "measuring SVG paint").map_err(BoundsError::Resource)?;
        let child = match node {
            usvg::Node::Group(child) => svg_bounds(child, budget, depth + 1)?,
            usvg::Node::Path(path) => {
                let painted = path.fill().is_some_and(|fill| fill.opacity().get() > 0.0)
                    || path.stroke().is_some_and(|stroke| stroke.opacity().get() > 0.0);
                (path.is_visible() && painted).then(|| path.abs_stroke_bounding_box())
            }
            usvg::Node::Image(image) => {
                if !image.is_visible() {
                    None
                } else if let usvg::ImageKind::SVG(tree) = image.kind() {
                    match svg_bounds(tree.root(), budget, depth + 1)? {
                        Some(bounds) => bounds.transform(image.abs_transform()).ok_or(BoundsError::Geometry)?
                            .intersect(&image.abs_bounding_box()),
                        None => None,
                    }
                } else {
                    Some(image.abs_bounding_box())
                }
            }
            usvg::Node::Text(text) => svg_bounds(text.flattened(), budget, depth + 1)?,
        };
        bounds = union(bounds, child.filter(|rect| !rect.is_empty()))?;
    }
    if !group.filters().is_empty() {
        // Filters may expand or independently produce paint (e.g. feFlood).
        let layer = group.abs_layer_bounding_box();
        bounds = Some(usvg::Rect::from_xywh(layer.x(), layer.y(), layer.width(), layer.height()).ok_or(BoundsError::Geometry)?);
    }
    if let (Some(rect), Some(clip)) = (bounds, group.clip_path()) {
        bounds = match clip_bounds(clip, group.abs_transform(), budget, depth + 1)? {
            Some(clip) => rect.intersect(&clip),
            None => None,
        };
    }
    Ok(bounds.filter(|rect| !rect.is_empty()))
}

/// Extract ordered semantic occurrences from this exact finished document.
/// Final raster/PDF rendering remains the responsibility of compile.rs and uses
/// the original pages, not any of the temporary pruned frames below.
pub fn extract(world: &dyn World, document: &PagedDocument) -> Result<Value, String> {
    let mut index = Index::default();
    index.budget.take(document.pages().len(), "indexing preview pages")?;
    let mut stack = Vec::new();
    let plans: Vec<_> = document.pages().iter()
        .map(|page| index.scan(&page.frame, &mut stack, 0))
        .collect::<Result<_, _>>()?;
    index.finish(&stack)?;
    let mut records = Vec::new();
    let mut output_budget = Budget { remaining: METADATA_BYTES };
    for element in &index.elements {
        let Some(kind) = element.kind else { continue };
        let range = source_range(world, element.content);
        let parent = element.owners.iter().copied()
            .find(|owner| index.elements[*owner].wire_id != element.wire_id)
            .map(|owner| index.elements[owner].wire_id).unwrap_or(0);
        let block = element.content.to_packed::<EquationElem>()
            .map(|equation| equation.block.get(StyleChain::default()))
            .unwrap_or(true);
        let status = if element.invalid { "unresolved" }
            else if range.is_none() { "unmapped" } else { "empty" };
        let range = range.unwrap_or_else(|| json!({"file":"", "start":-1, "end":-1}));
        output_budget.take(256 + range["file"].as_str().map(str::len).unwrap_or(0), "emitting preview occurrences")?;
        records.push(json!({
            "id": element.wire_id, "parent": parent, "kind": kind,
            "status": status, "block": block,
            "file": range["file"], "start": range["start"], "end": range["end"],
            "regions": []
        }));
    }
    let mut prune_budget = Budget::default();
    let mut svg_budget = Budget::default();
    let options = usvg::Options { dpi: 72.0, ..Default::default() };
    for (page_index, (page, plan)) in document.pages().iter().zip(&plans).enumerate() {
        let mut parts: Vec<_> = prune(&page.frame, plan, &index, &mut prune_budget)?.into_iter().collect();
        parts.sort_by_key(|((owner, _), part)| (*owner, part.order));
        for ((owner, _), part) in parts {
            let record = &mut records[index.elements[owner].wire_id - 1];
            if record["status"] == "unresolved" {
                continue;
            }
            let mut selected = page.clone();
            selected.frame = part.frame;
            selected.fill = Smart::Custom(None);
            let svg = typst_svg::svg(&selected, &Default::default());
            if svg.len() > METADATA_BYTES {
                return Err(resource_error("preparing SVG preview bounds"));
            }
            let measured = usvg::Tree::from_str(&svg, &options).map_err(|_| BoundsError::Geometry)
                .and_then(|tree| svg_bounds(tree.root(), &mut svg_budget, 0));
            let rect = match measured {
                Ok(Some(rect)) => rect,
                Ok(None) => continue,
                Err(BoundsError::Resource(message)) => return Err(message),
                Err(BoundsError::Geometry) => {
                    record["status"] = json!("unresolved");
                    record["regions"] = json!([]);
                    continue;
                }
            };
            let width = page.frame.width().to_pt();
            let height = page.frame.height().to_pt();
            let x = (f64::from(rect.left()) - f64::from(ANTIALIAS_PAD)).max(0.0);
            let y = (f64::from(rect.top()) - f64::from(ANTIALIAS_PAD)).max(0.0);
            let right = (f64::from(rect.right()) + f64::from(ANTIALIAS_PAD)).min(width);
            let bottom = (f64::from(rect.bottom()) + f64::from(ANTIALIAS_PAD)).min(height);
            if ![x, y, right, bottom, width, height].iter().all(|value| value.is_finite()) {
                record["status"] = json!("unresolved");
                record["regions"] = json!([]);
                continue;
            }
            if right <= x || bottom <= y {
                continue;
            }
            output_budget.take(256, "emitting preview regions")?;
            if let Some(regions) = record["regions"].as_array_mut() {
                regions.push(json!({"page":page_index, "x":x, "y":y, "width":right-x, "height":bottom-y}));
            }
            if record["status"] == "empty" {
                record["status"] = json!("ready");
            }
        }
    }
    Ok(Value::Array(records))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::world::{Config, Overlay, ProjectWorld, byte_offset};

    fn compile(source: &str) -> (ProjectWorld, PagedDocument) {
        let config = Config {
            project_root: std::env::temp_dir(),
            main_file: "main.typ".into(),
            ..Default::default()
        };
        let mut world = ProjectWorld::new(config.clone()).expect("test world");
        world.update(config, vec![Overlay { file: "main.typ".into(), text: source.into() }])
            .expect("source overlay");
        let document = typst::compile::<PagedDocument>(&world).output.expect("fixture compiles");
        (world, document)
    }

    fn records(source: &str) -> Vec<Value> {
        let (world, document) = compile(source);
        extract(&world, &document).expect("preview extraction").as_array().expect("preview array").clone()
    }

    fn text(frame: &Frame) -> String {
        let mut result = String::new();
        for (_, item) in frame.items() {
            match item {
                FrameItem::Text(run) => result.push_str(&run.text),
                FrameItem::Group(group) => result.push_str(&text(&group.frame)),
                _ => {}
            }
        }
        result
    }

    #[test]
    fn repeated_source_has_ordered_distinct_occurrences() {
        let source = "#set page(width: 100mm, height: 80mm, margin: 8mm)\n#for n in (2, 7, 11) { [Before $ E = #n c^2 $ after.] }";
        let (world, document) = compile(source);
        let output = extract(&world, &document).expect("preview extraction");
        assert_eq!(output, extract(&world, &document).expect("preview extraction"));
        let output = output.as_array().unwrap();
        assert_eq!(output.len(), 3);
        for (i, record) in output.iter().enumerate() {
            assert_eq!(record["id"], i + 1);
            assert_eq!(record["start"], output[0]["start"]);
            assert_eq!(record["end"], output[0]["end"]);
            assert_eq!(record["status"], "ready");
            assert_eq!(record["regions"].as_array().unwrap().len(), 1);
            if i > 0 {
                assert!(record["regions"][0]["y"].as_f64().unwrap() > output[i - 1]["regions"][0]["y"].as_f64().unwrap());
            }
        }
    }

    #[test]
    fn equation_continues_across_pages_without_new_occurrence() {
        let output = records("#set page(width: 100mm, height: 30mm, margin: 4mm)\n#show math.equation: set block(breakable: true)\nBefore\n$ x &= 1 \\ x &= 2 \\ x &= 3 \\ x &= 4 \\ x &= 5 \\ x &= 6 \\ x &= 7 $\nAfter");
        assert_eq!(output.len(), 1);
        assert_eq!(output[0]["status"], "ready");
        assert_eq!(output[0]["block"], true);
        let parts = output[0]["regions"].as_array().unwrap();
        assert_eq!(parts.len(), 2);
        assert_eq!(parts[0]["page"], 0);
        assert_eq!(parts[1]["page"], 1);
    }

    #[test]
    fn detached_footnote_is_a_separate_short_region() {
        let output = records("#set page(width: 100mm, height: 60mm, margin: 8mm)\n#show math.equation: it => [#it#footnote[Owned note]]\nBefore $sqrt(x)$ after.");
        assert_eq!(output.len(), 1);
        assert_eq!(output[0]["status"], "ready");
        let parts = output[0]["regions"].as_array().unwrap();
        assert_eq!(parts.len(), 2);
        assert_eq!(parts[0]["page"], parts[1]["page"]);
        for part in parts {
            assert!(part["height"].as_f64().unwrap() < 35.0);
        }
        let first_bottom = parts[0]["y"].as_f64().unwrap() + parts[0]["height"].as_f64().unwrap();
        assert!(parts[1]["y"].as_f64().unwrap() - first_bottom > 60.0);
    }

    #[test]
    fn clipping_limits_bounds_and_fully_clipped_math_is_empty() {
        let output = records("#set page(width: 100mm, height: 80mm, margin: 8mm)\n#box(width: 8pt, height: 10pt, clip: true)[$sqrt(x)$]");
        let part = &output[0]["regions"][0];
        assert_eq!(output[0]["status"], "ready");
        assert!(part["width"].as_f64().unwrap() <= 9.01);
        assert!(part["height"].as_f64().unwrap() <= 11.01);
        let output = records("#box(width: 8pt, height: 10pt, clip: true)[#move(dx: 40pt)[$sqrt(x)$]]");
        assert_eq!(output[0]["status"], "empty");
        assert_eq!(output[0]["regions"], json!([]));
    }

    #[test]
    fn show_rules_own_wrapper_and_replacement_not_neighbors() {
        for (rule, expected) in [
            ("it => box(fill: yellow, inset: 5pt, stroke: red, it)", "√"),
            ("it => [Replacement]", "Replacement"),
        ] {
            let source = format!("#set page(width: 100mm, height: 80mm, margin: 8mm)\n#show math.equation: {rule}\nBefore $sqrt(x)$ After.");
            let (world, document) = compile(&source);
            let mut index = Index::default();
            let mut stack = Vec::new();
            let plans: Vec<_> = document.pages().iter().map(|page| index.scan(&page.frame, &mut stack, 0).expect("semantic indexing")).collect();
            index.finish(&stack).expect("ownership resolution");
            let parts = prune(&document.pages()[0].frame, &plans[0], &index, &mut Budget::default()).expect("preview pruning");
            let owned: String = parts.values().map(|part| text(&part.frame)).collect();
            assert!(owned.contains(expected));
            assert!(!owned.contains("Before") && !owned.contains("After"));
            let output = extract(&world, &document).expect("preview extraction");
            assert_eq!(output[0]["status"], "ready");
            assert!(output[0]["regions"][0]["width"].as_f64().unwrap() < 100.0);
        }
        let output = records("#show math.equation: it => []\nBefore $x$ After");
        assert_eq!(output[0]["status"], "empty");
        assert_eq!(output[0]["regions"], json!([]));
    }

    #[test]
    fn figure_nesting_and_hash_introducer_use_utf16_boundaries() {
        let source = "😀 é\n#figure(table(columns: 2, [A], [B], [1], [2]), caption: [Sample])";
        let (world, document) = compile(source);
        let output = extract(&world, &document).expect("preview extraction");
        let output = output.as_array().unwrap();
        let figure = output.iter().find(|r| r["kind"] == "figure").unwrap();
        let table = output.iter().find(|r| r["kind"] == "table").unwrap();
        assert_eq!(table["parent"], figure["id"]);
        let snapshot = world.source(world.main()).unwrap();
        let start = byte_offset(&snapshot, figure["start"].as_u64().unwrap() as usize).unwrap();
        let end = byte_offset(&snapshot, figure["end"].as_u64().unwrap() as usize).unwrap();
        assert_eq!(&source[start..end], "#figure(table(columns: 2, [A], [B], [1], [2]), caption: [Sample])");
        assert_eq!(figure["start"], "😀 é\n".encode_utf16().count());
        assert!(byte_offset(&snapshot, 1).is_none());
        let start = byte_offset(&snapshot, table["start"].as_u64().unwrap() as usize).unwrap();
        let end = byte_offset(&snapshot, table["end"].as_u64().unwrap() as usize).unwrap();
        assert_eq!(&source[start..end], "table(columns: 2, [A], [B], [1], [2])");
    }

    #[test]
    fn realized_block_flag_is_not_inferred_from_source_whitespace() {
        let output = records("#math.equation(block: true, [x])\n\n#math.equation(block: false, [y])");
        assert_eq!(output.len(), 2);
        assert_eq!(output[0]["block"], true);
        assert_eq!(output[1]["block"], false);
        assert_eq!(output[0]["status"], "ready");
        assert_eq!(output[1]["status"], "ready");
    }

    #[test]
    fn transformed_and_parent_gradient_equations_keep_local_bounds() {
        for body in [
            "Before #rotate(20deg, box($sqrt(x)$)) After.",
            "#set text(fill: gradient.linear(red, blue, relative: \"parent\"))\nBefore $sqrt(x)$ After.",
            "Before #box(fill: black, inset: 4pt)[#text(fill: white)[$sqrt(x)$]] After.",
        ] {
            let output = records(&format!("#set page(width: 100mm, height: 80mm, margin: 8mm)\n{body}"));
            assert_eq!(output.len(), 1);
            assert_eq!(output[0]["status"], "ready");
            assert!(output[0]["regions"][0]["width"].as_f64().unwrap() < 40.0);
            assert!(output[0]["regions"][0]["height"].as_f64().unwrap() < 40.0);
        }
    }


    #[test]
    fn image_occurrences_and_transparent_images_keep_real_status() {
        let output = records(r##"#figure(image(bytes("<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'><rect width='20' height='10' fill='red'/></svg>"), width: 20pt), caption: [Image])"##);
        let figure = output.iter().find(|record| record["kind"] == "figure").unwrap();
        let image = output.iter().find(|record| record["kind"] == "image").unwrap();
        assert_eq!(image["parent"], figure["id"]);
        assert_eq!(image["status"], "ready");
        assert!((image["regions"][0]["width"].as_f64().unwrap() - 21.0).abs() < 0.1);
        let output = records(r##"#image(bytes((255, 0, 0, 0)), format: (encoding: "rgba8", width: 1, height: 1), width: 20pt)"##);
        assert_eq!(output[0]["kind"], "image");
        assert_eq!(output[0]["status"], "empty");
        assert_eq!(output[0]["regions"], json!([]));
    }

    #[test]
    fn copied_finished_frames_are_occurrences_not_page_continuations() {
        let (world, document) = compile("$x$");
        let page = document.pages()[0].clone();
        let duplicated = PagedDocument::new(vec![page.clone(), page].into(), Default::default());
        let output = extract(&world, &duplicated).expect("preview extraction");
        let output = output.as_array().unwrap();
        assert_eq!(output.len(), 2);
        assert_eq!(output[0]["start"], output[1]["start"]);
        assert_ne!(output[0]["id"], output[1]["id"]);
        assert_eq!(output[0]["regions"][0]["page"], 0);
        assert_eq!(output[1]["regions"][0]["page"], 1);
    }

    #[test]
    fn malformed_ownership_never_produces_a_crop() {
        let (_, document) = compile("$x$");
        for missing_parent in [false, true] {
            let mut index = Index::default();
            let mut stack = Vec::new();
            let plan = index.scan(&document.pages()[0].frame, &mut stack, 0).expect("semantic indexing");
            let equation = index.elements.iter().position(|element| element.kind == Some("equation")).unwrap();
            let location = index.elements[equation].content.location().unwrap();
            if missing_parent {
                index.locations.remove(&location);
            }
            let link = index.logical.len();
            index.logical.push(LogicalParent { location, preferred: None, resolved: None });
            // Self ancestry when resolvable, missing ancestry otherwise.
            index.elements[equation].parent = Some(Scope::Logical(link));
            index.finish(&stack).expect("ownership resolution");
            assert!(index.elements[equation].invalid);
            assert!(prune(&document.pages()[0].frame, &plan, &index, &mut Budget::default()).expect("preview pruning").is_empty());
        }
    }

    #[test]
    fn source_validation_rejects_nonexpression_and_detached_spans() {
        let (world, _) = compile("$x$");
        // spanned() only assigns a previously detached span, so start with
        // fresh content instead of trying to overwrite a compiled equation.
        let detached = Content::new(EquationElem::new(Content::empty()));
        assert!(source_range(&world, &detached).is_none());
        let source = world.source(world.main()).unwrap();
        let nonexpression = detached.spanned(source.root().span());
        assert!(source_range(&world, &nonexpression).is_none());
    }

    #[test]
    fn stroked_transformed_clips_are_measured_in_canvas_points() {
        let svg = r##"<svg xmlns="http://www.w3.org/2000/svg" width="100pt" height="100pt">
          <defs><clipPath id="clip"><rect width="8" height="10"/></clipPath></defs>
          <g transform="translate(20 30) scale(2)" clip-path="url(#clip)">
            <rect x="-5" y="-5" width="30" height="30" fill="none" stroke="black" stroke-width="20"/>
          </g></svg>"##;
        let tree = usvg::Tree::from_str(svg, &usvg::Options { dpi: 72.0, ..Default::default() }).unwrap();
        let bounds = svg_bounds(tree.root(), &mut Budget::default(), 0).unwrap().unwrap();
        assert_eq!((bounds.x(), bounds.y(), bounds.width(), bounds.height()), (20.0, 30.0, 16.0, 20.0));
    }


    #[test]
    fn numbered_equation_owns_its_number_but_inline_math_does_not() {
        let source = "#set page(width: 100mm, height: 80mm, margin: 8mm)\n#set math.equation(numbering: \"(1)\")\n#let mass = 7\nBefore $E = #mass c^2$ after.\n\n$ E = #mass c^2 $";
        let (world, document) = compile(source);
        let output = extract(&world, &document).expect("preview extraction");
        assert_eq!(output.as_array().unwrap().len(), 2);
        assert_eq!(output[0]["block"], false);
        assert_eq!(output[1]["block"], true);
        let mut index = Index::default();
        let mut stack = Vec::new();
        let plan = index.scan(&document.pages()[0].frame, &mut stack, 0).expect("semantic indexing");
        index.finish(&stack).expect("ownership resolution");
        let parts = prune(&document.pages()[0].frame, &plan, &index, &mut Budget::default()).expect("preview pruning");
        for ((owner, _), part) in parts {
            let owned = text(&part.frame);
            let record = &output[index.elements[owner].wire_id - 1];
            assert_eq!(record["status"], "ready");
            assert!(owned.contains('7'));
            assert!(!owned.contains("Before") && !owned.contains("after"));
            assert_eq!(owned.contains("(1)"), record["block"].as_bool().unwrap());
        }
    }

    #[test]
    fn missing_closes_are_unresolved_not_neighbor_ownership() {
        fn remove_closes(frame: &mut Frame) {
            frame.retain(|item| match item {
                FrameItem::Group(group) => { remove_closes(&mut group.frame); true }
                FrameItem::Tag(Tag::End(..)) => false,
                _ => true,
            });
        }
        let (_, document) = compile("Before $x$ After");
        let mut frame = document.pages()[0].frame.clone();
        remove_closes(&mut frame);
        let mut index = Index::default();
        let mut stack = Vec::new();
        let plan = index.scan(&frame, &mut stack, 0).expect("semantic indexing");
        index.finish(&stack).expect("ownership resolution");
        let equation = index.elements.iter().find(|element| element.kind == Some("equation")).unwrap();
        assert!(equation.invalid);
        assert!(prune(&frame, &plan, &index, &mut Budget::default()).expect("preview pruning").is_empty());
    }


    #[test]
    fn resource_limits_reject_before_collecting_partial_paint() {
        let (_, document) = compile("$x + y$");
        let frame = &document.pages()[0].frame;
        let mut limited = Index::default();
        limited.budget.remaining = 0;
        let error = limited.scan(frame, &mut Vec::new(), 0).err().expect("index cap");
        assert!(error.contains("resource limit"));
        assert!(limited.elements.is_empty());

        let mut index = Index::default();
        let mut stack = Vec::new();
        let plan = index.scan(frame, &mut stack, 0).expect("semantic indexing");
        index.finish(&stack).expect("ownership resolution");
        let error = prune(frame, &plan, &index, &mut Budget { remaining: 0 }).err().expect("pruned paint cap");
        assert!(error.contains("resource limit"));
        assert!(error.contains("pruned preview"));
    }

    #[test]
    fn deeply_nested_finished_output_returns_error_not_partial_previews() {
        let (world, document) = compile("$x$");
        let mut page = document.pages()[0].clone();
        for _ in 0..=MAX_DEPTH {
            let mut parent = empty_frame(&page.frame);
            parent.push(typst::layout::Point::zero(), FrameItem::Group(typst::layout::GroupItem::new(page.frame)));
            page.frame = parent;
        }
        let document = PagedDocument::new(vec![page].into(), Default::default());
        let error = extract(&world, &document).expect_err("depth cap must reject the entire extraction");
        assert!(error.contains("resource limit"));
        assert!(error.contains("nested"));
    }
}

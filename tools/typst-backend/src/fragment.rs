use comemo::Track;
use serde_json::{Value, json};
use typst::diag::SourceDiagnostic;
use typst::engine::{Engine, Route, Sink, Traced};
use typst::foundations::{Content, Context, NativeElement, Scope, Smart, StyleChain, Styles, Target, TargetElem};
use typst::introspection::{EmptyIntrospector, Locator};
use typst::layout::{Abs, Axes, Frame, FrameItem, Point, Region, Sides, Size};
use typst::math::EquationElem;
use typst::model::ParElem;
use typst::routines::SpanMode;
use typst::syntax::{Span, SyntaxMode};
use typst::utils::{Numeric, Protected};
use typst::World;
use typst_layout::{Page, PagedDocument};

use crate::world::FragmentWorld;

pub const MAX_SOURCE: usize = 64 * 1024;
pub const MAX_SVG: usize = 8 * 1024 * 1024;
pub const MAX_REPLY: usize = 64 * 1024;
const MAX_DIMENSION: f64 = 16384.0;
const PAGE_GAP: f64 = 8.0;

fn diagnostics(items: &[SourceDiagnostic]) -> Result<Vec<String>, String> {
    let mut bytes = 0usize;
    items.iter().map(|item| {
        bytes = bytes.saturating_add(item.message.len()).saturating_add(3);
        if bytes > MAX_REPLY - 512 { return Err("fragment diagnostics exceed 64 KiB".into()); }
        Ok(item.message.to_string())
    }).collect()
}

fn failure(items: impl AsRef<[SourceDiagnostic]>) -> String {
    match diagnostics(items.as_ref()).map(|items| items.join("\n")) {
        Ok(message) if message.len() <= (MAX_REPLY - 512) / 6 => message,
        _ => "fragment diagnostics exceed 64 KiB".into(),
    }
}

fn check_frame(frame: &Frame, markup: bool) -> Result<(), String> {
    for (_, item) in frame.items() {
        match item {
            FrameItem::Group(group) => check_frame(&group.frame, markup)?,
            FrameItem::Image(..) => return Err("fragments cannot contain images".into()),
            FrameItem::Link(..) if !markup => return Err("math fragments cannot contain links".into()),
            _ => {}
        }
    }
    Ok(())
}

pub fn render(request: &Value) -> Result<(Value, typst::foundations::Bytes), String> {
    let op = crate::text(request, "op")?;
    let markup = match op {
        "mathSvg" => false,
        "markupSvg" => true,
        _ => return Err("unknown fragment operation".into()),
    };
    let fields = ["v", "op", "epoch", "generation", "request", "source", "foreground", "sensitive"];
    let object = request.as_object().ok_or("fragment request must be an object")?;
    if object.keys().any(|key| !fields.contains(&key.as_str()) && (markup || key != "display")) {
        return Err("unknown fragment request field".into());
    }
    if object.get("sensitive").is_some_and(|value| !value.is_boolean()) {
        return Err("sensitive must be boolean".into());
    }
    let source = crate::text(request, "source")?;
    if source.trim().is_empty() || source.contains('\0') || source.len() > MAX_SOURCE {
        return Err("fragment source must be nonblank, NUL-free UTF-8 of at most 64 KiB".into());
    }
    let foreground = serde_json::from_value::<[u8; 4]>(request["foreground"].clone())
        .map_err(|_| "foreground must contain four RGBA bytes")?;
    if markup {
        return render_markup(request, source, foreground);
    }
    let display = request["display"].as_bool().ok_or("display must be boolean")?;
    render_math(request, source, display, foreground)
}

fn render_math(request: &Value, source: &str, display: bool, foreground: [u8; 4])
    -> Result<(Value, typst::foundations::Bytes), String>
{
    let world = FragmentWorld::math(foreground);
    let world = &world as &dyn World;
    let library = world.library();
    let introspector = EmptyIntrospector;
    let mut sink = Sink::new();
    let value = (library.routines.eval_string)(world.track(), library, sink.track_mut(),
        introspector.track(), Context::none().track(), source,
        SpanMode::Uniform(Span::detached()), SyntaxMode::Math, Scope::new())
        .map_err(failure)?;
    let typst::foundations::Value::Content(content) = value else {
        return Err("math evaluation did not return an equation".into());
    };
    let equation = content.to_packed::<EquationElem>().ok_or("math evaluation did not return an equation")?;
    let body = if display { typst::math::display(equation.body.clone(), false) } else { equation.body.clone() };
    // Keep inline placement. EquationElem's inline ShowSet resets outer size;
    // a body-level display style preserves display limits without block layout.
    let content = EquationElem::new(body).with_block(false).pack();
    let traced = Traced::default();
    let mut engine = Engine {
        world: world.track(), library, introspector: Protected::new(introspector.track()),
        traced: traced.track(), sink: sink.track_mut(), route: Route::root(),
    };
    let mut target = Styles::new();
    target.set(TargetElem::target, Target::Paged);
    // Inline math normally borrows paragraph leading outside its frame. A
    // standalone viewport must include that ink instead of borrowing space.
    target.set(ParElem::leading, Abs::zero().into());
    let base = StyleChain::new(&library.styles);
    let styles = base.chain(&target);
    let region = Region::new(Size::splat(Abs::inf()), Axes::splat(false));
    let mut frame = typst_layout::layout_frame(&mut engine, &content, Locator::root(), styles, region)
        .map_err(failure)?;
    if !sink.introspections().is_empty() {
        return Err("document introspection is unavailable in math fragments".into());
    }
    let delayed = sink.delayed();
    if !delayed.is_empty() { return Err(failure(delayed)); }
    check_frame(&frame, false)?;
    let size = frame.size();
    let baseline = frame.baseline();
    if !size.is_finite() || size.x <= Abs::zero() || size.y <= Abs::zero()
        || !baseline.to_pt().is_finite()
    {
        return Err("math frame has invalid geometry".into());
    }
    let padded = size + Size::splat(Abs::pt(2.0));
    if padded.x.to_pt() > MAX_DIMENSION || padded.y.to_pt() > MAX_DIMENSION {
        return Err("math frame exceeds 16384pt".into());
    }
    // Explicitly preserve the fallback baseline before resizing its viewport.
    frame.set_baseline(baseline);
    frame.translate(Point::splat(Abs::pt(1.0)));
    frame.set_size(padded);
    let page = Page { frame, bleed: Sides::splat(Abs::zero()), fill: Smart::Custom(None),
        numbering: None, supplement: Content::empty(), number: 1 };
    let svg = typst_svg::svg(&page, &typst_svg::SvgOptions { render_bleed: false, pretty: false });
    finish_svg(request, svg, padded, page.frame.baseline().to_pt(), 1, diagnostics(&sink.warnings())?)
}

fn render_markup(request: &Value, source: &str, foreground: [u8; 4])
    -> Result<(Value, typst::foundations::Bytes), String>
{
    let world = FragmentWorld::markup(foreground, source);
    // Full document evaluation/layout, including authored styles, show rules,
    // counters, queries and normal introspection convergence. Never wrap source.
    let result = typst::compile::<PagedDocument>(&world);
    let document = result.output.map_err(failure)?;
    let warnings = diagnostics(&result.warnings)?;
    let pages = document.pages();
    if pages.is_empty() || pages.len() > i32::MAX as usize {
        return Err("markup document has invalid page count".into());
    }
    let mut size = Size::new(Abs::zero(), Abs::pt(PAGE_GAP * pages.len().saturating_sub(1) as f64));
    for page in pages {
        let page_size = page.frame.size();
        check_geometry(page_size)?;
        check_frame(&page.frame, true)?;
        size.x = size.x.max(page_size.x);
        size.y += page_size.y;
        // Include every page and inter-page gap; no clipping or partial success.
        check_geometry(size)?;
    }
    let svg = typst_svg::svg_merged(&document,
        &typst_svg::SvgOptions { render_bleed: false, pretty: false }, Abs::pt(PAGE_GAP));
    finish_svg(request, svg, size, 0.0, pages.len(), warnings)
}

fn check_geometry(size: Size) -> Result<(), String> {
    if !size.is_finite() || size.x <= Abs::zero() || size.y <= Abs::zero()
        || size.x.to_pt() > MAX_DIMENSION || size.y.to_pt() > MAX_DIMENSION
    {
        return Err("fragment viewport must be finite, positive and at most 16384pt".into());
    }
    Ok(())
}

fn finish_svg(request: &Value, svg: String, size: Size, baseline: f64, pages: usize,
    diagnostics: Vec<String>) -> Result<(Value, typst::foundations::Bytes), String>
{
    if svg.is_empty() || svg.len() > MAX_SVG { return Err("fragment SVG exceeds 8 MiB".into()); }
    // The trusted exporter can also emit images from color glyphs or tiling
    // paints, outside the direct frame walk. Reject those whole outputs too.
    // Its only other non-local references are removable link annotations; glyph,
    // clip, gradient and tiling references are exporter-owned local definitions.
    if svg.contains("<image") || svg.contains("<foreignObject") {
        return Err("fragments cannot contain images".into());
    }
    let header = json!({"v":1, "op":request["op"], "epoch":request["epoch"],
        "generation":request["generation"], "request":request["request"],
        "widthPt":size.x.to_pt(), "heightPt":size.y.to_pt(),
        "baselinePt":baseline, "pageCount":pages, "diagnostics":diagnostics});
    if serde_json::to_vec(&header).map_err(|e| e.to_string())?.len() > MAX_REPLY {
        return Err("fragment header exceeds 64 KiB".into());
    }
    Ok((header, typst::foundations::Bytes::from_string(svg)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::world::project_id;
    use typst::diag::FileError;

    fn request(source: &str, display: bool) -> Value {
        json!({"v":1,"op":"mathSvg","epoch":1,"generation":1,"request":1,
            "source":source,"display":display,"foreground":[31,83,157,255]})
    }

    fn markup_request(source: &str) -> Value {
        json!({"v":1,"op":"markupSvg","epoch":1,"generation":1,"request":1,
            "source":source,"foreground":[31,83,157,255]})
    }

    #[test]
    fn markup_preserves_transparency_and_authored_styles() {
        let source = "#rect(width: 30pt, height: 20pt, fill: rgb(31, 83, 157, 199), stroke: none)";
        let (header, _) = render(&markup_request(source)).unwrap();
        assert_eq!(header["pageCount"], 1);
        assert_eq!(header["baselinePt"], 0.0);
        let world = FragmentWorld::markup([31, 83, 157, 255], source);
        let document = typst::compile::<PagedDocument>(&world).output.unwrap();
        let options = typst_render::RenderOptions {
            pixel_per_pt: typst::utils::Scalar::new(2.0), render_bleed: false,
        };
        let image = typst_render::render(&document.pages()[0], &options);
        assert_eq!(image.pixel(24, 24).unwrap().alpha(), 199);

        let authored = "#set page(width: 120pt, height: 80pt, margin: 0pt, fill: rgb(17, 23, 29))\n#set text(size: 24pt, fill: rgb(211, 37, 53))\n= Title\n";
        let (header, svg) = render(&markup_request(authored)).unwrap();
        assert_eq!(header["widthPt"], 120.0);
        assert_eq!(header["heightPt"], 80.0);
        let svg = std::str::from_utf8(&svg).unwrap();
        assert!(svg.contains("#11171d"));
        assert!(svg.contains("#d32535"));
    }

    #[test]
    fn markup_evaluates_document_grammar_context_and_all_pages() {
        let source = "#set page(width: 180pt, height: 180pt, margin: 8pt)\n\
            #show strong: it => text(fill: rgb(211, 37, 53), it)\n\
            = Heading\n*Styled* and _emphasized_ $sqrt(x)$\n\
            #let cell(n) = [#n]\n\
            #table(columns: (50pt, 50pt), [A], [B], cell(1), cell(2))\n\
            #context [Page #counter(page).display()]\n\
            #pagebreak()\n#set page(width: 100pt, height: 80pt)\n\
            #rect(width: 20pt, height: 12pt, fill: rgb(13, 173, 71), stroke: none)\n\
            #context [Page #counter(page).display()]";
        let (header, svg) = render(&markup_request(source)).unwrap();
        assert_eq!(header["pageCount"], 2);
        assert_eq!(header["widthPt"], 180.0);
        assert_eq!(header["heightPt"], 268.0);
        let svg = std::str::from_utf8(&svg).unwrap();
        assert!(svg.contains("#d32535"));
        assert!(svg.contains("#0dad47"));
        let world = FragmentWorld::markup([31, 83, 157, 255], source);
        let document = typst::compile::<PagedDocument>(&world).output.unwrap();
        for page in document.pages() {
            let image = typst_render::render(page, &typst_render::RenderOptions {
                pixel_per_pt: typst::utils::Scalar::new(2.0), render_bleed: false,
            });
            assert!(image.data().chunks_exact(4).filter(|pixel| pixel[3] > 128).count() > 100);
        }
    }

    #[test]
    fn markup_exposes_only_main_and_rejects_resources_and_images() {
        let source = "A private virtual document";
        let world = FragmentWorld::markup([0, 0, 0, 255], source);
        assert_eq!(world.source(world.main()).unwrap().text(), source);
        assert!(matches!(world.file(world.main()), Err(FileError::AccessDenied)));
        let other = project_id("other.typ").unwrap();
        assert!(matches!(world.source(other), Err(FileError::AccessDenied)));
        assert!(matches!(world.file(other), Err(FileError::AccessDenied)));
        assert!(world.today(None).is_none());
        for source in ["", " \n\t", "a\0b", "#let bad = (", "#read(\"private.txt\")",
            "#read(\"fragment.typ\")", "#include \"other.typ\"", "#import \"other.typ\": *",
            "#import \"@preview/mitex:0.2.4\": *", "#datetime.today()",
            "#image(\"https://example.com/image.png\")"] {
            assert!(render(&markup_request(source)).is_err(), "accepted {source:?}");
        }
        let image = "image(bytes(\"<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><rect width='8' height='8' fill='red'/></svg>\"), format: \"svg\")";
        for source in [format!("Before #{image} after"),
            format!("Visible first page #pagebreak() #{image}"),
            format!("#rect(width: 30pt, height: 30pt, fill: tiling(size: (8pt, 8pt))[#{image}])")] {
            assert_eq!(render(&markup_request(&source)).unwrap_err(), "fragments cannot contain images");
        }
        // Links are annotations; visible linked text stays outside the overlay.
        let (_, svg) = render(&markup_request("#link(\"https://example.com\")[Visible link]")).unwrap();
        let svg = std::str::from_utf8(&svg).unwrap();
        assert!(svg.contains("<a"));
        assert!(svg.contains("<path"));
        assert!(svg.contains("#1f539d"));
    }

    #[test]
    fn markup_bounds_and_schema_fail_closed() {
        let boundary = format!("x{}", " ".repeat(MAX_SOURCE - 1));
        assert!(render(&markup_request(&boundary)).is_ok());
        assert!(render(&markup_request(&(boundary + " "))).is_err());
        for source in ["#set page(width: 16385pt, height: 20pt, margin: 0pt)\nx",
            "#set page(width: 40pt, height: 9000pt, margin: 0pt)\nx #pagebreak() y",
            "#set page(width: 0pt, height: 20pt, margin: 0pt)\nx",
            "#box(width: 1pt, height: 1pt, stack(spacing: 0pt, ..range(100000).map(_ => [x])))"] {
            assert!(render(&markup_request(source)).is_err(), "accepted {source:?}");
        }
        let error = render(&markup_request("#panic(\"x\" * 70000)")).unwrap_err();
        assert!(error.len() < MAX_REPLY / 6);
        let mut request = markup_request("Hello");
        request["display"] = json!(false);
        assert_eq!(render(&request).unwrap_err(), "unknown fragment request field");
        request.as_object_mut().unwrap().remove("display");
        request["sensitive"] = json!("true");
        assert_eq!(render(&request).unwrap_err(), "sensitive must be boolean");
    }

    #[test]
    fn alternating_kinds_and_sensitive_completion_keep_sessions_stateless() {
        let mut session = crate::compile::Session::default();
        for sensitive in [false, true] {
            for mut request in [markup_request("= Heading\n$frac(1, 2)$"), request("sqrt(x)", false)] {
                request["sensitive"] = json!(sensitive);
                let mut replies = 0;
                session.handle(&request, &mut |header, svg| {
                    replies += 1;
                    assert_eq!(header["op"], request["op"]);
                    assert_eq!(header["pageCount"], 1);
                    assert!(header["widthPt"].as_f64().unwrap() > 2.0);
                    assert!(std::str::from_utf8(&svg).unwrap().contains("#1f539d"));
                    Ok(())
                }).unwrap();
                assert_eq!(replies, 1);
                assert_eq!(session.dependencies(), json!([]));
                assert_eq!(session.handle(&request, &mut |_, _| Err("delivery failed".into())).unwrap_err(), "delivery failed");
                assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    session.handle(&request, &mut |_, _| panic!("delivery unwind"))
                })).is_err());
                request["source"] = json!("#let broken = (");
                assert!(session.handle(&request, &mut |_, _| panic!("partial result")).is_err());
            }
        }
    }

    #[test]
    fn sensitivity_is_optional_and_strictly_boolean() {
        let mut session = crate::compile::Session::default();
        let mut expected_geometry = None;
        for sensitive in [None, Some(false), Some(true)] {
            let mut input = request("frac(a^2 + b^2, sqrt(c))", false);
            if let Some(sensitive) = sensitive { input["sensitive"] = json!(sensitive); }
            let mut replies = 0;
            session.handle(&input, &mut |header, svg| {
                replies += 1;
                assert_eq!(header["op"], "mathSvg");
                let geometry = (header["widthPt"].clone(), header["heightPt"].clone(),
                    header["baselinePt"].clone());
                if let Some(expected) = &expected_geometry {
                    assert_eq!(&geometry, expected);
                } else {
                    expected_geometry = Some(geometry);
                }
                let svg = std::str::from_utf8(&svg).unwrap();
                assert!(svg.contains("<path"));
                assert!(svg.contains("#1f539d"));
                Ok(())
            }).unwrap();
            assert_eq!(replies, 1);
            assert_eq!(session.dependencies(), json!([]));
        }
        for value in [Value::Null, json!(0), json!(1), json!(-1), json!(0.5),
            json!(""), json!("false"), json!("true"), json!([]), json!({})] {
            let mut invalid = request("sqrt(x)", false);
            invalid["sensitive"] = value;
            assert_eq!(render(&invalid).unwrap_err(), "sensitive must be boolean");
            let error = session.handle(&invalid, &mut |_, _| {
                panic!("invalid sensitivity must not publish SVG");
            }).unwrap_err();
            assert_eq!(error, "sensitive must be boolean");
        }
    }

    #[test]
    fn math_completion_covers_delivery_failure_and_unwind() {
        for sensitive in [false, true] {
            let mut session = crate::compile::Session::default();
            let mut input = request("frac(a^2 + b^2, sqrt(c))", false);
            input["sensitive"] = json!(sensitive);
            let error = session.handle(&input, &mut |_, svg| {
                assert!(std::str::from_utf8(&svg).unwrap().contains("<path"));
                Err("reply delivery failed".into())
            }).unwrap_err();
            assert_eq!(error, "reply delivery failed");

            // Unwind after real evaluation to exercise the dispatch guard without
            // injecting a compiler/cache substitute or changing FFI panic handling.
            let mut delivered = false;
            let unwound = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                session.handle(&input, &mut |_, svg| {
                    assert!(std::str::from_utf8(&svg).unwrap().contains("<path"));
                    delivered = true;
                    panic!("math reply delivery unwind");
                })
            }));
            assert!(delivered);
            assert!(unwound.is_err());

            input["source"] = json!("frac(");
            assert!(session.handle(&input, &mut |_, _| {
                panic!("invalid math must not publish SVG");
            }).is_err());
            input["source"] = json!("sqrt(x)");
            let mut replies = 0;
            session.handle(&input, &mut |header, svg| {
                replies += 1;
                assert_eq!(header["op"], "mathSvg");
                assert!(std::str::from_utf8(&svg).unwrap().contains("<path"));
                Ok(())
            }).unwrap();
            assert_eq!(replies, 1);
        }
    }

    #[test]
    fn fragments_have_intrinsic_geometry_and_display_limits() {
        let (text, svg) = render(&request("sum_(i=1)^n i", false)).unwrap();
        let (display, _) = render(&request("sum_(i=1)^n i", true)).unwrap();
        assert!(display["heightPt"].as_f64().unwrap() > text["heightPt"].as_f64().unwrap());
        assert!(text["widthPt"].as_f64().unwrap() < 150.0);
        assert!(text["heightPt"].as_f64().unwrap() < 100.0);
        assert!(text["baselinePt"].as_f64().unwrap().is_finite());
        let svg = std::str::from_utf8(&svg).unwrap();
        assert!(svg.contains("<path"));
        assert!(svg.contains("#1f539d"));
        let (fraction, _) = render(&request("frac(a^2 + b^2, sqrt(c))", false)).unwrap();
        assert!(fraction["heightPt"].as_f64().unwrap() > 12.0);
    }

    #[test]
    fn resources_and_invalid_inputs_fail_locally() {
        let world = FragmentWorld::math([0, 0, 0, 255]);
        let id = project_id("private.typ").unwrap();
        assert!(matches!(world.file(id), Err(FileError::AccessDenied)));
        assert!(matches!(world.source(id), Err(FileError::AccessDenied)));
        assert!(world.today(None).is_none());
        for source in ["", " \n\t", "x\0y", "frac(", r"\frac{a}{b}",
            "#read(\"private.txt\")", "#import \"private.typ\": *",
            "#import \"@preview/mitex:0.2.4\": *",
            "#link(\"https://example.com\")[$x$]",
            "#context counter(page).display()"] {
            for sensitive in [false, true] {
                let mut input = request(source, false);
                input["sensitive"] = json!(sensitive);
                let mut session = crate::compile::Session::default();
                assert!(session.handle(&input, &mut |_, _| {
                    panic!("invalid math must not publish SVG");
                }).is_err(), "accepted {source:?} (sensitive={sensitive})");
            }
        }
        let mut invalid = request("x", false);
        invalid["root"] = json!(".");
        assert!(render(&invalid).is_err());
        invalid = request("x", false);
        invalid["foreground"] = json!([256, 0, 0, 0]);
        assert!(render(&invalid).is_err());
        assert!(render(&request("sqrt(x)", false)).is_ok());
    }

    #[test]
    fn source_and_output_limits_are_enforced() {
        let boundary = format!("x{}", " ".repeat(MAX_SOURCE - 1));
        assert!(render(&request(&boundary, false)).is_ok());
        assert!(render(&request(&(boundary + " "), false)).is_err());
        assert!(render(&request("#box(width: 16384pt)[$x$]", false)).is_err());
        // Real evaluation creates a small viewport with enough repeated painted
        // items to exceed the SVG byte budget, not a mocked oversized reply.
        assert!(render(&request("#box(width: 1pt, height: 1pt, stack(spacing: 0pt, ..range(100000).map(_ => [$x$])))", false)).is_err());
    }

    #[test]
    fn fragment_session_does_not_initialize_a_project() {
        let mut session = crate::compile::Session::default();
        let mut headers = Vec::new();
        session.handle(&request("sqrt(x)", false), &mut |header, _| {
            headers.push(header); Ok(())
        }).unwrap();
        assert_eq!(headers[0]["op"], "mathSvg");
        assert_eq!(session.dependencies(), json!([]));
        let mut compile = request("x", false);
        compile["op"] = json!("compile");
        assert!(session.handle(&compile, &mut |_, _| Ok(())).unwrap_err().contains("not initialized"));
    }
}

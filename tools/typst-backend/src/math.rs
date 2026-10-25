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
use typst_layout::Page;

use crate::world::MathWorld;

pub const MAX_SOURCE: usize = 64 * 1024;
pub const MAX_SVG: usize = 8 * 1024 * 1024;
pub const MAX_REPLY: usize = 64 * 1024;
const MAX_DIMENSION: f64 = 16384.0;

fn diagnostics(items: &[SourceDiagnostic]) -> Result<Vec<String>, String> {
    let mut bytes = 0usize;
    items.iter().map(|item| {
        bytes = bytes.saturating_add(item.message.len());
        if bytes > MAX_REPLY - 512 { return Err("math diagnostics exceed 64 KiB".into()); }
        Ok(item.message.to_string())
    }).collect()
}

fn failure(items: impl AsRef<[SourceDiagnostic]>) -> String {
    match diagnostics(items.as_ref()).map(|items| items.join("\n")) {
        Ok(message) if message.len() <= (MAX_REPLY - 512) / 6 => message,
        _ => "math diagnostics exceed 64 KiB".into(),
    }
}

fn check_frame(frame: &Frame) -> Result<(), String> {
    for (_, item) in frame.items() {
        match item {
            FrameItem::Group(group) => check_frame(&group.frame)?,
            FrameItem::Image(..) | FrameItem::Link(..) => {
                return Err("math fragments cannot contain images or links".into());
            }
            _ => {}
        }
    }
    Ok(())
}

pub fn render(request: &Value) -> Result<(Value, typst::foundations::Bytes), String> {
    let fields = ["v", "op", "epoch", "generation", "request", "source", "display", "foreground", "sensitive"];
    let object = request.as_object().ok_or("math request must be an object")?;
    if object.keys().any(|key| !fields.contains(&key.as_str())) {
        return Err("unknown math request field".into());
    }
    if object.get("sensitive").is_some_and(|value| !value.is_boolean()) {
        return Err("sensitive must be boolean".into());
    }
    let source = crate::text(request, "source")?;
    if source.trim().is_empty() || source.contains('\0') || source.len() > MAX_SOURCE {
        return Err("math source must be nonblank, NUL-free UTF-8 of at most 64 KiB".into());
    }
    let display = request["display"].as_bool().ok_or("display must be boolean")?;
    let foreground = serde_json::from_value::<[u8; 4]>(request["foreground"].clone())
        .map_err(|_| "foreground must contain four RGBA bytes")?;
    let world = MathWorld::new(foreground);
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
    check_frame(&frame)?;
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
    if svg.is_empty() || svg.len() > MAX_SVG { return Err("math SVG exceeds 8 MiB".into()); }
    let header = json!({"v":1, "op":"mathSvg", "epoch":request["epoch"],
        "generation":request["generation"], "request":request["request"],
        "widthPt":padded.x.to_pt(), "heightPt":padded.y.to_pt(),
        "baselinePt":page.frame.baseline().to_pt(), "diagnostics":diagnostics(&sink.warnings())?});
    if serde_json::to_vec(&header).map_err(|e| e.to_string())?.len() > MAX_REPLY {
        return Err("math header exceeds 64 KiB".into());
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
        let world = MathWorld::new([0, 0, 0, 255]);
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

#set page(width: 140mm, height: 140mm, margin: 12mm)
#set text(size: 12pt)
#import "lib.typ": mass

= Native Typst preview
BEFORE smoke. Select and copy this text.

Inline formula: $E = #mass c^2$.

#set math.equation(numbering: "(1)")
$ E = #mass c^2 $

#figure(table(columns: 2, [Quantity], [Value], [Mass], [#mass]), caption: [Live table])

// Switch between Side-by-side, Read, and In-place preview using the mode selector.
// The source below uses only built-ins and the demo's unsaved lib.typ import.
#set heading(numbering: "1.1")
#set page(numbering: "1")
#pagebreak()
#outline(title: [Feature guide], depth: 1)

= Text and document structure <text-demo>

*Strong*, _emphasis_, #underline[underlined], #strike[struck through],
#highlight[highlighted], and `inline code` can share a paragraph.
Use #super[superscripts], #sub[subscripts], and #smallcaps[Small capitals].

#link("https://typst.app/docs/")[Typst documentation] is a clickable PDF link.
Footnotes are laid out automatically.#footnote[This note belongs to the surrounding text.]

#quote(block: true)[A document is both editable source and structured, typeset content.]

== Set and show rules

#{
  set text(fill: blue)
  show "Typst": strong
  [This scoped style makes Typst bold and this paragraph blue.]
}

```typst
#let twice(value) = value * 2
#twice(21)
```

= Lists and terms

- A bullet item
  - A nested item
- Another item

+ An automatically numbered step
+ A second step
  + A nested step

/ Source: The editable Typst document.
/ Preview: Output compiled from that source.

= Mathematics <math-demo>

Inline math stays compact: $alpha + beta = gamma$, $sqrt(2)$, and $a/b$.

$ sum_(k=1)^n k = (n(n+1))/2 $
$ integral_0^1 x^2 dif x = 1/3 $
$ bold(A) = mat(1, 2; 3, 4), quad vec(x, y) $
$ f(x) = cases(x^2 & "if" x >= 0, -x & "otherwise") $

// Ampersands align columns; a backslash starts a new equation row.
$ (a+b)^2 &= a^2 + 2 a b + b^2 \
  (a-b)^2 &= a^2 - 2 a b + b^2 $

= Tables, figures, and cross-references

#figure(
  table(
    columns: (1fr, auto, auto),
    inset: 6pt,
    align: (left, right, right),
    fill: (x, y) => if y == 0 { blue.lighten(85%) },
    table.header([*Item*], [*Count*], [*Price*]),
    [Notebook], [2], [5.00],
    [Pencil], [3], [1.50],
    table.cell(colspan: 2)[*Total*], [14.50],
  ),
  caption: [A styled table with a header and a spanning cell.],
) <supplies>

See @supplies, @math-demo, and the graph in @wave.
Captions and references update when the document changes.

= Images

// SVG bytes are embedded so the example works without image files or downloads.
#let landscape = bytes(
  "<svg xmlns='http://www.w3.org/2000/svg' width='240' height='120' viewBox='0 0 240 120'>"
  + "<rect width='240' height='120' fill='#dff3ff'/>"
  + "<circle cx='190' cy='28' r='17' fill='#ffbd35'/>"
  + "<path d='M0 120 L75 25 L150 120 Z' fill='#427d65'/>"
  + "<path d='M80 120 L155 48 L240 120 Z' fill='#69a880'/>"
  + "<path d='M53 53 L75 25 L97 53 L78 47 Z' fill='white'/></svg>"
)
#figure(
  image(landscape, format: "svg", width: 65mm,
        alt: "Two green mountains and a yellow sun under a blue sky."),
  caption: [A self-contained vector image.],
) <landscape>

// This is a real 16 by 16 raster image, generated as grayscale pixel bytes.
#image(
  bytes(range(256)),
  format: (encoding: "luma8", width: 16, height: 16),
  width: 25mm,
  scaling: "pixelated",
  alt: "A grayscale raster ramp from black to white.",
)

= Graphs and drawing <graphs-demo>

// A sampled sine curve, drawn with Typst itself rather than an external package.
#figure(
  block(width: 250pt, height: 145pt)[
    #place(top + left, curve(
      stroke: 0.6pt + gray,
      curve.move((20pt, 15pt)), curve.line((20pt, 120pt)),
      curve.move((20pt, 70pt)), curve.line((235pt, 70pt)),
    ))
    #place(top + left, {
      let point(i) = (20pt + i * 3.5pt, 70pt - 45pt * calc.sin(i / 60 * 2 * calc.pi))
      curve(
        stroke: 1.8pt + blue,
        curve.move(point(0)),
        ..range(1, 61).map(i => curve.line(point(i))),
      )
    })
    #place(top + left, dx: 5pt, dy: 20pt)[#text(size: 9pt)[1]]
    #place(top + left, dx: 1pt, dy: 106pt)[#text(size: 9pt)[-1]]
    #place(top + left, dx: 20pt, dy: 75pt)[$0$]
    #place(top + left, dx: 218pt, dy: 75pt)[$2 pi$]
    #place(top + left, dx: 75pt, dy: 2pt)[$y = sin(x)$]
  ],
  kind: "graph",
  supplement: [Graph],
  caption: [A computed sine graph using curves, coordinates, and placement.],
) <wave>

#figure(
  stack(dir: ltr, spacing: 12pt,
    rect(width: 35pt, height: 35pt, fill: blue.lighten(70%), radius: 4pt),
    circle(radius: 18pt, fill: gradient.linear(red, yellow)),
    polygon((0pt, 35pt), (20pt, 0pt), (40pt, 35pt), fill: green.lighten(50%)),
  ),
  caption: [Native shapes, strokes, rounded corners, and a gradient.],
)

= Layout and reusable styling

#grid(
  columns: (1fr, 1fr), gutter: 8pt,
  rect(inset: 8pt, fill: blue.lighten(90%))[A flexible grid cell.],
  rect(inset: 8pt, fill: green.lighten(90%))[A second grid cell.],
)

#columns(2, gutter: 12pt)[
  This text starts in the left column. Columns divide a flow into parallel areas.
  #colbreak()
  An explicit column break starts this paragraph in the right column.
]

#align(right)[Right-aligned content.]
#pad(left: 12pt)[Padding moves this content inward.]
Inline #box(fill: yellow.lighten(70%), inset: 3pt)[boxed content] stays in the text flow.

#let callout(title, body) = block(
  inset: 8pt, radius: 4pt, fill: blue.lighten(90%), stroke: 0.5pt + blue,
)[
  *#title*
  #parbreak()
  #body
]
#callout("A reusable template")[Functions accept content and apply shared styling.]

= Programming, imports, and data

The quick-start formula imports `mass` from the demo's unsaved `lib.typ` module:
its value is #mass. Local functions, conditions, arrays, and dictionaries work too.

#let square(n) = n * n
#let settings = (threshold: 3, unit: "pt")
#let values = range(1, 6)
Squares: #values.map(n => str(square(n))).join(", ").

#for value in values [
  #value is #if value >= settings.threshold [at or above] else [below] the threshold. \
]

// The same loaders also accept file paths in an ordinary Typst project.
#let readings = json(bytes("{\"samples\": [2, 4, 6], \"unit\": \"ms\"}"))
JSON mean: #(readings.samples.sum() / readings.samples.len()) #readings.unit.

#let rows = csv(bytes("Name,Value\nAlpha,2\nBeta,4\nGamma,6"))
#table(columns: 2, inset: 5pt, ..rows.flatten())

= Context, counters, and state

#let step = counter("demo-step")
#step.step()
Step #context step.display("1").
#step.step()
Step #context step.display("1").

#let progress = state("demo-progress", 0)
#progress.update(7)
#context [
  Stored state: #progress.get().
  Current page: #counter(page).display().
  Heading elements in the document: #query(heading).len().
]

= Citations and bibliography

Bibliographic citations are separate from labels and cross-references.
This example cites a classic typesetting book. @knuth1984

#bibliography(
  bytes("@book{knuth1984, author={Donald E. Knuth}, title={The TeXbook}, year={1984}, publisher={Addison-Wesley}}"),
  style: "ieee",
)

= Repeated occurrences

Choose *In-place preview*. Use *Next* and *Previous* above this formula to switch
between red and blue. The loop evaluates the same source expression twice;
side-by-side and read modes show both outputs on the page.

#for color in (rgb("#ff0000"), rgb("#0000ff")) { text(fill: color)[$x$] }

= Split-output scrolling

This is *one equation* with many aligned rows, not separate equations. Short pages
force it to span several PDF pages. In *In-place preview*, scroll *inside the
rendered equation* to reach the final row, $x_32 = 32$.

// Page size is local to the rest of this sample, not a demo/editor setting.
#set page(width: 140mm, height: 80mm, margin: 12mm)
#show math.equation: set block(breakable: true)
$ x_1 &= 1 \ x_2 &= 2 \ x_3 &= 3 \ x_4 &= 4 \
  x_5 &= 5 \ x_6 &= 6 \ x_7 &= 7 \ x_8 &= 8 \
  x_9 &= 9 \ x_10 &= 10 \ x_11 &= 11 \ x_12 &= 12 \
  x_13 &= 13 \ x_14 &= 14 \ x_15 &= 15 \ x_16 &= 16 \
  x_17 &= 17 \ x_18 &= 18 \ x_19 &= 19 \ x_20 &= 20 \
  x_21 &= 21 \ x_22 &= 22 \ x_23 &= 23 \ x_24 &= 24 \
  x_25 &= 25 \ x_26 &= 26 \ x_27 &= 27 \ x_28 &= 28 \
  x_29 &= 29 \ x_30 &= 30 \ x_31 &= 31 \ x_32 &= 32 $

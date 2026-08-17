<!-- Tests: the full image grammar, including every spelling that makes the
     cleaned destination differ from the raw source text. -->

Unsized: ![alt](img.png)

All four sizes: ![a](img.png =500x) ![b](img.png =500x300) ![c](img.png =x300)
and one with a title: ![d](img.png "the title" =800x600)

No separating space, so not a size: ![e](img.png=500x)

Escaped destination: ![f](vx_images/a\_b.png)

Angle-bracketed destination: ![g](<a b.png>)

Entity-encoded destination: ![h](a&amp;b.png)

Percent-encoded destination: ![i](a%20b.png)

A title containing a close-bracket-paren: ![j](img.png "a](b")

Nested and escaped parens in the destination: ![k](a(b)c.png) ![l](a\(b.png)

All three title delimiters: ![m](img.png "dq") ![n](img.png 'sq') ![o](img.png (paren))

Reference-style: ![p][ref]

Nested and escaped brackets in the alt text: ![q\[x\]y](img.png) ![r *em* s](img.png)

An image inside another image's description: regions nest, destination spans do
not. ![outer ![inner](inner.png)](outer.png)

A multiline image whose alt text wraps:

![multi
line alt](img.png =400x)

A multiline image whose destination is on the next line:

![alt](
img.png)

Standalone on its own line:

![standalone](img.png =300x)

> ![in a block quote](img.png)

- ![in a list item](img.png)

[ref]: reference-target.png

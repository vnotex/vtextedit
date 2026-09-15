# VTextEdit: core editor component of VNote
**Test Page** with *all elements*, such as `inline code`, ~~strike~~, $a * b = c$, and so on.

## Inline destination concealment

Destinations longer than 20 graphemes keep three at each end around three middle dots.
Hover over the dots or either visible end to see the full source in a tooltip.
Move the caret into either visible end or click the dots to reveal the full source inline.
Leave the destination to conceal it again; copying always uses the original text.

[Alphabet](abcdefghijklmnopqrstuvwxyz) followed by ordinary text.
In Vi normal mode, place the caret on the opening parenthesis and type `5l` to reach `e`.

[Web link](https://example.com/abcdefghijklmnopqrstuvwxyz "The title stays visible")

Autolink: <https://example.com/abcdefghijklmnopqrstuvwxyz>

[Reference link][conceal-example]

[conceal-example]: https://example.com/abcdefghijklmnopqrstuvwxyz "Reference title"

Threshold boundary: [20 graphemes](abcdefghijklmnopqrst) stays visible;
[21 graphemes](abcdefghijklmnopqrstu) is concealed.

Markdown image: ![Bundled icon](:/demo/data/example_files/vnote.png "Local preview" =64x64) uses an offline resource.

HTML image: <img src=":/demo/data/example_files/vnote.png" alt="Bundled icon" width="64" height="64"> uses the same offline resource.

Reference-style embedded image: ![Base64 VNote icon][conceal-base64-image].

[conceal-base64-image]: data:image/png;base64,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 "Embedded VNote icon"

Inline code remains literal: `[not a link](abcdefghijklmnopqrstuvwxyz)`.

## More Markdown elements

![Image](./vnote.png)

Thanks to [VNote](https://github.com/vnotex/vnote)!

Inline image ![Image Inline](./jiafei.png) is supported.

![Network Image Need OpenSSL](https://github.com/vnotex/vnote/raw/master/pics/vnote.png)

	Tab and trailing spaces highlight  

```cpp
#include <iostream>

int main() {
    cout << "Hi, VNote" << endl;
    return 5;
}
```

```puml
class A {
    + VNote &getVNote() const
}
```

```
// Fenced code block without lang specified.
int a = 100;
```

# Title 1
## Test Outline
This is a **Title 2**.

### Another Sub
1. Test the list;
2. List item 2;
    - Nested unordered list;
    - Nested unordered list 2;

## VNote is great
Could not agree more!

1. Nested code block

    ```cpp
    #include <iostream>
    ```
2. List item 2

## Table
| Component | Description | Supported |
|-----------|-------------|:---------:|
| `VTextEdit` | Base edit widget with cursor and selection | Yes |
| `VTextEditor` | Adds syntax highlight, Vi mode and folding | Yes |
| `VMarkdownEditor` | Markdown parsing and in-place preview | Yes |

Alignment test:

| Left | Center | Right |
|:-----|:------:|------:|
| a | b | c |
| *italic* | **bold** | `code` |

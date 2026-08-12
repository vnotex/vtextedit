#ifndef PREVIEWLOGGING_H
#define PREVIEWLOGGING_H

#include <QLoggingCategory>

#include <vtextedit/preview.h>

// Diagnostics for the interactive preview pipeline.
//
// These are Qt logging categories rather than the compile-time
// VTE_DEBUG_HIGHLIGHT switch used by the parser, so they can be turned on in a
// release build of an application embedding the library without rebuilding it.
// Everything is off by default and costs one atomic load per call site.
//
// Enable through the environment, for example:
//
//   QT_LOGGING_RULES="vte.preview.*=true"                 everything
//   QT_LOGGING_RULES="vte.preview.replace=true"           only source rewrites
//   QT_LOGGING_RULES="vte.preview.*=true;vte.preview.layout=false"
//
// or from code with QLoggingCategory::setFilterRules().
//
// The categories follow the flow of one parse generation:
//
//   snapshot -> host -> table -> layout
//                 \-> replace (a widget writing back to the document)
namespace vte {
// Immutable snapshots built from the parse result: how many per type, and
// which ones were dropped for want of source text.
Q_DECLARE_LOGGING_CATEGORY(previewSnapshotLog)

// Reconciliation of snapshots against live widgets: identity matching,
// creation, in-place updates, removal, and factory registration.
Q_DECLARE_LOGGING_CATEGORY(previewHostLog)

// Space reservation and geometry: the specs and claims submitted to the
// layout, the measured sizes, and the rects handed back.
Q_DECLARE_LOGGING_CATEGORY(previewLayoutLog)

// Source replacement requests issued by a widget, with the validation verdict.
Q_DECLARE_LOGGING_CATEGORY(previewReplaceLog)

// The built-in table sheet: binding decisions, round-trip safety, size limits
// and measurement.
Q_DECLARE_LOGGING_CATEGORY(previewTableLog)

// Two more categories live in textdocumentlayout.cpp, which is compiled on its
// own by the layout tests and so cannot link this unit:
//
//   vte.layout.repair     blocks which lost their layout offset
//   vte.layout.geometry   reserved bands, block offsets and published rects
//
// A preview drawn on top of the markdown source is the two sides of
// vte.preview.layout and vte.layout.geometry disagreeing, so enable both:
//
//   QT_LOGGING_RULES="vte.preview.layout=true;vte.layout.geometry=true"

// Readable element type for the logs, so a trace never prints a bare enum
// value the reader has to look up.
inline const char *previewTypeName(PreviewElementType p_type) {
  switch (p_type) {
  case PreviewElementType::Image:
    return "Image";
  case PreviewElementType::Code:
    return "Code";
  case PreviewElementType::Math:
    return "Math";
  case PreviewElementType::Table:
    return "Table";
  default:
    return "?";
  }
}

inline const char *previewPlacementName(PreviewPlacement p_placement) {
  switch (p_placement) {
  case PreviewPlacement::BlockAfterSource:
    return "BlockAfterSource";
  case PreviewPlacement::InlineAboveLine:
    return "InlineAboveLine";
  default:
    return "?";
  }
}
} // namespace vte

#endif // PREVIEWLOGGING_H

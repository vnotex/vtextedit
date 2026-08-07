#ifndef PREVIEWFROMAST_H
#define PREVIEWFROMAST_H

#include <vtextedit/preview.h>

#include "markdownastwalker.h"

namespace vte {
// cmark_table_align ordinals: 0 none, 1 left, 2 center, 3 right.
PreviewTableAlignment toPreviewAlignment(int p_cmarkAlignment);

// The single conversion from a parsed table element to a snapshot.
//
// A parse generation and an accepted replacement's rebase must produce
// structurally identical snapshots for the same table: the prefix validation
// indexes TablePreview::rowPrefixes() assuming this layout, and the sheet
// compares its own serialized output against the bound source. Two copies of
// the conversion would let the two snapshots diverge for the same table.
QSharedPointer<const Preview> createTablePreview(quint64 p_revision, int p_startPos, int p_endPos,
                                                 const QString &p_source,
                                                 const md::TableElement &p_element);
} // namespace vte

#endif // PREVIEWFROMAST_H

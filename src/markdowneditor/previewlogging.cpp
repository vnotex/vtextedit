#include "previewlogging.h"

namespace vte {
// The default severity is deliberately QtWarningMsg: the two-argument form of
// Q_LOGGING_CATEGORY enables every message type, which would make an embedding
// application log the whole preview trace without ever asking for it. Debug
// output therefore has to be turned on explicitly, for example with
// QT_LOGGING_RULES="vte.preview.*=true".
Q_LOGGING_CATEGORY(previewSnapshotLog, "vte.preview.snapshot", QtWarningMsg)

Q_LOGGING_CATEGORY(previewHostLog, "vte.preview.host", QtWarningMsg)

Q_LOGGING_CATEGORY(previewLayoutLog, "vte.preview.layout", QtWarningMsg)

Q_LOGGING_CATEGORY(previewReplaceLog, "vte.preview.replace", QtWarningMsg)

Q_LOGGING_CATEGORY(previewTableLog, "vte.preview.table", QtWarningMsg)
} // namespace vte

#include "hlformatresolver.h"

namespace vte {
namespace md {

QVector<PreviewFormatRun> resolveFormatRuns(const QVector<HLUnit> &p_units,
                                            const QVector<QTextCharFormat> &p_styles) {
  QVector<PreviewFormatRun> runs;
  if (p_units.isEmpty() || p_styles.isEmpty()) {
    return runs;
  }

  // Drop units we cannot resolve a format for. The snapshot path is not
  // covered by the highlighter's own asserts.
  QVector<HLUnit> units;
  units.reserve(p_units.size());
  for (const auto &unit : p_units) {
    if (static_cast<int>(unit.styleIndex) < p_styles.size()) {
      units.append(unit);
    }
  }

  runs.reserve(units.size());
  for (int i = 0; i < units.size(); ++i) {
    const auto &unit = units[i];
    QTextCharFormat newFormat = p_styles[unit.styleIndex];
    for (int j = i - 1; j >= 0; --j) {
      if (units[j].start + units[j].length <= unit.start) {
        // It won't affect current unit.
        continue;
      } else {
        // Merge the format.
        QTextCharFormat tmpFormat(newFormat);
        newFormat = p_styles[units[j].styleIndex];
        // tmpFormat takes precedence.
        newFormat.merge(tmpFormat);
      }
    }

    PreviewFormatRun run;
    run.m_start = static_cast<int>(unit.start);
    run.m_length = static_cast<int>(unit.length);
    run.m_format = newFormat;
    runs.append(run);
  }

  return runs;
}

} // namespace md
} // namespace vte

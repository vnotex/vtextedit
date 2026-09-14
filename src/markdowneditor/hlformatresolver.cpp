#include "hlformatresolver.h"

#include <algorithm>

namespace vte {
namespace md {

QVector<PreviewFormatRun> resolveFormatRuns(const QVector<HLUnit> &p_units,
                                            const QVector<QTextCharFormat> &p_styles,
                                            const QVector<HLUnitStyle> &p_overlays) {
  QVector<PreviewFormatRun> runs;
  if (p_units.isEmpty() && p_overlays.isEmpty()) {
    return runs;
  }

  // Keep overlays separate from ordinary styles. Standalone table widgets may
  // supply fewer styles than the source highlighter, or none at all.
  QVector<const HLUnit *> units;
  units.reserve(p_units.size());
  for (const auto &unit : p_units) {
    if (unit.styleIndex < static_cast<unsigned int>(p_styles.size())) {
      units.append(&unit);
    }
  }

  // Preserve the source highlighter's sequential ordinary-style merging.
  runs.reserve(units.size());
  for (int i = 0; i < units.size(); ++i) {
    const auto &unit = *units[i];
    QTextCharFormat newFormat = p_styles[unit.styleIndex];
    for (int j = i - 1; j >= 0; --j) {
      if (units[j]->start + units[j]->length <= unit.start) {
        // It won't affect current unit.
        continue;
      } else {
        // Merge the format.
        QTextCharFormat tmpFormat(newFormat);
        newFormat = p_styles[units[j]->styleIndex];
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

  if (p_overlays.isEmpty()) {
    return runs;
  }

  // Each boundary can change the final ordinary format or the winning color.
  // Splitting at both ends also handles crossing (not just nested) overlaps.
  QVector<int> boundaries;
  boundaries.reserve(2 * (runs.size() + p_overlays.size()));
  for (const auto &run : runs) {
    boundaries.append(run.m_start);
    boundaries.append(run.m_start + run.m_length);
  }
  for (const auto &overlay : p_overlays) {
    if (overlay.length > 0) {
      boundaries.append(static_cast<int>(overlay.start));
      boundaries.append(static_cast<int>(overlay.start + overlay.length));
    }
  }
  if (boundaries.size() < 2) {
    return runs;
  }
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

  const int ordinaryRunCount = runs.size();
  runs.reserve(ordinaryRunCount + boundaries.size() - 1);
  for (int i = 0; i + 1 < boundaries.size(); ++i) {
    const int start = boundaries[i];
    const HLUnitStyle *overlay = nullptr;
    // The input orders outer overlays before inner ones. Last covering wins.
    for (int j = p_overlays.size() - 1; j >= 0; --j) {
      const auto *candidate = &p_overlays[j];
      if (candidate->start <= static_cast<unsigned long>(start) &&
          candidate->start + candidate->length > static_cast<unsigned long>(start)) {
        overlay = candidate;
        break;
      }
    }
    if (!overlay) {
      continue;
    }

    PreviewFormatRun run;
    run.m_start = start;
    run.m_length = boundaries[i + 1] - start;
    // setFormat() replaces a range: use the last ordinary run covering this
    // piece, not a merge of overlapping runs or the format at the overlay's start.
    for (int j = ordinaryRunCount - 1; j >= 0; --j) {
      const auto &ordinary = runs[j];
      if (ordinary.m_start <= start && ordinary.m_start + ordinary.m_length > start) {
        run.m_format = ordinary.m_format;
        break;
      }
    }
    run.m_format.merge(overlay->format);
    if (runs.size() > ordinaryRunCount && runs.last().m_start + runs.last().m_length == start &&
        runs.last().m_format == run.m_format) {
      runs.last().m_length += run.m_length;
    } else {
      runs.append(run);
    }
  }

  return runs;
}

} // namespace md
} // namespace vte

#ifndef KATEVII_MOVINGCURSOR_H
#define KATEVII_MOVINGCURSOR_H

#include <katevi/katevi_export.h>

#include <QString>

namespace KateViI {
class KATEVI_EXPORT MovingCursor {
public:
  /**
   * Insert behavior of this cursor, should it stay if text is insert at its
   * position or should it move.
   */
  enum InsertBehavior {
    StayOnInsert = 0x0, ///< stay on insert
    MoveOnInsert = 0x1  ///< move on insert
  };

  /**
   * Wrap behavior for end of line treatement used in move().
   */
  enum WrapBehavior {
    Wrap = 0x0,  ///< wrap at end of line
    NoWrap = 0x1 ///< do not wrap at end of line
  };
};
} // namespace KateViI

#endif // KATEVII_MOVINGCURSOR_H

#ifndef KATEVII_MARKINTERFACE_H
#define KATEVII_MARKINTERFACE_H

#include <katevi/katevi_export.h>

#include <functional>

#include <QString>

namespace KateViI {
class KateViEditorInterface;

/**
 * \brief Mark class containing line and mark types.
 *
 * \section mark_intro Introduction
 *
 * The class Mark represents a mark in a Document. It contains the \e line
 * and \e type. A line can have multiple marks, like a \e bookmark and a
 * \e breakpoint, i.e. the \e type contains all marks combined with a logical
 * \e OR (<tt>|</tt>). There are several predefined mark types, look into the
 * MarkInterface for further details.
 *
 * \see KTextEditor::MarkInterface, KTextEditor::Document
 */
class KATEVI_EXPORT Mark {
public:
  /** The line that contains the mark. */
  int line;

  /** The mark types in the line, combined with logical OR. */
  uint type;
};

class KATEVI_EXPORT MarkInterface {
public:
  enum MarkChangeAction {
    MarkAdded = 0,  /**< action: a mark was added.  */
    MarkRemoved = 1 /**< action: a mark was removed. */
  };

  /**
   * Predefined mark types.
   *
   * To add a new standard mark type, edit this interface and document
   * the type.
   */
  enum MarkTypes {
    /** Bookmark */
    markType01 = 0x1,
    /** Breakpoint active */
    markType02 = 0x2,
    /** Breakpoint reached */
    markType03 = 0x4,
    /** Breakpoint disabled */
    markType04 = 0x8,
    /** Execution mark */
    markType05 = 0x10,
    /** Warning */
    markType06 = 0x20,
    /** Error */
    markType07 = 0x40,

    markType08 = 0x80,
    markType09 = 0x100,
    markType10 = 0x200,
    markType11 = 0x400,
    markType12 = 0x800,
    markType13 = 0x1000,
    markType14 = 0x2000,
    markType15 = 0x4000,
    markType16 = 0x8000,
    markType17 = 0x10000,
    markType18 = 0x20000,
    markType19 = 0x40000,
    markType20 = 0x80000,
    markType21 = 0x100000,
    markType22 = 0x200000,
    markType23 = 0x400000,
    markType24 = 0x800000,
    markType25 = 0x1000000,
    markType26 = 0x2000000,
    markType27 = 0x4000000,
    markType28 = 0x8000000,
    markType29 = 0x10000000,
    markType30 = 0x20000000,
    markType31 = 0x40000000,
    markType32 = 0x80000000,
    /* reserved marks */
    Bookmark = markType01,
    BreakpointActive = markType02,
    BreakpointReached = markType03,
    BreakpointDisabled = markType04,
    Execution = markType05,
    Warning = markType06,
    Error = markType07,
    SearchMatch = markType32,
  };

  /**
   * Remove the mark mask of type \p markType from \p line.
   * \param line line to remove the mark
   * \param markType mark type to be removed
   * \see clearMark()
   */
  virtual void removeMark(int line, uint markType) = 0;

  // Connect @p_slot to markChanged signal.
  virtual void connectMarkChanged(
      std::function<void(KateViI::KateViEditorInterface *p_editorInterface, KateViI::Mark p_mark,
                         KateViI::MarkInterface::MarkChangeAction p_action)>
          p_slot) = 0;
};
} // namespace KateViI

#endif // KATEVII_MARKINTERFACE_H

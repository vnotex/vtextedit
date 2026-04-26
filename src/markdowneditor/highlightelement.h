#ifndef HIGHLIGHTELEMENT_H
#define HIGHLIGHTELEMENT_H

struct HighlightElement {
  int type;               // MarkdownSyntaxStyle ordinal value
  unsigned long pos;      // Start offset (QChar/document-absolute)
  unsigned long end;      // End offset (QChar/document-absolute)
  HighlightElement *next; // Next element in linked list
};

// Same value as pmh_NUM_LANG_TYPES — number of highlight style types
#define NUM_HIGHLIGHT_STYLES 33

// Free all elements in the p_result array and the array itself.
void freeHighlightElements(HighlightElement **p_result, int p_numTypes);

#endif

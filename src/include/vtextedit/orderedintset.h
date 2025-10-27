#ifndef ORDEREDINTSET_H
#define ORDEREDINTSET_H

#include <QMap>

namespace vte {
struct QMapDummyValue {};

typedef QMap<int, QMapDummyValue> OrderedIntSet;
} // namespace vte

#endif // ORDEREDINTSET_H

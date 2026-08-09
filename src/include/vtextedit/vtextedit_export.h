#ifndef VTEXTEDIT_VTEXTEDIT_EXPORT_H
#define VTEXTEDIT_VTEXTEDIT_EXPORT_H

#ifdef VTEXTEDIT_STATIC_DEFINE
#define VTEXTEDIT_EXPORT
#define VTEXTEDIT_NO_EXPORT
#else
#ifndef VTEXTEDIT_EXPORT
#ifdef VTEXTEDIT_LIB
/* We are building this library */
#define VTEXTEDIT_EXPORT Q_DECL_EXPORT
#else
/* We are using this library */
#define VTEXTEDIT_EXPORT Q_DECL_IMPORT
#endif
#endif

#ifndef VTEXTEDIT_NO_EXPORT
#define VTEXTEDIT_NO_EXPORT
#endif
#endif

#endif /* VTEXTEDIT_EXPORT_H */

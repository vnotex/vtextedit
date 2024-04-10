#ifndef KATEVI_EXPORT_H
#define KATEVI_EXPORT_H

#ifdef KATEVI_STATIC_DEFINE
#  define KATEVI_EXPORT
#  define KATEVI_NO_EXPORT
#else
#  ifndef KATEVI_EXPORT
#    ifdef KATEVI_LIBRARY
        /* We are building this library */
#      define KATEVI_EXPORT Q_DECL_EXPORT
#    else
        /* We are using this library */
#      define KATEVI_EXPORT Q_DECL_IMPORT
#    endif
#  endif

#  ifndef KATEVI_NO_EXPORT
#    define KATEVI_NO_EXPORT 
#  endif
#endif

#ifndef KATEVI_DEPRECATED
#  define KATEVI_DEPRECATED Q_DECL_DEPRECATED
#endif

#ifndef KATEVI_DEPRECATED_EXPORT
#  define KATEVI_DEPRECATED_EXPORT KATEVI_EXPORT KATEVI_DEPRECATED
#endif

#ifndef KATEVI_DEPRECATED_NO_EXPORT
#  define KATEVI_DEPRECATED_NO_EXPORT KATEVI_NO_EXPORT KATEVI_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef KATEVI_NO_DEPRECATED
#    define KATEVI_NO_DEPRECATED
#  endif
#endif

#endif /* KATEVI_EXPORT_H */

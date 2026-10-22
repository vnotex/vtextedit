#ifndef VTEXTEDIT_TYPST_H
#define VTEXTEDIT_TYPST_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define VTE_TYPST_API __declspec(dllimport)
#define VTE_TYPST_CALL __cdecl
#else
#define VTE_TYPST_API
#define VTE_TYPST_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VteTypstSession VteTypstSession;
typedef struct VteTypstReply VteTypstReply;

enum {
  VTE_TYPST_OK = 0,
  VTE_TYPST_INVALID_ARGUMENT = 1,
  VTE_TYPST_ERROR = 2,
  VTE_TYPST_PANIC = 3,
  VTE_TYPST_POISONED = 4,
  VTE_TYPST_CALLBACK_FAILED = 5
};

/*
 * ABI 1 uses UTF-8 JSON v1 metadata, without framing or binary request data.
 * Request/header and binary reply ceilings are 16 MiB and 128 MiB respectively;
 * configured session resource limits can be lower. Input is borrowed only for
 * the synchronous call. Null or zero-length requests are invalid. All nonnull
 * pointers must be valid for their stated lengths and handle lifetimes.
 *
 * Session creation, execution and destruction must occur on the same thread.
 * Calls must be serialized, and callbacks must not reenter/destroy the session.
 * A session remains busy until execute returns, even after caller cancellation.
 * A caught unwinding panic returns PANIC and poisons the session: subsequent
 * execute calls return POISONED. Destroy it only after execute has returned.
 * Abort, stack overflow, native faults and allocation failure are not contained.
 *
 * Ordinary session errors are delivered as op:error with their existing request
 * identities and return OK if delivery succeeded. Invalid ABI arguments, JSON,
 * UTF-8 or request identities return INVALID_ARGUMENT without delivery. Other
 * non-OK statuses fail the call even if earlier events were delivered. Panic
 * diagnostics are delivered when possible; callers must also handle the status.
 */
VTE_TYPST_API uint32_t VTE_TYPST_CALL vte_typst_abi_version(void);

/* Sets *out_session to null on failure. The output pointer must be nonnull. */
VTE_TYPST_API int32_t VTE_TYPST_CALL vte_typst_session_create_v1(VteTypstSession **out_session);

/* Null is a no-op. A valid handle is consumed even if destruction returns PANIC.
 * INVALID_ARGUMENT (wrong thread/reentry) does not consume the handle. */
VTE_TYPST_API int32_t VTE_TYPST_CALL vte_typst_session_destroy_v1(VteTypstSession *session);

/*
 * Ownership transfers to the callback on entry, UNCONDITIONALLY, including when
 * it returns nonzero. Rust never reclaims a delivered reply. Release it exactly
 * once with vte_typst_reply_release, or transfer it to another owning object.
 * Returning nonzero stops all further delivery for this request and produces
 * CALLBACK_FAILED. A callback must never unwind/throw across this C boundary;
 * C++ implementations must be noexcept trampolines that catch all exceptions.
 */
typedef int32_t(VTE_TYPST_CALL *VteTypstReplyCallback)(void *context, VteTypstReply *reply);

VTE_TYPST_API int32_t VTE_TYPST_CALL vte_typst_session_execute_v1(VteTypstSession *session,
                                                                  const uint8_t *request,
                                                                  size_t length,
                                                                  VteTypstReplyCallback callback,
                                                                  void *context);

/* Pure parse-only operation, independent of sessions, fonts, filesystem access,
 * evaluation and downloads. Request: {"v":1,"op":"scanImageReferences",
 * "sources":[...]} with exactly 1..3 strings. Malformed requests produce no
 * reply. Resource failures may emit op:error and return ERROR. */
VTE_TYPST_API int32_t VTE_TYPST_CALL vte_typst_scan_image_references_v1(
    const uint8_t *request, size_t length, VteTypstReplyCallback callback, void *context);

/* Immutable borrowed spans, not NUL-terminated. Remain valid until release,
 * independent of session lifetime. Replies may be read/released on any thread,
 * but release must not overlap any access. Null reply or null length returns
 * null; a nonnull length is set to zero on failure. Empty payload returns null.
 * No handle or span may be freed with C/C++ free/delete. */
VTE_TYPST_API const uint8_t *VTE_TYPST_CALL vte_typst_reply_header(const VteTypstReply *reply,
                                                                   size_t *length);
VTE_TYPST_API const uint8_t *VTE_TYPST_CALL vte_typst_reply_payload(const VteTypstReply *reply,
                                                                    size_t *length);

/* Null is a no-op. A nonnull handle is consumed even if release returns PANIC. */
VTE_TYPST_API int32_t VTE_TYPST_CALL vte_typst_reply_release(VteTypstReply *reply);

#ifdef __cplusplus
}
#endif

#endif /* VTEXTEDIT_TYPST_H */

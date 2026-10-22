#ifndef VTE_TYPSTRUNTIME_P_H
#define VTE_TYPSTRUNTIME_P_H

#include <vtextedit/typstruntime.h>
#include <vtextedit_typst.h>

#include <QByteArray>
#include <QObject>

#include <array>
#include <memory>
#include <vector>

class QThread;

namespace vte {

// The handle owns immutable Rust bytes independently of the session. Copies share
// ownership, including queued deliveries discarded by Qt during receiver teardown.
class TypstRuntimeReply {
public:
  TypstRuntimeReply() = default;
  explicit TypstRuntimeReply(VteTypstReply *p_reply)
      : m_reply(p_reply, [](VteTypstReply *p_owned) { vte_typst_reply_release(p_owned); }) {}
  explicit operator bool() const { return bool(m_reply); }
  const uint8_t *header(size_t *p_size) const {
    return vte_typst_reply_header(m_reply.get(), p_size);
  }
  const uint8_t *payload(size_t *p_size) const {
    return vte_typst_reply_payload(m_reply.get(), p_size);
  }

private:
  std::shared_ptr<VteTypstReply> m_reply;
};

struct TypstRuntimeControl;
struct TypstRuntimeSessionState;
struct TypstRuntimeCall;

// GUI-owned endpoint only. Native handles and active invocations are owned by the
// runtime lanes and may safely outlive this endpoint.
class TypstRuntimeSession : public QObject {
  Q_OBJECT
public:
  ~TypstRuntimeSession() Q_DECL_OVERRIDE;
  bool execute(QByteArray p_request);
  bool cancelQueued();
  void suppressReplies();
  bool isRunning() const;
  bool isAvailable() const;

signals:
  void replyReady(const vte::TypstRuntimeReply &p_reply);
  // Emitted only after execute has really returned (or queued work was cancelled).
  void finished(int p_status);
  void stopped();

private:
  friend class TypstRuntimePrivate;
  TypstRuntimeSession(std::shared_ptr<TypstRuntimeSessionState> p_state, QObject *p_parent);
  std::shared_ptr<TypstRuntimeSessionState> m_state;
  std::shared_ptr<TypstRuntimeCall> m_call;
};

class TypstRuntimePrivate {
public:
  static TypstRuntimePrivate *get(TypstRuntime *p_runtime) { return p_runtime->d.data(); }
  explicit TypstRuntimePrivate(TypstRuntime *p_owner);
  ~TypstRuntimePrivate();
  bool isAvailable() const;
  void shutdown();
  TypstRuntimeSession *createSession(QObject *p_parent);
  // 0/1: serial compile/render lanes; 2: independent parse-only scanner.
  QObject *worker(int p_lane) const { return m_workers.at(p_lane); }
  TypstRuntimeReply scan(QByteArray p_request, QString *p_error);
  static constexpr int Cancelled = -1;

private:
  std::shared_ptr<TypstRuntimeControl> m_control;
  std::array<QThread *, 3> m_threads = {};
  std::array<QObject *, 3> m_workers = {};
  std::vector<std::weak_ptr<TypstRuntimeSessionState>> m_sessions;
  unsigned m_nextLane = 0;
};

} // namespace vte

Q_DECLARE_METATYPE(vte::TypstRuntimeReply)

#endif // VTE_TYPSTRUNTIME_P_H

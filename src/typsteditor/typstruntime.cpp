#include <vtextedit/typstruntime.h>

#include "typstruntime_p.h"

#include <QMetaObject>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

using namespace vte;

struct vte::TypstRuntimeControl {
  std::atomic<bool> m_available{true};
  std::atomic<bool> m_scannerBusy{false};
  std::mutex m_admission;
  QObject *m_delivery = nullptr;
};

struct vte::TypstRuntimeCall {
  enum Phase { Queued, Running, Returned, Cancelled };
  std::atomic<Phase> m_phase{Queued};
  std::atomic<bool> m_suppressed{false};
  QByteArray m_request;
};

struct vte::TypstRuntimeSessionState {
  std::shared_ptr<TypstRuntimeControl> m_control;
  QObject *m_worker = nullptr;
  // The runtime dispatcher resolves this pointer under the lock, then posts a
  // context-bound event to the endpoint's thread. Worker tasks never read it.
  std::mutex m_receiverMutex;
  TypstRuntimeSession *m_receiver = nullptr;
  std::atomic<bool> m_retired{false};
  // Accessed only on the assigned lane, including destruction.
  VteTypstSession *m_native = nullptr;
};

namespace {
void destroySession(const std::shared_ptr<TypstRuntimeSessionState> &p_state) {
  if (p_state->m_native) {
    vte_typst_session_destroy_v1(p_state->m_native);
    p_state->m_native = nullptr;
  }
}

template <typename Function>
bool postToSession(const std::shared_ptr<TypstRuntimeSessionState> &p_state, Function p_function) {
  return QMetaObject::invokeMethod(
      p_state->m_control->m_delivery,
      [state = p_state, function = std::move(p_function)]() mutable {
        std::lock_guard<std::mutex> lock(state->m_receiverMutex);
        if (auto *receiver = state->m_receiver) {
          QMetaObject::invokeMethod(
              receiver, [receiver, function = std::move(function)]() { function(receiver); },
              Qt::QueuedConnection);
        }
      },
      Qt::QueuedConnection);
}

struct CallbackContext {
  std::shared_ptr<TypstRuntimeSessionState> m_state;
  std::shared_ptr<TypstRuntimeCall> m_call;
};

int32_t VTE_TYPST_CALL receiveReply(void *p_context, VteTypstReply *p_reply) noexcept {
  try {
    // shared_ptr's constructor invokes its deleter even if allocation fails.
    TypstRuntimeReply reply(p_reply);
    const auto &context = *static_cast<CallbackContext *>(p_context);
    const auto state = context.m_state;
    const auto call = context.m_call;
    if (state->m_retired.load() || call->m_suppressed.load() ||
        !state->m_control->m_available.load()) {
      return VTE_TYPST_OK;
    }
    // The dispatcher belongs to the runtime, which drains all lanes before its
    // destruction. Workers never inspect a GUI receiver, QPointer, or bridge.
    const bool queued = postToSession(
        state, [state, call, reply = std::move(reply)](TypstRuntimeSession *p_receiver) {
          if (!state->m_retired.load() && !call->m_suppressed.load() &&
              state->m_control->m_available.load()) {
            emit p_receiver->replyReady(reply);
          }
        });
    return queued ? VTE_TYPST_OK : VTE_TYPST_CALLBACK_FAILED;
  } catch (...) {
    // Ownership has already been acquired, and no C++ exception crosses Rust.
    return VTE_TYPST_CALLBACK_FAILED;
  }
}

struct ScanState {
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_done = false;
  bool m_abandoned = false;
  int m_status = VTE_TYPST_ERROR;
  TypstRuntimeReply m_reply;
};

int32_t VTE_TYPST_CALL receiveScan(void *p_context, VteTypstReply *p_reply) noexcept {
  try {
    TypstRuntimeReply reply(p_reply);
    auto &state = *static_cast<ScanState *>(p_context);
    std::lock_guard<std::mutex> lock(state.m_mutex);
    if (state.m_abandoned)
      return VTE_TYPST_OK;
    if (state.m_reply)
      return VTE_TYPST_CALLBACK_FAILED;
    state.m_reply = std::move(reply);
    return VTE_TYPST_OK;
  } catch (...) {
    return VTE_TYPST_CALLBACK_FAILED;
  }
}
} // namespace

TypstRuntime::TypstRuntime(QObject *p_parent)
    : QObject(p_parent), d(new TypstRuntimePrivate(this)) {}

TypstRuntime::~TypstRuntime() = default;

bool TypstRuntime::isAvailable() const { return d->isAvailable(); }

void TypstRuntime::shutdown() { d->shutdown(); }

TypstRuntimePrivate::TypstRuntimePrivate(TypstRuntime *p_owner)
    : m_control(std::make_shared<TypstRuntimeControl>()) {
  m_control->m_delivery = new QObject(p_owner);
  m_control->m_delivery->setObjectName(QStringLiteral("typst-delivery"));
  for (size_t i = 0; i < m_threads.size(); ++i) {
    auto *thread = new QThread(p_owner);
    thread->setObjectName(i == 2 ? QStringLiteral("typst-scanner")
                                 : QStringLiteral("typst-compile-%1").arg(int(i)));
    auto *worker = new QObject;
    worker->setObjectName(thread->objectName() + QStringLiteral("-worker"));
    worker->moveToThread(thread);
    thread->setProperty("worker", QVariant::fromValue(worker));
    QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    m_threads[i] = thread;
    m_workers[i] = worker;
    thread->start();
  }
}

TypstRuntimePrivate::~TypstRuntimePrivate() { shutdown(); }

bool TypstRuntimePrivate::isAvailable() const { return m_control->m_available.load(); }

void TypstRuntimePrivate::shutdown() {
  std::unique_lock<std::mutex> admission(m_control->m_admission);
  if (!m_control->m_available.exchange(false))
    return;
  // Retain native state before releasing admission: a consumer on another
  // thread may be destroyed during the drain and cannot enqueue cleanup once
  // admission is closed. Weak bookkeeping alone would then lose its native handle.
  std::vector<std::shared_ptr<TypstRuntimeSessionState>> sessions;
  sessions.reserve(m_sessions.size());
  for (const auto &weak : m_sessions) {
    if (auto state = weak.lock()) {
      state->m_retired.store(true);
      sessions.push_back(std::move(state));
    }
  }
  admission.unlock();
  // Drain relies only on lane event loops and native return, never GUI acknowledgements.
  Q_ASSERT(QThread::currentThread() == m_control->m_delivery->thread());
  for (size_t i = 0; i < m_threads.size(); ++i) {
    QMetaObject::invokeMethod(
        m_workers[i],
        [this, i, &sessions]() {
          for (const auto &state : sessions) {
            if (state->m_worker == m_workers[i])
              destroySession(state);
          }
        },
        Qt::BlockingQueuedConnection);
    m_threads[i]->quit();
    m_threads[i]->wait();
    m_threads[i]->setProperty("worker", QVariant());
    m_workers[i] = nullptr;
  }
  // Consumers may deliberately keep a stopped runtime. Complete their admitted
  // work with failure, but never deliver successful output after shutdown.
  for (const auto &state : sessions) {
    postToSession(state, [](TypstRuntimeSession *p_receiver) { emit p_receiver->stopped(); });
  }
}

TypstRuntimeSession *TypstRuntimePrivate::createSession(QObject *p_parent) {
  std::lock_guard<std::mutex> admission(m_control->m_admission);
  if (!isAvailable())
    return nullptr;
  m_sessions.erase(std::remove_if(m_sessions.begin(), m_sessions.end(),
                                  [](const auto &p_state) { return p_state.expired(); }),
                   m_sessions.end());
  auto state = std::make_shared<TypstRuntimeSessionState>();
  state->m_control = m_control;
  state->m_worker = m_workers[m_nextLane++ % 2];
  m_sessions.push_back(state);
  return new TypstRuntimeSession(std::move(state), p_parent);
}

TypstRuntimeSession::TypstRuntimeSession(std::shared_ptr<TypstRuntimeSessionState> p_state,
                                         QObject *p_parent)
    : QObject(p_parent), m_state(std::move(p_state)) {
  m_state->m_receiver = this;
}

TypstRuntimeSession::~TypstRuntimeSession() {
  {
    std::lock_guard<std::mutex> lock(m_state->m_receiverMutex);
    m_state->m_receiver = nullptr;
  }
  m_state->m_retired.store(true);
  suppressReplies();
  cancelQueued();
  std::lock_guard<std::mutex> admission(m_state->m_control->m_admission);
  if (m_state->m_control->m_available.load()) {
    QMetaObject::invokeMethod(
        m_state->m_worker, [state = m_state]() { destroySession(state); }, Qt::QueuedConnection);
  }
}

bool TypstRuntimeSession::isAvailable() const {
  return m_state->m_control->m_available.load() && !m_state->m_retired.load();
}

bool TypstRuntimeSession::isRunning() const {
  return m_call && m_call->m_phase.load() == TypstRuntimeCall::Running;
}

bool TypstRuntimeSession::cancelQueued() {
  if (!m_call)
    return false;
  auto expected = TypstRuntimeCall::Queued;
  return m_call->m_phase.compare_exchange_strong(expected, TypstRuntimeCall::Cancelled);
}

void TypstRuntimeSession::suppressReplies() {
  if (m_call)
    m_call->m_suppressed.store(true);
}

bool TypstRuntimeSession::execute(QByteArray p_request) {
  std::lock_guard<std::mutex> admission(m_state->m_control->m_admission);
  if (!isAvailable() || (m_call && m_call->m_phase.load() != TypstRuntimeCall::Returned))
    return false;
  auto call = std::make_shared<TypstRuntimeCall>();
  call->m_request = std::move(p_request);
  const auto state = m_state;
  m_call = call;
  const bool queued = QMetaObject::invokeMethod(
      state->m_worker,
      [state, call]() {
        int status = TypstRuntimePrivate::Cancelled;
        auto expected = TypstRuntimeCall::Queued;
        if (call->m_phase.compare_exchange_strong(expected, TypstRuntimeCall::Running) &&
            !state->m_retired.load() && state->m_control->m_available.load()) {
          status = VTE_TYPST_ERROR;
          if (vte_typst_abi_version() == 1) {
            status = state->m_native ? VTE_TYPST_OK : vte_typst_session_create_v1(&state->m_native);
            if (status == VTE_TYPST_OK) {
              CallbackContext context{state, call};
              status = vte_typst_session_execute_v1(
                  state->m_native, reinterpret_cast<const uint8_t *>(call->m_request.constData()),
                  size_t(call->m_request.size()), receiveReply, &context);
            }
          }
        }
        call->m_request.clear();
        call->m_phase.store(TypstRuntimeCall::Returned);
        if (state->m_retired.load() || !state->m_control->m_available.load())
          destroySession(state);
        postToSession(state, [call, status](TypstRuntimeSession *p_receiver) {
          emit p_receiver->finished(status);
        });
      },
      Qt::QueuedConnection);
  if (!queued) {
    m_call.reset();
    return false;
  }
  return true;
}

TypstRuntimeReply TypstRuntimePrivate::scan(QByteArray p_request, QString *p_error) {
  const auto fail = [p_error](const QString &p_message) {
    if (p_error)
      *p_error = p_message;
    return TypstRuntimeReply();
  };
  std::unique_lock<std::mutex> admission(m_control->m_admission);
  if (!isAvailable())
    return fail(TypstRuntime::tr("Typst runtime is unavailable."));
  bool expected = false;
  if (!m_control->m_scannerBusy.compare_exchange_strong(expected, true))
    return fail(TypstRuntime::tr("The previous image reference scan is still running."));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  const auto state = std::make_shared<ScanState>();
  const auto control = m_control;
  const bool queued = QMetaObject::invokeMethod(
      m_workers[2],
      [state, control, request = std::move(p_request)]() {
        bool abandoned;
        {
          std::lock_guard<std::mutex> lock(state->m_mutex);
          abandoned = state->m_abandoned;
        }
        int status = VTE_TYPST_ERROR;
        if (!abandoned && control->m_available.load() && vte_typst_abi_version() == 1) {
          status = vte_typst_scan_image_references_v1(
              reinterpret_cast<const uint8_t *>(request.constData()), size_t(request.size()),
              receiveScan, state.get());
        }
        {
          std::lock_guard<std::mutex> lock(state->m_mutex);
          state->m_status = status;
          state->m_done = true;
        }
        control->m_scannerBusy.store(false);
        state->m_changed.notify_all();
      },
      Qt::QueuedConnection);
  admission.unlock();
  if (!queued) {
    m_control->m_scannerBusy.store(false);
    return fail(TypstRuntime::tr("Unable to queue image reference scan."));
  }
  std::unique_lock<std::mutex> lock(state->m_mutex);
  if (!state->m_changed.wait_until(lock, deadline, [&state]() { return state->m_done; })) {
    state->m_abandoned = true;
    state->m_reply = {};
    return fail(TypstRuntime::tr("Image reference scan timed out."));
  }
  if (!isAvailable() || state->m_status != VTE_TYPST_OK || !state->m_reply)
    return fail(
        TypstRuntime::tr("Image reference scan failed (native status %1).").arg(state->m_status));
  return std::move(state->m_reply);
}

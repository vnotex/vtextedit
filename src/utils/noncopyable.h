#ifndef VTE_NONCOPYABLE_H
#define VTE_NONCOPYABLE_H

namespace vte {
class Noncopyable {
protected:
  Noncopyable() = default;

  virtual ~Noncopyable() = default;

  Noncopyable(const Noncopyable &) = delete;
  Noncopyable &operator=(const Noncopyable &) = delete;
};
} // namespace vte

#endif // VTE_NONCOPYABLE_H

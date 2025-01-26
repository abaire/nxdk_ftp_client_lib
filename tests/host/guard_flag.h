#ifndef GUARD_FLAG_H
#define GUARD_FLAG_H

#include <condition_variable>
#include <mutex>

//! Encapsulates a boolean that may be set from one thread and awaited on
//! another.
struct GuardFlag {
  std::condition_variable cv;
  std::mutex m;
  bool flag_set = false;
  bool disabled = false;

  void Await() {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [this] { return flag_set; });
  }

  //! Awaits the flag being set to true or a timeout. Returns false on timeout.
  bool Await(std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(m);
    return cv.wait_for(lock, timeout, [this] { return flag_set; });
  }

  //! Sets the flag to true.
  void Set() {
    {
      std::lock_guard lock(m);
      flag_set = true;
    }
    cv.notify_one();
  }

  //! Clears the flag without notifying the condition variable.
  [[nodiscard]] bool Clear() {
    std::lock_guard lock(m);
    if (disabled) {
      return false;
    }
    flag_set = false;

    return true;
  }

  // Clears the flag and awaits it being set again.
  void ClearAndAwait() {
    if (!Clear()) {
      return;
    }
    Await();
  }

  // Sets the flag and prevents it from being reset.
  void SetAndClamp() {
    {
      std::lock_guard lock(m);
      flag_set = true;
      disabled = true;
    }
    cv.notify_one();
  }
};

#endif  // GUARD_FLAG_H

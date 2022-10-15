#pragma once

#include <Windows.h>

#include <atomic>

struct ManagedHandle {
  HANDLE h = INVALID_HANDLE_VALUE;

  ManagedHandle() {}

  ManagedHandle(ManagedHandle &) = delete;

  ManagedHandle(ManagedHandle &&other) {
    h = other.h;
    other.h = INVALID_HANDLE_VALUE;
  }

  ~ManagedHandle() {
    if (h != INVALID_HANDLE_VALUE) {
      assert(CloseHandle(h));
    }
  }

  ManagedHandle &operator=(ManagedHandle &) = delete;

  ManagedHandle &operator=(ManagedHandle &&other) {
    h = other.h;
    other.h = INVALID_HANDLE_VALUE;
    return *this;
  }

  operator bool() {
    return h != INVALID_HANDLE_VALUE;
  }

  operator HANDLE() {
    return h;
  }

  operator std::atomic<HANDLE>() {
    auto curr = h;
    h = INVALID_HANDLE_VALUE;
    return curr;
  }
};

void close_handle(std::atomic<HANDLE> &h) {
  auto to_close = h.exchange(INVALID_HANDLE_VALUE);
  if (to_close != INVALID_HANDLE_VALUE) {
    if (!CancelIoEx(to_close, nullptr)) {
      assert(GetLastError() == ERROR_NOT_FOUND);
    }
    assert(CloseHandle(to_close));
  }
}

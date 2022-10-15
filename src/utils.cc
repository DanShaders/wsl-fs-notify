#include "utils.h"

#ifdef WIN32
#  include <Windows.h>
#else
#  include <unistd.h>
#endif

#include "config.h"

bool write_exactly(fd_t fd, std::string_view s) {
#ifdef WIN32
  DWORD written;

  while (s.size()) {
    if (!WriteFile(fd, s.data(), (DWORD) std::min<size_t>(s.size(), MAXDWORD), &written, nullptr)) {
      return false;
    }
    s = s.substr(written);
  }
  return true;
#else
  while (s.size()) {
    ssize_t written = write(fd, s.data(), s.size());
    if (written <= 0) {
      return false;
    }
    s = s.substr(written);
  }
  return true;
#endif
}

bool HelloRequest::is_eq(const char *hello_str) {
  for (int i = 0; i < HELLO_LENGTH; ++i) {
    if (hello_str[i] != data[i]) {
      return false;
    }
  }
  return true;
}

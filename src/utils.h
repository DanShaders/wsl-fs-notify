#pragma once

#include <string_view>

#ifdef WIN32
using fd_t = void *;  // HANDLE
inline const fd_t INVALID_FD = nullptr;
#else
using fd_t = int;
inline const fd_t INVALID_FD = -1;
#endif

bool write_exactly(fd_t fd, std::string_view s);

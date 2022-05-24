#pragma once

#include <cstdint>

using import_callback_t = void (*)(char *, char *, uint64_t *);

void for_each_import(wchar_t *filename, uint64_t image_base, import_callback_t callback);

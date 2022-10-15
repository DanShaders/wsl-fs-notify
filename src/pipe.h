#pragma once

#include <Windows.h>

BOOL WINAPI MyCreatePipeEx(OUT LPHANDLE lpReadPipe, OUT LPHANDLE lpWritePipe,
                           IN LPSECURITY_ATTRIBUTES lpPipeAttributes, IN DWORD nSize,
                           DWORD dwReadMode, DWORD dwWriteMode);
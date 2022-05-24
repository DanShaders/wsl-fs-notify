#pragma once

#include <cstdint>

const wchar_t WSL_COMMAND[] = L"wsl-fs-notify";

const char CLIENT_HELLO[] = "WFN\n\0";
const char SERVER_HELLO[] = "WFN\n\1";
const int HELLO_LENGTH = 5;

#ifndef WIN32
const uint32_t FILE_ACTION_ADDED = 0x00000001;
const uint32_t FILE_ACTION_REMOVED = 0x00000002;
const uint32_t FILE_ACTION_MODIFIED = 0x00000003;
const uint32_t FILE_ACTION_RENAMED_OLD_NAME = 0x00000004;
const uint32_t FILE_ACTION_RENAMED_NEW_NAME = 0x00000005;
#endif

const uint32_t ERROR_WSL_START_FAILED = (1 << 29) | 1;
const uint32_t ERROR_HANDSHAKE_FAILED = (1 << 29) | 2;

#pragma pack(push, 1)
struct DirectoryWatchRequest {
	char type;
	void *directory;
	bool recursive;
	uint64_t path_length;
	char path[0];
};

struct Event {
	char msg_type;
	void *directory;
	uint32_t action;
};
#pragma pack(pop)

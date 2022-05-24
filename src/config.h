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
const uint32_t ERROR_MODE_CHANGE = (1 << 29) | 3;

#pragma pack(push, 1)
struct HelloRequest {
	char data[HELLO_LENGTH];

	bool is_eq(const char *hello_str);  // utils.cc
};

/*
 * Message types:
 *  D - start watching directory (DirectoryWatchRequest)
 *  S - stop watching directory (DirectoryUnwatchRequest)
 *  U - fs event (Event)
 */

struct DirectoryWatchRequest {
	char msg_type;
	void *directory;
	bool recursive;
	// trailer: path
};

struct DirectoryUnwatchRequest {
	char msg_type;
	void *directory;
};

struct Event {
	char msg_type;
	void *directory;
	uint32_t action;
	// trailer: path
};
#pragma pack(pop)

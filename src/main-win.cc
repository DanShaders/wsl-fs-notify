#include <Windows.h>
#include <fcntl.h>
#include <psapi.h>
#include <stdio.h>
#include <wslapi.h>

#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "message.h"
#include "pe.h"
#include "pipe.h"

std::wstring get_path_by_handle(HANDLE file) {
	const int INITIAL_LEN = 1024;

	std::wstring ret;
	wchar_t *path = (wchar_t *) malloc(INITIAL_LEN * sizeof(wchar_t));
	DWORD len = GetFinalPathNameByHandleW(file, path, INITIAL_LEN, 0);
	if (!len) {
		return L"";
	}
	if (len <= INITIAL_LEN) {
		ret = path;
	} else {
		path = (wchar_t *) realloc(path, len * sizeof(wchar_t));
		DWORD len2 = GetFinalPathNameByHandleW(file, path, len, 0);
		if (len2 > len) {
			return L"";
		}
		ret = path;
	}
	free(path);
	return ret;
}

void ensure_console() {
	static bool has_console = false;

	if (!has_console) {
		if (AllocConsole()) {
			ShowWindow(GetConsoleWindow(), SW_HIDE);
		}
		has_console = true;
	}
}

#define ERR_IF(cond, err)      \
	do {                       \
		if (cond) {            \
			SetLastError(err); \
			return false;      \
		}                      \
	} while (0)

const int STDOUT_BUFF = 1024;

struct ForeignNotifier {
	HANDLE process, in, out;
	char input[STDOUT_BUFF];
	PullableMessageStream in_stream;
	OVERLAPPED out_ov{};
};

struct IOOperation {
	HANDLE notify_in;
	std::deque<PMessage> events;
	void *buffer = nullptr;
	DWORD buffer_length;
	LPOVERLAPPED overlapped;
	LPOVERLAPPED_COMPLETION_ROUTINE overlapped_completion;

	void flush() {
		if (!events.size() || buffer == nullptr) {
			return;
		}

		auto buff = (char *) buffer;
		DWORD offset = 0;
		DWORD *next_offset = nullptr;
		while (events.size()) {
			auto ev = events.front()->as<Event>();
			auto path = events.front()->get_trailer<Event>();
			size_t wlen = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int) path.size(), 0, 0) - 1;
			size_t clen = 2 * wlen + sizeof(FILE_NOTIFY_INFORMATION);

			if (buffer_length - offset < clen) {
				break;
			}

			auto info = (FILE_NOTIFY_INFORMATION *) (buff + offset);
			info->NextEntryOffset = (DWORD) clen;
			info->Action = ev->action;
			info->FileNameLength = (DWORD) wlen;
			MultiByteToWideChar(CP_UTF8, 0, path.data(), (int) path.size(), info->FileName,
								(int) wlen);

			next_offset = &info->NextEntryOffset;
			offset += (int) clen;
			events.pop_front();
		}

		if (next_offset != nullptr) {
			*next_offset = 0;
		}
		buffer = nullptr;
		overlapped_completion(ERROR_SUCCESS, offset, overlapped);
	}
};

std::map<std::wstring, std::shared_ptr<ForeignNotifier>> notifiers;
std::map<HANDLE, IOOperation> io_ops;

void stdout_cb(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
	if (dwErrorCode == ERROR_OPERATION_ABORTED) {
		return;
	}

	auto notifier = (ForeignNotifier *) lpOverlapped->hEvent;
	auto &stream = notifier->in_stream;
	stream.feed(notifier->input, dwNumberOfBytesTransfered);

	std::vector<decltype(io_ops)::iterator> affected;
	while (stream.has_message()) {
		auto msg = *stream.get_message();
		if (msg->data[0] == 'U') {
			if (auto it = io_ops.find(msg->as<Event>()->directory); it != io_ops.end()) {
				it->second.events.push_back(std::move(msg));
				affected.push_back(it);
			}
		}
	}
	for (auto op : affected) {
		op->second.flush();
	}

	ReadFileEx(notifier->out, &notifier->input, STDOUT_BUFF, &notifier->out_ov, stdout_cb);
}

BOOL WINAPI ReadDirectoryChangesW_detour(HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength,
										 BOOL bWatchSubtree, DWORD dwNotifyFilter,
										 LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped,
										 LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
	std::wstring path = get_path_by_handle(hDirectory);
	if (!path.starts_with(LR"(\\?\UNC\wsl$\)")) {
		return ReadDirectoryChangesW(hDirectory, lpBuffer, nBufferLength, bWatchSubtree,
									 dwNotifyFilter, lpBytesReturned, lpOverlapped,
									 lpCompletionRoutine);
	}

	ERR_IF(lpCompletionRoutine == nullptr, ERROR_INVALID_FUNCTION);

	auto sep = path.find(L'\\', 13);
	ERR_IF(sep == path.npos, ERROR_INVALID_FUNCTION);
	auto distro = path.substr(13, sep - 13);
	path = path.substr(sep);
	for (wchar_t &c : path) {
		if (c == L'\\') {
			c = L'/';
		}
	}

	auto it = notifiers.find(distro.c_str());
	if (it == notifiers.end()) {
		auto notifier = std::make_shared<ForeignNotifier>();

		ensure_console();

		HANDLE in, out;
		SECURITY_ATTRIBUTES sa_attrs = {
			.nLength = sizeof(SECURITY_ATTRIBUTES),
			.lpSecurityDescriptor = nullptr,
			.bInheritHandle = true,
		};
		ERR_IF(!MyCreatePipeEx(&in, &notifier->in, &sa_attrs, 0, 0, 0) ||
				   !MyCreatePipeEx(&notifier->out, &out, &sa_attrs, 0, FILE_FLAG_OVERLAPPED, 0) ||
				   WslLaunch(distro.c_str(), WSL_COMMAND, false, in, out,
							 GetStdHandle(STD_OUTPUT_HANDLE), &notifier->process) != S_OK,
			   ERROR_WSL_START_FAILED);

		notifier->in_stream.set_fd(notifier->out);

		HelloRequest client_hello;
		memcpy(client_hello.data, CLIENT_HELLO, HELLO_LENGTH);
		Message::from(client_hello)->write_to(notifier->in);

		auto server_hello = notifier->in_stream.pull_message();
		ERR_IF(!server_hello || !(*server_hello)->as<HelloRequest>()->is_eq(SERVER_HELLO),
			   ERROR_HANDSHAKE_FAILED);

		notifier->out_ov.hEvent = notifier.get();
		assert(
			ReadFileEx(notifier->out, &notifier->input, STDOUT_BUFF, &notifier->out_ov, stdout_cb));
		it = notifiers.insert({distro.c_str(), notifier}).first;
	}

	auto op_it = io_ops.find(hDirectory);
	if (op_it != io_ops.end()) {
		auto &op = op_it->second;
		op.buffer = lpBuffer;
		op.buffer_length = nBufferLength;
		op.overlapped = lpOverlapped;
		op.overlapped_completion = lpCompletionRoutine;
		return true;
	}

	size_t path_length = wcstombs(nullptr, path.c_str(), 0);
	std::string mbpath(path_length, 0);
	wcstombs(&mbpath[0], path.c_str(), path_length + 1);

	io_ops[hDirectory] = {
		.notify_in = it->second->in,
		.events = {},
		.buffer = lpBuffer,
		.buffer_length = nBufferLength,
		.overlapped = lpOverlapped,
		.overlapped_completion = lpCompletionRoutine,
	};
	DirectoryWatchRequest req = {
		.msg_type = 'D',
		.directory = hDirectory,
		.recursive = (bool) bWatchSubtree,
	};
	return Message::from(req, mbpath.c_str(), mbpath.size())->write_to(it->second->in);
}

BOOL WINAPI CancelIo_detour(HANDLE hFile) {
	auto it = io_ops.find(hFile);
	if (it != io_ops.end()) {
		if (it->second.buffer != nullptr) {
			it->second.overlapped_completion(ERROR_OPERATION_ABORTED, 0, it->second.overlapped);
		}

		auto &op = it->second;
		DirectoryUnwatchRequest req = {
			.msg_type = 'S',
			.directory = hFile,
		};
		Message::from(req)->write_to(op.notify_in);
		io_ops.erase(it);
	}
	return CancelIo(hFile);
}

const std::pair<const char *, void *> ENTRIES[] = {
	{"ReadDirectoryChangesW", (void *) ReadDirectoryChangesW_detour},
	{"CancelIo", (void *) CancelIo_detour},
};

void import_callback(char *, char *function_name, uint64_t *import_offset) {
	for (const auto &[function, address] : ENTRIES) {
		if (!strcmp(function_name, function)) {
			DWORD old_protect;
			assert(VirtualProtect(import_offset, 8, PAGE_READWRITE, &old_protect));
			*(import_offset) = (uint64_t) address;
			assert(VirtualProtect(import_offset, 8, old_protect, &old_protect));
		}
	}
}

BOOL WINAPI DllMain([[maybe_unused]] HINSTANCE hinst, [[maybe_unused]] DWORD dwReason,
					[[maybe_unused]] LPVOID reserved) {
	if (dwReason == DLL_PROCESS_ATTACH) {
		HMODULE image = GetModuleHandleW(nullptr);
		assert(image);

		MODULEINFO image_info;
		assert(GetModuleInformation(GetCurrentProcess(), image, &image_info, sizeof(image_info)));

		const size_t IMAGE_NAME_SIZE = 1024;
		wchar_t image_name[IMAGE_NAME_SIZE];
		assert(GetModuleFileNameW(image, image_name, IMAGE_NAME_SIZE));

		for_each_import(image_name, (uint64_t) image_info.lpBaseOfDll, import_callback);
	} else if (dwReason == DLL_PROCESS_DETACH) {
		for (const auto &[name, notifier] : notifiers) {
			CancelIo(notifier->out);
			TerminateProcess(notifier->process, 0);
		}
	}
	return true;
}
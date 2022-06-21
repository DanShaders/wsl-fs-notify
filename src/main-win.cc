//clang-format off
#include <Windows.h>
#include <Detours.h>
//clang-format on
#include <fcntl.h>
#include <psapi.h>
#include <wslapi.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "handle.h"
#include "message.h"
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
	std::atomic<HANDLE> in_read, in_write, out_read, out_write, process;
	HANDLE process_waiter;

	std::atomic_flag failed = ATOMIC_FLAG_INIT;

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

	ReadFileEx(notifier->out_read, &notifier->input, STDOUT_BUFF, &notifier->out_ov, stdout_cb);
}

void check_process(ForeignNotifier &notifier) {
	DWORD exit_code;
	if (GetExitCodeProcess(notifier.process, &exit_code)) {
		if (exit_code != STILL_ACTIVE) {
			notifier.failed.test_and_set();
			close_handle(notifier.in_read);
			close_handle(notifier.in_write);
			close_handle(notifier.out_read);
			close_handle(notifier.out_write);
		}
	}
}

void process_cb(void *raw_notifier, unsigned char timed_out) {
	check_process(*(ForeignNotifier *) raw_notifier);
}

auto ReadDirectoryChangesW_true = ReadDirectoryChangesW;
auto CancelIo_true = CancelIo;

BOOL WINAPI ReadDirectoryChangesW_detour(HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength,
										 BOOL bWatchSubtree, DWORD dwNotifyFilter,
										 LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped,
										 LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
	std::wstring path = get_path_by_handle(hDirectory);
	if (!path.starts_with(LR"(\\?\UNC\wsl$\)")) {
		return ReadDirectoryChangesW_true(hDirectory, lpBuffer, nBufferLength, bWatchSubtree,
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
		ensure_console();

		SECURITY_ATTRIBUTES sa_attrs = {
			.nLength = sizeof(SECURITY_ATTRIBUTES),
			.lpSecurityDescriptor = nullptr,
			.bInheritHandle = true,
		};

		ManagedHandle stdin_read, stdin_write, stdout_read, stdout_write, process;
		ERR_IF(!MyCreatePipeEx(&stdin_read.h, &stdin_write.h, &sa_attrs, 0, 0, 0) ||
				   !MyCreatePipeEx(&stdout_read.h, &stdout_write.h, &sa_attrs, 0,
								   FILE_FLAG_OVERLAPPED, 0) ||
				   WslLaunch(distro.c_str(), WSL_COMMAND, false, stdin_read, stdout_write,
							 GetStdHandle(STD_OUTPUT_HANDLE), &process.h) != S_OK,
			   ERROR_WSL_START_FAILED);

		auto notifier = std::make_shared<ForeignNotifier>(stdin_read, stdin_write, stdout_read,
														  stdout_write, process);
		assert(RegisterWaitForSingleObject(&notifier->process_waiter, notifier->process, process_cb,
										   notifier.get(), INFINITE, WT_EXECUTEONLYONCE));
		check_process(*notifier);

		notifier->in_stream.set_fd(notifier->out_read);

		HelloRequest client_hello;
		memcpy(client_hello.data, CLIENT_HELLO, HELLO_LENGTH);
		ERR_IF(!Message::from(client_hello)->write_to(notifier->in_write), ERROR_HANDSHAKE_FAILED);

		auto server_hello = notifier->in_stream.pull_message();
		ERR_IF(!server_hello || !(*server_hello)->as<HelloRequest>()->is_eq(SERVER_HELLO),
			   ERROR_HANDSHAKE_FAILED);

		notifier->out_ov.hEvent = notifier.get();
		ReadFileEx(notifier->out_read, &notifier->input, STDOUT_BUFF, &notifier->out_ov, stdout_cb);
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
		.notify_in = it->second->in_write,
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
	return Message::from(req, mbpath.c_str(), mbpath.size())->write_to(it->second->in_write);
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
	return CancelIo_true(hFile);
}

BOOL WINAPI DllMain([[maybe_unused]] HINSTANCE hinst, [[maybe_unused]] DWORD dwReason,
					[[maybe_unused]] LPVOID reserved) {
	if (DetourIsHelperProcess()) {
		return TRUE;
	}

	if (dwReason == DLL_PROCESS_ATTACH) {
		DetourRestoreAfterWith();
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());

		DetourAttach(&(void *&) ReadDirectoryChangesW_true, (void *) ReadDirectoryChangesW_detour);
		DetourAttach(&(void *&) CancelIo_true, (void *) CancelIo_detour);

		DetourTransactionCommit();
	} else if (dwReason == DLL_PROCESS_DETACH) {
		for (const auto &[name, notifier] : notifiers) {
			CancelIo_true(notifier->out_read);
			TerminateProcess(notifier->process, 0);
			UnregisterWait(notifier->process_waiter);
		}

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());

		DetourDetach(&(void *&) ReadDirectoryChangesW_true, (void *) ReadDirectoryChangesW_detour);
		DetourDetach(&(void *&) CancelIo_true, (void *) CancelIo_detour);

		DetourTransactionCommit();
	}
	return true;
}
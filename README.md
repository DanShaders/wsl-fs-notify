# wsl-fs-notify

Implements filesystem notifications for WSL 2 UNC paths (e.g. \\wsl$\Arch\root)

## Compiling from source
On Windows: `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build-win . && ninja -C build-win`

In WSL: `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build-linux . && ninja -C build-linux install`

## Running
You should make the process load `build-win/libwsl-fs-notify.dll` before any calls to `ReadDirectoryChangesW`.

One of the ways to do so is by using `withdll.exe` example program from [Microsoft Detours library](https://github.com/microsoft/Detours).
So then start program like this: `withdll.exe /d:build-win/libwsl-fs-notify.dll "C:\Program Files\Sublime Text\sublime_text.exe"`.

## Limitations
1. Reasonably modern Windows 10/11 x64
2. Not thread-safe
3. `ReadDirectoryChangesW` must be called by executable itself (not some other loaded DLL)
4. Only asynchronous calls to `ReadDirectoryChangesW` with a completion routine are supported.
5. `bWatchSubtree` is ignored

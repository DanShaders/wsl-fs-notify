# wsl-fs-notify

Implements filesystem notifications for WSL 2 UNC paths (e.g. \\wsl$\Arch\root)

## Compiling from source
### Build dependencies:
- Windows: MinGW GCC ^11, CMake, Ninja build (or any other build system)
- WSL: GCC ^11, CMake, Ninja build (or any other build system), libev

### Compilation:
```
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build-win . && ninja -C build-win
wsl -- cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B build-linux . ^&^& ninja -C build-linux install
```

Then copy `/usr/local/bin/wsl-fs-notify` to every WSL distro you want to get notifications from.

## Running
In general, you should make the process load `build-win/wsl-fs-notify.dll` as early as possible.

One of the ways to do so is by using `withdll.exe` from [Microsoft Detours library](https://github.com/microsoft/Detours) (it is compiled by default). So then start program like this:
```
"build-win/withdll.exe" /d:build-win/wsl-fs-notify.dll "C:\Program Files\Sublime Text\sublime_text.exe"
```

## Limitations
1. Not thread-safe
2. Only asynchronous calls to `ReadDirectoryChangesW` with a completion routine are supported.
3. `bWatchSubtree` is not fully implemented
4. `dwNotifyFilter` is ignored

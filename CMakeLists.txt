cmake_minimum_required(VERSION 3.0.0 FATAL_ERROR)

project(wsl-fs-notify VERSION 0.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wshadow -Wconversion -fno-exceptions")
set(CMAKE_CXX_FLAGS_DEBUG "-Og")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

if (WIN32)
	add_library(wsl-fs-notify SHARED
		src/main-win.cc
		src/message.cc
		src/pe.cc
		src/pipe.cc
	)
	target_link_libraries(wsl-fs-notify PRIVATE Wslapi)
elseif (UNIX)
	add_executable(wsl-fs-notify
		src/main-wsl.cc
		src/message.cc
	)
	target_link_libraries(wsl-fs-notify PRIVATE ev)
	install(TARGETS wsl-fs-notify)
endif()
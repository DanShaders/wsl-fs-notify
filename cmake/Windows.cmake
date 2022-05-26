include(cmake/Detours.cmake)

add_library(
	wsl-fs-notify SHARED
	src/main-win.cc
	src/message.cc
	src/pipe.cc
	src/utils.cc
)

target_link_libraries(
	wsl-fs-notify PRIVATE
	Wslapi
	Detours
)

set_target_properties(wsl-fs-notify PROPERTIES PREFIX "")

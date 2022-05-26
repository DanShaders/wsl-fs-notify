add_executable(wsl-fs-notify
	src/main-wsl.cc
	src/message.cc
	src/utils.cc
)
target_link_libraries(wsl-fs-notify PRIVATE ev)
install(TARGETS wsl-fs-notify)

set(DETOURS_ROOT "${CMAKE_SOURCE_DIR}/dependencies/Detours/")
list(
	APPEND DETOURS_SOURCES
    src/creatwth.cpp
    src/detours.cpp
    src/disasm.cpp
    src/disolarm.cpp
    src/disolarm64.cpp
    src/disolia64.cpp
    src/disolx64.cpp
    src/disolx86.cpp
    src/image.cpp
    src/modules.cpp
)

add_library(Detours)

foreach(FILE IN LISTS DETOURS_SOURCES)
	target_sources(Detours PRIVATE "${DETOURS_ROOT}${FILE}")
endforeach()

target_compile_options(
	Detours PRIVATE
	-Wno-attributes
	-Wno-cast-function-type
	-Wno-conversion
	-Wno-conversion-null
	-Wno-pointer-arith
	-Wno-reorder
	-Wno-sign-compare
	-Wno-tautological-compare
	-Wno-unknown-pragmas
	-fno-strict-aliasing
	-DDETOUR_DEBUG=0
	-DWIN32_LEAN_AND_MEAN
	-D_WIN32_WINNT=0x501
)

configure_file(${DETOURS_ROOT}/src/detours.h ${DETOURS_ROOT}/include/detours.h COPYONLY)
configure_file(${DETOURS_ROOT}/src/detver.h ${DETOURS_ROOT}/include/detver.h COPYONLY)

target_include_directories(
	Detours INTERFACE
	${DETOURS_ROOT}/include
)


option(BUILD_WITHDLL "Build Detour's withdll.exe" ON)

if (BUILD_WITHDLL)
	add_executable(
		Detours_withdll
		${DETOURS_ROOT}/samples/withdll/withdll.cpp
	)
	target_link_libraries(Detours_withdll Detours)
	target_link_options(Detours_withdll PRIVATE -mwindows)
	set_target_properties(Detours_withdll PROPERTIES OUTPUT_NAME "withdll")
endif()

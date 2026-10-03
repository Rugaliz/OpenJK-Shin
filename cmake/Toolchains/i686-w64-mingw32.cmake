set(GNU_HOST i686-w64-mingw32)
set(CMAKE_SYSTEM_PROCESSOR "i686")

set(COMPILER_PREFIX "${GNU_HOST}-")

set(CMAKE_SYSTEM_NAME "Windows")
set(CMAKE_CROSSCOMPILING TRUE)
set(WIN32 TRUE)
set(MINGW TRUE)

set(CMAKE_C_COMPILER ${COMPILER_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${COMPILER_PREFIX}g++)
set(CMAKE_RC_COMPILER ${COMPILER_PREFIX}windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Tools that are built and run during the build (the GLSL compactor of rend2) are Windows programs: run them with Wine
find_program(WINE_EXECUTABLE NAMES wine wine64)
if(WINE_EXECUTABLE)
	set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE_EXECUTABLE})
endif()

# Don't depend on the compiler's runtime DLLs (libstdc++, libc++, libgcc, libunwind, winpthread)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static")

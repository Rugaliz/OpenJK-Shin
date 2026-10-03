#!/usr/bin/env bash
#
# Builds every game in this repository on Linux (and other Unix-like systems):
#
#   Jedi Outcast   single player   (openjo_sp)
#   Jedi Outcast   multiplayer     (openjo client, openjoded dedicated server)
#   Jedi Academy   single player   (openjk_sp)
#   Jedi Academy   multiplayer     (openjk client, openjkded dedicated server, game/cgame/ui modules, renderers)
#
# It checks that what the build needs is installed (and says how to install what is missing),
# configures with CMake, builds, and copies the result to a folder laid out like the game
# folders, ready to be copied into the GameData folder of each game.
#
# Run ./build.sh --help for the options.

set -u
set -o pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ----------------------------------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------------------------------

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
	C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_BLUE=$'\033[34m'
	C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
	C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_BOLD=""; C_DIM=""; C_OFF=""
fi

step()  { printf '\n%s==>%s %s%s%s\n' "$C_BLUE" "$C_OFF" "$C_BOLD" "$*" "$C_OFF"; }
ok()    { printf '  %s[ ok ]%s %s\n' "$C_GREEN" "$C_OFF" "$*"; }
warn()  { printf '  %s[warn]%s %s\n' "$C_YELLOW" "$C_OFF" "$*"; }
fail()  { printf '  %s[MISS]%s %s\n' "$C_RED" "$C_OFF" "$*"; }
info()  { printf '  %s%s%s\n' "$C_DIM" "$*" "$C_OFF"; }
die()   { printf '\n%serror:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

# ----------------------------------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------------------------------

BUILD_TYPE="Release"
BUILD_DIR="$SOURCE_DIR/build"
OUTPUT_DIR="$SOURCE_DIR/dist"
JOBS=""
GENERATOR=""
WHAT="all"                 # all, or a comma separated list of: jo, ja-sp, ja-mp
USE_INTERNAL_LIBS=0
USE_INTERNAL_SDL3=""       # empty = decide from what is installed
BUILD_REND2=1
DO_CLEAN=0
DO_INSTALL=1
CHECK_ONLY=0
ASSUME_YES=0
EXTRA_CMAKE_ARGS=()

usage() {
	cat <<EOF
${C_BOLD}Usage:${C_OFF} ./build.sh [options] [-- extra CMake arguments]

Builds the games and copies them to a folder ready to be copied into the game folders.

${C_BOLD}What to build${C_OFF}
  --only LIST           Comma separated list of what to build (default: all)
                          jo     Jedi Outcast single player
                          jo-mp  Jedi Outcast multiplayer (client, dedicated server, renderer)
                          ja-sp  Jedi Academy single player
                          ja-mp  Jedi Academy multiplayer (client, dedicated server, modules)
  --no-rend2            Leave out the experimental multiplayer rend2 renderer

${C_BOLD}How to build${C_OFF}
  -t, --type TYPE       Release (default), Debug, RelWithDebInfo or MinSizeRel
  -j, --jobs N          Number of parallel jobs (default: all processors)
  -G, --generator NAME  CMake generator (default: Ninja if installed, else Unix Makefiles)
  -b, --build-dir DIR   Where to build (default: ./build)
  -o, --output DIR      Where to copy the result (default: ./dist)
      --no-install      Only build, do not copy the result to the output folder
      --clean           Delete the build folder first

${C_BOLD}Libraries${C_OFF}
      --internal-libs   Build zlib, libpng, libjpeg and SDL3 from the copies in this repository / downloaded
                        (SDL3 is downloaded the first time, which needs an internet connection)
      --internal-sdl3   Download and build SDL3 even if one is installed
      --system-sdl3     Never download SDL3 (fail if it is not installed)

${C_BOLD}Other${C_OFF}
      --check           Only check that everything needed is installed, then stop
  -y, --yes             Do not stop to ask questions
  -h, --help            This text

Examples:
  ./build.sh                          everything, Release
  ./build.sh --only jo -j 8           only Jedi Outcast, 8 jobs
  ./build.sh -t Debug --only ja-mp    Jedi Academy multiplayer with debug information
  ./build.sh --check                  see what is missing without building
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--only)           WHAT="${2:-}"; shift 2 ;;
		--only=*)         WHAT="${1#*=}"; shift ;;
		--no-rend2)       BUILD_REND2=0; shift ;;
		-t|--type)        BUILD_TYPE="${2:-}"; shift 2 ;;
		--type=*)         BUILD_TYPE="${1#*=}"; shift ;;
		-j|--jobs)        JOBS="${2:-}"; shift 2 ;;
		--jobs=*)         JOBS="${1#*=}"; shift ;;
		-G|--generator)   GENERATOR="${2:-}"; shift 2 ;;
		--generator=*)    GENERATOR="${1#*=}"; shift ;;
		-b|--build-dir)   BUILD_DIR="${2:-}"; shift 2 ;;
		--build-dir=*)    BUILD_DIR="${1#*=}"; shift ;;
		-o|--output)      OUTPUT_DIR="${2:-}"; shift 2 ;;
		--output=*)       OUTPUT_DIR="${1#*=}"; shift ;;
		--no-install)     DO_INSTALL=0; shift ;;
		--clean)          DO_CLEAN=1; shift ;;
		--internal-libs)  USE_INTERNAL_LIBS=1; USE_INTERNAL_SDL3=1; shift ;;
		--internal-sdl3)  USE_INTERNAL_SDL3=1; shift ;;
		--system-sdl3)    USE_INTERNAL_SDL3=0; shift ;;
		--check)          CHECK_ONLY=1; shift ;;
		-y|--yes)         ASSUME_YES=1; shift ;;
		-h|--help)        usage; exit 0 ;;
		--)               shift; EXTRA_CMAKE_ARGS=("$@"); break ;;
		*)                printf 'Unknown option: %s\n\n' "$1" >&2; usage >&2; exit 2 ;;
	esac
done

case "$BUILD_TYPE" in
	Release|Debug|RelWithDebInfo|MinSizeRel) ;;
	*) die "unknown build type '$BUILD_TYPE' (use Release, Debug, RelWithDebInfo or MinSizeRel)" ;;
esac

[[ -n "$JOBS" && ! "$JOBS" =~ ^[1-9][0-9]*$ ]] && die "--jobs needs a number, got '$JOBS'"
if [[ -z "$JOBS" ]]; then
	JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

BUILD_JO=0; BUILD_JO_MP=0; BUILD_JA_SP=0; BUILD_JA_MP=0
if [[ "$WHAT" == "all" ]]; then
	BUILD_JO=1; BUILD_JO_MP=1; BUILD_JA_SP=1; BUILD_JA_MP=1
else
	IFS=',' read -r -a WHAT_LIST <<< "$WHAT"
	for item in "${WHAT_LIST[@]}"; do
		case "$item" in
			jo)    BUILD_JO=1 ;;
			jo-mp) BUILD_JO_MP=1 ;;
			ja-sp) BUILD_JA_SP=1 ;;
			ja-mp) BUILD_JA_MP=1 ;;
			*)     die "unknown value '$item' for --only (use jo, jo-mp, ja-sp, ja-mp)" ;;
		esac
	done
fi

# ----------------------------------------------------------------------------------------------
# Checking what is installed
# ----------------------------------------------------------------------------------------------

MISSING_REQUIRED=0
MISSING_PACKAGES=()      # package names for the detected distribution

DISTRO_FAMILY="unknown"
detect_distro() {
	local id="" like=""
	if [[ -r /etc/os-release ]]; then
		# shellcheck disable=SC1091
		id="$(. /etc/os-release; echo "${ID:-}")"
		like="$(. /etc/os-release; echo "${ID_LIKE:-}")"
	fi
	case " $id $like " in
		*" arch "*|*" manjaro "*|*" endeavouros "*) DISTRO_FAMILY="arch" ;;
		*" debian "*|*" ubuntu "*)                  DISTRO_FAMILY="debian" ;;
		*" fedora "*|*" rhel "*|*" centos "*)       DISTRO_FAMILY="fedora" ;;
		*" suse "*|*" opensuse "*)                  DISTRO_FAMILY="suse" ;;
		*" alpine "*)                               DISTRO_FAMILY="alpine" ;;
	esac
	if [[ "$(uname -s)" == "Darwin" ]]; then DISTRO_FAMILY="macos"; fi
}

# pkg <key>: the package that provides <key> on this distribution
pkg() {
	local key="$1"
	case "$DISTRO_FAMILY:$key" in
		arch:cmake)  echo cmake ;;           arch:ninja)  echo ninja ;;
		arch:cxx)    echo base-devel ;;      arch:pkgconf) echo pkgconf ;;
		arch:sdl3)   echo sdl3 ;;            arch:zlib)   echo zlib ;;
		arch:png)    echo libpng ;;          arch:jpeg)   echo libjpeg-turbo ;;
		arch:gl)     echo libglvnd ;;        arch:git)    echo git ;;
		arch:x11)    echo "libx11 libxext libxcursor libxi libxrandr libxkbcommon wayland" ;;

		debian:cmake) echo cmake ;;          debian:ninja) echo ninja-build ;;
		debian:cxx)   echo build-essential ;; debian:pkgconf) echo pkg-config ;;
		debian:sdl3)  echo libsdl3-dev ;;    debian:zlib)  echo zlib1g-dev ;;
		debian:png)   echo libpng-dev ;;     debian:jpeg)  echo libjpeg-dev ;;
		debian:gl)    echo libgl-dev ;;      debian:git)   echo git ;;
		debian:x11)   echo "libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxkbcommon-dev libwayland-dev" ;;

		fedora:cmake) echo cmake ;;          fedora:ninja) echo ninja-build ;;
		fedora:cxx)   echo "gcc-c++ make" ;; fedora:pkgconf) echo pkgconf-pkg-config ;;
		fedora:sdl3)  echo SDL3-devel ;;     fedora:zlib)  echo zlib-devel ;;
		fedora:png)   echo libpng-devel ;;   fedora:jpeg)  echo libjpeg-turbo-devel ;;
		fedora:gl)    echo mesa-libGL-devel ;; fedora:git) echo git ;;
		fedora:x11)   echo "libX11-devel libXext-devel libXcursor-devel libXi-devel libXrandr-devel libxkbcommon-devel wayland-devel" ;;

		suse:cmake)   echo cmake ;;          suse:ninja)   echo ninja ;;
		suse:cxx)     echo "gcc-c++ make" ;; suse:pkgconf) echo pkg-config ;;
		suse:sdl3)    echo SDL3-devel ;;     suse:zlib)    echo zlib-devel ;;
		suse:png)     echo libpng16-devel ;; suse:jpeg)    echo libjpeg8-devel ;;
		suse:gl)      echo Mesa-libGL-devel ;; suse:git)   echo git ;;
		suse:x11)     echo "libX11-devel libXext-devel libXcursor-devel libXi-devel libXrandr-devel libxkbcommon-devel wayland-devel" ;;

		alpine:cmake) echo cmake ;;          alpine:ninja) echo samurai ;;
		alpine:cxx)   echo build-base ;;     alpine:pkgconf) echo pkgconf ;;
		alpine:sdl3)  echo sdl3-dev ;;       alpine:zlib)  echo zlib-dev ;;
		alpine:png)   echo libpng-dev ;;     alpine:jpeg)  echo libjpeg-turbo-dev ;;
		alpine:gl)    echo mesa-dev ;;       alpine:git)   echo git ;;
		alpine:x11)   echo "libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxkbcommon-dev wayland-dev" ;;

		macos:cmake)  echo cmake ;;          macos:ninja)  echo ninja ;;
		macos:cxx)    echo "xcode-select --install" ;; macos:pkgconf) echo pkg-config ;;
		macos:sdl3)   echo sdl3 ;;           macos:zlib)   echo zlib ;;
		macos:png)    echo libpng ;;         macos:jpeg)   echo jpeg-turbo ;;
		macos:gl)     echo "" ;;             macos:git)    echo git ;;
		macos:x11)    echo "" ;;
		*) echo "" ;;
	esac
}

install_hint() {
	[[ ${#MISSING_PACKAGES[@]} -eq 0 ]] && return
	local list; list="$(printf '%s\n' "${MISSING_PACKAGES[@]}" | tr ' ' '\n' | awk 'NF && !seen[$0]++' | tr '\n' ' ')"
	printf '\n%sTo install what is missing:%s\n' "$C_BOLD" "$C_OFF"
	case "$DISTRO_FAMILY" in
		arch)   printf '  sudo pacman -S --needed %s\n' "$list" ;;
		debian) printf '  sudo apt install %s\n' "$list" ;;
		fedora) printf '  sudo dnf install %s\n' "$list" ;;
		suse)   printf '  sudo zypper install %s\n' "$list" ;;
		alpine) printf '  sudo apk add %s\n' "$list" ;;
		macos)  printf '  brew install %s\n' "$list" ;;
		*)      printf '  Install the development packages for: cmake, a C++ compiler, SDL3, zlib, libpng, libjpeg, OpenGL\n' ;;
	esac
}

# need <found 0/1> <label> <package key> <detail on failure>
report_required() {
	local found="$1" label="$2" key="$3" detail="${4:-}"
	if [[ "$found" == 1 ]]; then
		ok "$label"
	else
		fail "$label${detail:+ - $detail}"
		MISSING_REQUIRED=1
		local p; p="$(pkg "$key")"
		[[ -n "$p" ]] && MISSING_PACKAGES+=("$p")
	fi
}

have_cmd() { command -v "$1" >/dev/null 2>&1; }

version_ge() { # version_ge <have> <need>
	[[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]
}

C_COMPILER=""
CXX_COMPILER=""
pick_compilers() {
	C_COMPILER="${CC:-}"; CXX_COMPILER="${CXX:-}"
	[[ -z "$C_COMPILER" ]]   && for c in cc gcc clang; do have_cmd "$c" && { C_COMPILER="$c"; break; }; done
	[[ -z "$CXX_COMPILER" ]] && for c in c++ g++ clang++; do have_cmd "$c" && { CXX_COMPILER="$c"; break; }; done
}

header_ok() { # header_ok <header>: the C compiler can find it
	[[ -n "$C_COMPILER" ]] && printf '#include <%s>\n' "$1" | "$C_COMPILER" -E -x c - >/dev/null 2>&1
}

lib_ok() { # lib_ok <-lname ...>: the C compiler can link it
	[[ -n "$C_COMPILER" ]] && printf 'int main(void){return 0;}\n' | "$C_COMPILER" -x c - "$@" -o /dev/null >/dev/null 2>&1
}

pkg_version() { have_cmd pkg-config && pkg-config --modversion "$1" 2>/dev/null; }

check_dependencies() {
	detect_distro
	pick_compilers

	step "Checking what the build needs"
	info "System: $(uname -sm), ${DISTRO_FAMILY} family"

	# --- tools ---
	local cmake_ver=""
	if have_cmd cmake; then
		cmake_ver="$(cmake --version | head -n1 | awk '{print $3}')"
		if version_ge "$cmake_ver" "3.16"; then
			report_required 1 "cmake $cmake_ver" cmake
		else
			report_required 0 "cmake $cmake_ver" cmake "3.16 or newer is needed"
		fi
	else
		report_required 0 "cmake" cmake "not found"
	fi

	if [[ -n "$C_COMPILER" && -n "$CXX_COMPILER" ]]; then
		local cxx_ver; cxx_ver="$("$CXX_COMPILER" --version 2>/dev/null | head -n1)"
		report_required 1 "C and C++ compiler (${cxx_ver:-$CXX_COMPILER})" cxx
	else
		report_required 0 "C and C++ compiler" cxx "none of cc/gcc/clang and c++/g++/clang++ found"
	fi

	if [[ -z "$GENERATOR" ]]; then
		if have_cmd ninja; then
			GENERATOR="Ninja"; ok "ninja (build tool)"
		elif have_cmd make; then
			GENERATOR="Unix Makefiles"; warn "ninja not found, using make (slower; the ninja package is optional but recommended)"
		else
			report_required 0 "ninja or make (build tool)" ninja "neither found"
		fi
	else
		ok "generator: $GENERATOR"
	fi

	if have_cmd pkg-config; then
		ok "pkg-config"
	else
		warn "pkg-config not found, libraries are looked for by name instead (can be less reliable)"
		local p; p="$(pkg pkgconf)"; [[ -n "$p" ]] && MISSING_PACKAGES+=("$p")
	fi

	# --- libraries ---
	if [[ $USE_INTERNAL_LIBS -eq 0 ]]; then
		if header_ok zlib.h && lib_ok -lz; then ok "zlib"; else report_required 0 "zlib (development files)" zlib "or use --internal-libs"; fi
		if header_ok png.h && lib_ok -lpng; then ok "libpng"; else report_required 0 "libpng (development files)" png "or use --internal-libs"; fi
		if header_ok jpeglib.h && lib_ok -ljpeg; then ok "libjpeg"; else report_required 0 "libjpeg (development files)" jpeg "or use --internal-libs"; fi
	else
		ok "zlib, libpng, libjpeg: the copies in lib/ are used (--internal-libs)"
	fi

	if header_ok GL/gl.h || header_ok OpenGL/gl.h; then
		ok "OpenGL headers"
	else
		report_required 0 "OpenGL headers" gl "GL/gl.h not found"
	fi

	# SDL3 (3.2.0 or newer): installed, or downloaded and built by CMake
	local sdl_ver=""
	sdl_ver="$(pkg_version sdl3)"
	if [[ -z "$USE_INTERNAL_SDL3" ]]; then
		if [[ -n "$sdl_ver" ]] && version_ge "$sdl_ver" "3.2.0"; then
			USE_INTERNAL_SDL3=0
		else
			USE_INTERNAL_SDL3=1
		fi
	fi
	if [[ "$USE_INTERNAL_SDL3" == 0 ]]; then
		if [[ -z "$sdl_ver" ]]; then
			report_required 0 "SDL3 (development files)" sdl3 "not found; or let the build download it with --internal-sdl3"
		elif ! version_ge "$sdl_ver" "3.2.0"; then
			report_required 0 "SDL3 $sdl_ver" sdl3 "3.2.0 or newer is needed; or let the build download it with --internal-sdl3"
		else
			ok "SDL3 $sdl_ver"
		fi
	else
		if [[ -n "$sdl_ver" ]]; then
			info "SDL3 $sdl_ver is installed but a copy is downloaded and built instead, as asked"
		else
			warn "SDL3 is not installed: CMake will download and build SDL3 3.4.16 (needs an internet connection)"
			info "installing the SDL3 package instead is quicker and gets you the system's audio and video drivers"
		fi
		# The bundled SDL3 needs the window system development files to get X11/Wayland support
		if [[ "$(uname -s)" == "Linux" ]]; then
			if have_cmd pkg-config && pkg-config --exists x11 xext 2>/dev/null; then
				ok "X11 development files (for the downloaded SDL3)"
			else
				warn "X11/Wayland development files not found: a downloaded SDL3 may be built without window system support"
				local p; p="$(pkg x11)"; [[ -n "$p" ]] && MISSING_PACKAGES+=("$p")
			fi
		fi
	fi

	# --- source tree ---
	if [[ -f "$SOURCE_DIR/CMakeLists.txt" && -d "$SOURCE_DIR/codemp" && -d "$SOURCE_DIR/code" ]]; then
		ok "source tree: $SOURCE_DIR"
	else
		report_required 0 "source tree" cmake "run this script from the root of the repository"
	fi

	if [[ $MISSING_REQUIRED -ne 0 ]]; then
		printf '\n%sSomething the build needs is missing (see the lines marked [MISS]).%s\n' "$C_RED" "$C_OFF"
		install_hint
		return 1
	fi
	printf '\n%sEverything needed is installed.%s\n' "$C_GREEN" "$C_OFF"
	return 0
}

# ----------------------------------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------------------------------

if ! check_dependencies; then
	exit 1
fi

if [[ $CHECK_ONLY -eq 1 ]]; then
	exit 0
fi

onoff() { [[ "$1" -eq 1 ]] && echo ON || echo OFF; }

step "Configuring ($BUILD_TYPE, $GENERATOR, $JOBS jobs)"

[[ -n "$BUILD_DIR" && "$BUILD_DIR" != "/" ]] || die "refusing to use '$BUILD_DIR' as the build folder"
if [[ $DO_CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
	[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || die "'$BUILD_DIR' does not look like a CMake build folder, not deleting it"
	info "deleting $BUILD_DIR"
	rm -rf "$BUILD_DIR"
fi

# A build folder stays tied to the generator it was created with
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
	existing="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")"
	if [[ -n "$existing" && "$existing" != "$GENERATOR" ]]; then
		warn "$BUILD_DIR was made with '$existing', using that (use --clean to start over)"
		GENERATOR="$existing"
	fi
fi

CMAKE_ARGS=(
	-S "$SOURCE_DIR" -B "$BUILD_DIR" -G "$GENERATOR"
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE"
	-DBuildJK2SPEngine="$(onoff $BUILD_JO)"
	-DBuildJK2SPGame="$(onoff $BUILD_JO)"
	-DBuildJK2SPRdVanilla="$(onoff $BUILD_JO)"
	-DBuildJK2MPEngine="$(onoff $BUILD_JO_MP)"
	-DBuildJK2MPDed="$(onoff $BUILD_JO_MP)"
	-DBuildJK2MPRdVanilla="$(onoff $BUILD_JO_MP)"
	-DBuildSPEngine="$(onoff $BUILD_JA_SP)"
	-DBuildSPGame="$(onoff $BUILD_JA_SP)"
	-DBuildSPRdVanilla="$(onoff $BUILD_JA_SP)"
	-DBuildMPEngine="$(onoff $BUILD_JA_MP)"
	-DBuildMPDed="$(onoff $BUILD_JA_MP)"
	-DBuildMPGame="$(onoff $BUILD_JA_MP)"
	-DBuildMPCGame="$(onoff $BUILD_JA_MP)"
	-DBuildMPUI="$(onoff $BUILD_JA_MP)"
	-DBuildMPRdVanilla="$(onoff $BUILD_JA_MP)"
	-DBuildMPRend2="$(onoff $((BUILD_JA_MP && BUILD_REND2)))"
	-DBuildTests=OFF
	-DUseInternalSDL3="$(onoff "$USE_INTERNAL_SDL3")"
)
if [[ $USE_INTERNAL_LIBS -eq 1 ]]; then
	CMAKE_ARGS+=(-DUseInternalLibs=ON -DUseInternalZlib=ON -DUseInternalPNG=ON -DUseInternalJPEG=ON)
fi
CMAKE_ARGS+=(${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"})

if ! cmake "${CMAKE_ARGS[@]}"; then
	die "CMake could not configure the project (see the messages above)"
fi

step "Building"
START_TIME=$SECONDS
if ! cmake --build "$BUILD_DIR" --parallel "$JOBS"; then
	die "the build failed (see the messages above)"
fi
ok "built in $((SECONDS - START_TIME)) s"

if [[ $DO_INSTALL -eq 1 ]]; then
	step "Copying the result to $OUTPUT_DIR"
	if ! cmake --install "$BUILD_DIR" --prefix "$OUTPUT_DIR" >/dev/null; then
		die "could not copy the result to $OUTPUT_DIR"
	fi
fi

step "Done"
if [[ $DO_INSTALL -eq 1 ]]; then
	for game_dir in "$OUTPUT_DIR"/JediOutcast "$OUTPUT_DIR"/JediAcademy; do
		[[ -d "$game_dir" ]] || continue
		printf '  %s%s%s\n' "$C_BOLD" "${game_dir#"$SOURCE_DIR"/}" "$C_OFF"
		(cd "$game_dir" && find . -type f | sort | sed 's|^\./|      |')
	done
	cat <<EOF

Copy the contents of each folder into the matching game's GameData folder (next to the base/ folder),
then run the executable: openjo_sp.x86_64 / openjo.x86_64 for Jedi Outcast, openjk_sp.x86_64 / openjk.x86_64 for Jedi Academy.
EOF
else
	printf '  The programs are in %s\n' "$BUILD_DIR"
fi

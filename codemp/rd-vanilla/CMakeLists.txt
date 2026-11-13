#============================================================================
# Copyright (C) 2013 - 2018, OpenJK contributors
#
# This file is part of the OpenJK source code.
#
# OpenJK is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.
#============================================================================

# Make sure the user is not executing this script directly
if(NOT InOpenJK)
	message(FATAL_ERROR "Use the top-level cmake script!")
endif(NOT InOpenJK)

# Check for dependencies first

set(MPVanillaRendererIncludeDirectories ${MPDir})
set(MPVanillaRendererIncludeDirectories
	${MPVanillaRendererIncludeDirectories}
	${SharedDir}
	${MPDir}
	"${MPDir}/rd-vanilla"
	"${GSLIncludeDirectory}"
	)

set(MPVanillaRendererFiles
	"${MPDir}/rd-vanilla/glext.h"
	"${MPDir}/rd-vanilla/qgl.h"
	"${MPDir}/rd-vanilla/G2_API.cpp"
	"${MPDir}/rd-vanilla/G2_bolts.cpp"
	"${MPDir}/rd-vanilla/G2_bones.cpp"
	"${MPDir}/rd-vanilla/G2_misc.cpp"
	"${MPDir}/rd-vanilla/G2_surfaces.cpp"
	"${MPDir}/rd-vanilla/tr_arb.cpp"
	"${MPDir}/rd-vanilla/tr_backend.cpp"
	"${MPDir}/rd-vanilla/tr_bsp.cpp"
	"${MPDir}/rd-vanilla/tr_cmds.cpp"
	"${MPDir}/rd-vanilla/tr_curve.cpp"
	"${MPDir}/rd-vanilla/tr_decals.cpp"
	"${MPDir}/rd-vanilla/tr_ghoul2.cpp"
	"${MPDir}/rd-vanilla/tr_glsl.cpp"
	"${MPDir}/rd-vanilla/tr_image.cpp"
	"${MPDir}/rd-vanilla/tr_init.cpp"
	"${MPDir}/rd-vanilla/tr_light.cpp"
	"${MPDir}/rd-vanilla/tr_local.h"
	"${MPDir}/rd-vanilla/tr_main.cpp"
	"${MPDir}/rd-vanilla/tr_marks.cpp"
	"${MPDir}/rd-vanilla/tr_mesh.cpp"
	"${MPDir}/rd-vanilla/tr_post.cpp"
	"${MPDir}/rd-vanilla/tr_model.cpp"
	"${MPDir}/rd-vanilla/tr_quicksprite.cpp"
	"${MPDir}/rd-vanilla/tr_quicksprite.h"
	"${MPDir}/rd-vanilla/tr_scene.cpp"
	"${MPDir}/rd-vanilla/tr_shade.cpp"
	"${MPDir}/rd-vanilla/tr_shade_calc.cpp"
	"${MPDir}/rd-vanilla/tr_shader.cpp"
	"${MPDir}/rd-vanilla/tr_shadows.cpp"
	"${MPDir}/rd-vanilla/tr_skin.cpp"
	"${MPDir}/rd-vanilla/tr_sky.cpp"
	"${MPDir}/rd-vanilla/tr_subs.cpp"
	"${MPDir}/rd-vanilla/tr_surface.cpp"
	"${MPDir}/rd-vanilla/tr_surfacesprites.cpp"
	"${MPDir}/rd-vanilla/tr_world.cpp"
	"${MPDir}/rd-vanilla/tr_WorldEffects.cpp"
	"${MPDir}/rd-vanilla/tr_WorldEffects.h"
	)
source_group("renderer" FILES ${MPVanillaRendererFiles})

set(MPVanillaRendererGhoul2Files
	"${MPDir}/ghoul2/g2_local.h"
	"${MPDir}/ghoul2/ghoul2_shared.h"
	"${MPDir}/ghoul2/G2_gore.cpp"
	"${MPDir}/ghoul2/G2_gore.h")
source_group("ghoul2" FILES ${MPVanillaRendererGhoul2Files})
set(MPVanillaRendererFiles ${MPVanillaRendererFiles} ${MPVanillaRendererGhoul2Files})

set(MPVanillaRendererRdCommonFiles
	"${MPDir}/rd-common/mdx_format.h"
	"${MPDir}/rd-common/tr_common.h"
	"${MPDir}/rd-common/tr_font.cpp"
	"${MPDir}/rd-common/tr_font.h"
	"${MPDir}/rd-common/tr_image_load.cpp"
	"${MPDir}/rd-common/tr_image_jpg.cpp"
	"${MPDir}/rd-common/tr_image_tga.cpp"
	"${MPDir}/rd-common/tr_image_png.cpp"
	"${MPDir}/rd-common/tr_noise.cpp"
	"${MPDir}/rd-common/tr_public.h"
	"${MPDir}/rd-common/tr_types.h")
source_group("rd-common" FILES ${MPVanillaRendererRdCommonFiles})
set(MPVanillaRendererFiles ${MPVanillaRendererFiles} ${MPVanillaRendererRdCommonFiles})

set(MPVanillaRendererCommonFiles
	"${MPDir}/qcommon/matcomp.cpp"
	"${MPDir}/qcommon/q_shared.cpp"

	${SharedCommonFiles})
source_group("common" FILES ${MPVanillaRendererCommonFiles})
set(MPVanillaRendererFiles ${MPVanillaRendererFiles} ${MPVanillaRendererCommonFiles})

set(MPVanillaRendererCommonSafeFiles
	${SharedCommonSafeFiles}
	)
source_group("common/safe" FILES ${MPVanillaRendererCommonSafeFiles})
set(MPVanillaRendererFiles ${MPVanillaRendererFiles} ${MPVanillaRendererCommonSafeFiles})

# Transparently use either bundled or system libjpeg.
list(APPEND MPVanillaRendererIncludeDirectories ${JPEG_INCLUDE_DIR})
list(APPEND MPVanillaRendererLibraries          ${JPEG_LIBRARIES})

# Transparently use either bundled or system libpng.  Order is important --
# libpng used zlib, so it must come before it on the linker command line.
list(APPEND MPVanillaRendererIncludeDirectories ${PNG_INCLUDE_DIRS})
list(APPEND MPVanillaRendererLibraries          ${PNG_LIBRARIES})

# Transparently use either bundled or system zlib.
list(APPEND MPVanillaRendererIncludeDirectories ${ZLIB_INCLUDE_DIR})
list(APPEND MPVanillaRendererLibraries          ${ZLIB_LIBRARIES})

# Transparently use our bundled minizip.
list(APPEND MPVanillaRendererIncludeDirectories ${MINIZIP_INCLUDE_DIRS})
list(APPEND MPVanillaRendererLibraries          ${MINIZIP_LIBRARIES})

set(MPVulkanRendererLibrariesBase ${MPVanillaRendererLibraries})	# (the Vulkan module does not link OpenGL)
find_package(OpenGL REQUIRED)
set(MPVanillaRendererIncludeDirectories ${MPVanillaRendererIncludeDirectories} ${OPENGL_INCLUDE_DIR})
set(MPVanillaRendererLibraries ${MPVanillaRendererLibraries} ${OPENGL_LIBRARIES})

set(MPVanillaRendererIncludeDirectories ${MPVanillaRendererIncludeDirectories} ${OpenJKLibDir})
# the renderer is made twice, for Jedi Academy and for Jedi Outcast (JK2_MODE: the models of Outcast use their own skeleton)
function(add_mp_vanilla_renderer_project TargetName Defines IncludeDirectories)
	add_library(${TargetName} SHARED ${MPVanillaRendererFiles})

	if(NOT MSVC)
		# remove "lib" prefix for .so/.dylib files
		set_target_properties(${TargetName} PROPERTIES PREFIX "")
	endif()

	if(WIN32)
		install(TARGETS ${TargetName}
			RUNTIME
			DESTINATION ${InstallDir}
			COMPONENT ${JKAMPClientComponent})
	else(WIN32)
		install(TARGETS ${TargetName}
			LIBRARY
			DESTINATION ${InstallDir}
			COMPONENT ${JKAMPClientComponent})
	endif()

	set_target_properties(${TargetName} PROPERTIES COMPILE_DEFINITIONS "${Defines}")

	# Hide symbols not explicitly marked public.
	set_property(TARGET ${TargetName} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})

	set_target_properties(${TargetName} PROPERTIES INCLUDE_DIRECTORIES "${IncludeDirectories}")
	set_target_properties(${TargetName} PROPERTIES PROJECT_LABEL "MP Vanilla Renderer")
	target_link_libraries(${TargetName} ${MPVanillaRendererLibraries})
endfunction(add_mp_vanilla_renderer_project)

if(BuildMPRdVanilla)
	set(InstallDir ${JKAInstallDir})
	add_mp_vanilla_renderer_project(${MPVanillaRenderer} "${SharedDefines}" "${MPVanillaRendererIncludeDirectories}")
endif(BuildMPRdVanilla)
if(BuildJK2MPRdVanilla)
	set(InstallDir ${JK2InstallDir})
	add_mp_vanilla_renderer_project(${JK2MPVanillaRenderer} "${SharedDefines};JK2_MODE" "${MPVanillaRendererIncludeDirectories}")
endif(BuildJK2MPRdVanilla)

# The Vulkan renderer module: these same sources compiled with RD_VULKAN (and RD_VULKAN_MP), plus the Vulkan layer of
# the single player renderer in code/rd-vulkan, which builds against this renderer (vk_renderer.h). See
# docs/vulkan-renderer.md.
if(BuildMPRdVulkan OR BuildJK2MPRdVulkan)
	find_package(Vulkan)
	set(MPVulkanIncludeDir ${Vulkan_INCLUDE_DIRS})
	set(MPVulkanLibrary Vulkan::Vulkan)
	# MinGW (the Windows version built on Linux): an import library for vulkan-1.dll made from the list of what the
	# renderer calls, and a copy of the headers (as for the single player module, code/rd-vanilla/CMakeLists.txt)
	if(NOT Vulkan_FOUND AND MINGW AND CMAKE_DLLTOOL AND Vulkan_INCLUDE_DIR AND EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.h")
		set(MPVulkanImportLibrary "${CMAKE_CURRENT_BINARY_DIR}/libvulkan-1.dll.a")
		execute_process(COMMAND ${CMAKE_DLLTOOL} -m i386:x86-64 -d "${SPDir}/rd-vulkan/vulkan-1.def" -l "${MPVulkanImportLibrary}"
			RESULT_VARIABLE MPVulkanImportResult)
		if(MPVulkanImportResult EQUAL 0)
			file(REMOVE_RECURSE "${CMAKE_CURRENT_BINARY_DIR}/vulkan-include")
			file(COPY "${Vulkan_INCLUDE_DIR}/vulkan" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/vulkan-include")
			if(EXISTS "${Vulkan_INCLUDE_DIR}/vk_video")
				file(COPY "${Vulkan_INCLUDE_DIR}/vk_video" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/vulkan-include")
			endif()
			set(MPVulkanIncludeDir "${CMAKE_CURRENT_BINARY_DIR}/vulkan-include")
			set(MPVulkanLibrary "${MPVulkanImportLibrary}")
			set(Vulkan_FOUND TRUE)
		endif()
	endif()
	if(NOT Vulkan_FOUND)
		message(STATUS "Vulkan headers/loader not found, not building the MP Vulkan renderer module")
	else()
		# (tr_glsl.cpp and tr_post.cpp are the OpenGL versions of what vk_post.cpp does)
		set(MPVulkanRendererFiles ${MPVanillaRendererFiles})
		list(REMOVE_ITEM MPVulkanRendererFiles "${MPDir}/rd-vanilla/tr_glsl.cpp" "${MPDir}/rd-vanilla/tr_post.cpp")
		list(APPEND MPVulkanRendererFiles
			"${SPDir}/rd-vulkan/qgl.h"
			"${SPDir}/rd-vulkan/vk_local.h"
			"${SPDir}/rd-vulkan/vk_priv.h"
			"${SPDir}/rd-vulkan/vk_renderer.h"
			"${SPDir}/rd-vulkan/vk_spirv.h"
			"${SPDir}/rd-vulkan/vk_draw.cpp"
			"${SPDir}/rd-vulkan/vk_frame.cpp"
			"${SPDir}/rd-vulkan/vk_gl.cpp"
			"${SPDir}/rd-vulkan/vk_image.cpp"
			"${SPDir}/rd-vulkan/vk_init.cpp"
			"${SPDir}/rd-vulkan/vk_mem.cpp"
			"${SPDir}/rd-vulkan/vk_post.cpp"
			"${SPDir}/rd-vulkan/vk_rt.cpp"
			)
		set(MPVulkanRendererIncludeDirectories ${MPVanillaRendererIncludeDirectories} "${SPDir}/rd-vulkan" ${MPVulkanIncludeDir})
		list(REMOVE_ITEM MPVulkanRendererIncludeDirectories ${OPENGL_INCLUDE_DIR})

		function(add_mp_vulkan_renderer_project TargetName InstallDir Component Defines Label)
			add_library(${TargetName} SHARED ${MPVulkanRendererFiles})
			if(NOT MSVC)
				set_target_properties(${TargetName} PROPERTIES PREFIX "")
			endif()
			if(WIN32)
				install(TARGETS ${TargetName} RUNTIME DESTINATION ${InstallDir} COMPONENT ${Component})
			else(WIN32)
				install(TARGETS ${TargetName} LIBRARY DESTINATION ${InstallDir} COMPONENT ${Component})
			endif()
			set_target_properties(${TargetName} PROPERTIES COMPILE_DEFINITIONS "${Defines}")
			set_property(TARGET ${TargetName} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})
			set_target_properties(${TargetName} PROPERTIES INCLUDE_DIRECTORIES "${MPVulkanRendererIncludeDirectories}")
			set_target_properties(${TargetName} PROPERTIES PROJECT_LABEL ${Label})
			target_link_libraries(${TargetName} ${MPVulkanRendererLibrariesBase} ${MPVulkanLibrary})
		endfunction(add_mp_vulkan_renderer_project)

		if(BuildMPRdVulkan)
			add_mp_vulkan_renderer_project(${MPVulkanRenderer} ${JKAInstallDir} ${JKAMPClientComponent}
				"${SharedDefines};RD_VULKAN;RD_VULKAN_MP" "MP Vulkan Renderer")
		endif(BuildMPRdVulkan)
		if(BuildJK2MPRdVulkan)
			add_mp_vulkan_renderer_project(${JK2MPVulkanRenderer} ${JK2InstallDir} ${JKAMPClientComponent}
				"${SharedDefines};JK2_MODE;RD_VULKAN;RD_VULKAN_MP" "JK2 MP Vulkan Renderer")
		endif(BuildJK2MPRdVulkan)
	endif()
endif(BuildMPRdVulkan OR BuildJK2MPRdVulkan)

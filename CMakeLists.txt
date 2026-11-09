cmake_minimum_required(VERSION 3.21)

set(PLUGIN_NAME "SkyBuild")
set(PLUGIN_AUTHOR "Sokco816")

project(
    ${PLUGIN_NAME}
    VERSION 0.0.2
    LANGUAGES CXX
)

message(STATUS "Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "Pointer size: ${CMAKE_SIZEOF_VOID_P}")
message(STATUS "Vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")

set(
    GENERATED_PLUGIN_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Plugin.h"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Plugin.h.in"
    "${GENERATED_PLUGIN_HEADER}"
    @ONLY
)
# ------------------------------------------------------------
# Deployment options
# ------------------------------------------------------------

option(
    DEPLOY_TO_MODS
    "Copy the built plugin into the configured Skyrim/mod-manager folder"
    ON
)

# Prefer deploying directly into Skyrim's real Data folder.
if(
    DEFINED ENV{SKYRIM_FOLDER}
    AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data"
)
    set(
        OUTPUT_FOLDER
        "$ENV{SKYRIM_FOLDER}/Data"
    )
# Fall back to a Mod Organizer 2 or Vortex staging folder when the Skyrim
# installation itself is not configured.
elseif(
    DEFINED ENV{SKYRIM_MODS_FOLDER}
    AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}"
)
    set(
        OUTPUT_FOLDER
        "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}"
    )
endif()

# Alternatively, specify a deployment folder manually:
#
# set(OUTPUT_FOLDER "C:/Modding/ModOrganizer/mods/${PROJECT_NAME}")

# ------------------------------------------------------------
# Source files
# ------------------------------------------------------------

include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)

# ------------------------------------------------------------
# CommonLibSSE-NG
# ------------------------------------------------------------

if(
    NOT EXISTS
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG/CMakeLists.txt"
)
    message(
        FATAL_ERROR
        "CommonLibSSE-NG was not found. Run:\n"
        "git submodule update --init --recursive"
    )
endif()


# We only consume CommonLibSSE-NG; do not build its internal test suite.
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE-NG tests" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG"
    "${CMAKE_CURRENT_BINARY_DIR}/CommonLibSSE"
    EXCLUDE_FROM_ALL
)

find_package(lz4 CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)
find_package(recastnavigation CONFIG REQUIRED)
include(FetchContent)
FetchContent_Declare(nifly GIT_REPOSITORY https://github.com/ousnius/nifly.git
    GIT_TAG 40584be62399c64a3cb54fd2a68466e739932f39)
FetchContent_MakeAvailable(nifly)

add_library(SkyBuildMC33 STATIC extern/mc33/source/libMC33++.cpp)
target_compile_features(SkyBuildMC33 PUBLIC cxx_std_20)
target_compile_definitions(SkyBuildMC33 PUBLIC MC33_DOUBLE_PRECISION=1 MC33_NORMAL_NEG=1)

# A private ImGui context and its backends: no Menu Framework ABI or runtime dependency.
add_library(SkyBuildImGui STATIC
    extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp
    extern/imgui/backends/imgui_impl_win32.cpp
    extern/imgui/backends/imgui_impl_dx11.cpp
    extern/ImAnim/im_anim.cpp)
target_include_directories(SkyBuildImGui PUBLIC extern/imgui extern/imgui/backends extern/ImAnim)
target_compile_features(SkyBuildImGui PRIVATE cxx_std_20)
target_compile_definitions(SkyBuildImGui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN SKYBUILD_RELATIVE_MOUSE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_link_libraries(SkyBuildImGui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

# Defines add_commonlibsse_plugin().
include(
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake"
)

# ------------------------------------------------------------
# Plugin target
# ------------------------------------------------------------

add_commonlibsse_plugin(
    ${PROJECT_NAME}
    NAME "${PLUGIN_NAME}"
    VERSION "${PROJECT_VERSION}"
    MINIMUM_SKSE_VERSION "2.0.20"
    SOURCES
        ${headers}
        ${sources}
        "${GENERATED_PLUGIN_HEADER}"
)

target_compile_features(
    ${PROJECT_NAME}
    PRIVATE
        cxx_std_23
)

target_precompile_headers(
    ${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h"
)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        lz4::lz4
        minhook::minhook
        RecastNavigation::Recast
        SkyBuildMC33
        SkyBuildImGui
        Microsoft::DirectXTK
        nifly
        d2d1
        ole32
        shell32
        uuid
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/mc33/LICENSE.txt"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyBuild-MC33-LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/LICENSE.txt"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyBuild-ImGui-LICENSE.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/ImAnim/LICENSE"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyBuild-ImAnim-LICENSE.txt"
    VERBATIM
)

# Collect source and linked-dependency notices next to the build artifact.
set(SKYBUILD_LICENSE_FOLDER "${CMAKE_CURRENT_BINARY_DIR}/SkyBuild-licenses")
file(MAKE_DIRECTORY "${SKYBUILD_LICENSE_FOLDER}")
configure_file(extern/CommonLibSSE-NG/COPYING.txt "${SKYBUILD_LICENSE_FOLDER}/CommonLibSSE-NG-COPYING.txt" COPYONLY)
file(COPY extern/CommonLibSSE-NG/licenses/ DESTINATION "${SKYBUILD_LICENSE_FOLDER}/CommonLibSSE-NG")
configure_file(extern/mc33/LICENSE.txt "${SKYBUILD_LICENSE_FOLDER}/MC33.txt" COPYONLY)
configure_file("${nifly_SOURCE_DIR}/LICENSE" "${SKYBUILD_LICENSE_FOLDER}/Nifly.txt" COPYONLY)
configure_file(extern/imgui/LICENSE.txt "${SKYBUILD_LICENSE_FOLDER}/Dear-ImGui.txt" COPYONLY)
configure_file(extern/ImAnim/LICENSE "${SKYBUILD_LICENSE_FOLDER}/ImAnim.txt" COPYONLY)
configure_file(src/map-markers/THIRD_PARTY.md "${SKYBUILD_LICENSE_FOLDER}/MapMarkerFramework.txt" COPYONLY)
file(GLOB SKYBUILD_DEPENDENCY_NOTICES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
foreach(notice IN LISTS SKYBUILD_DEPENDENCY_NOTICES)
    get_filename_component(package_directory "${notice}" DIRECTORY)
    get_filename_component(package_name "${package_directory}" NAME)
    configure_file("${notice}" "${SKYBUILD_LICENSE_FOLDER}/${package_name}.txt" COPYONLY)
endforeach()
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SKYBUILD_LICENSE_FOLDER}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyBuild-licenses"
    VERBATIM)

# ------------------------------------------------------------
# Compiler settings
# ------------------------------------------------------------

if(MSVC)
    target_compile_options(SkyBuildMC33 PRIVATE /EHsc)
    target_compile_options(
        ${PROJECT_NAME}
        PRIVATE
            /EHsc
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
            /W4
    )
endif()

# ------------------------------------------------------------
# Deployment
# ------------------------------------------------------------

if(DEFINED OUTPUT_FOLDER AND DEPLOY_TO_MODS)
    set(
        DLL_FOLDER
        "${OUTPUT_FOLDER}/SKSE/Plugins"
    )

    set(
        NATIVE_UI_SOURCE_FOLDER
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/SkyBuild/assets"
    )

    set(
        NATIVE_UI_OUTPUT_FOLDER
        "${OUTPUT_FOLDER}/SkyBuild/UI/assets"
    )

    set(
        CONFIG_FOLDER
        "${PROJECT_NAME}"
    )

    set(
        SOURCE_CONFIG_FOLDER
        "${CMAKE_CURRENT_SOURCE_DIR}/config"
    )

    message(
        STATUS
        "Plugin deployment folder: ${DLL_FOLDER}"
    )

    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD

        COMMAND
            "${CMAKE_COMMAND}"
            -E make_directory
            "${DLL_FOLDER}"
            "${OUTPUT_FOLDER}/SkyBuild"

        COMMAND
            "${CMAKE_COMMAND}"
            -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"

        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/extern/mc33/LICENSE.txt"
            "${OUTPUT_FOLDER}/SkyBuild/SkyBuild-MC33-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/LICENSE.txt"
            "${OUTPUT_FOLDER}/SkyBuild/SkyBuild-ImGui-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/extern/ImAnim/LICENSE"
            "${OUTPUT_FOLDER}/SkyBuild/SkyBuild-ImAnim-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${SKYBUILD_LICENSE_FOLDER}"
            "${OUTPUT_FOLDER}/SkyBuild/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/src/map-markers/THIRD_PARTY.md"
            "${OUTPUT_FOLDER}/SkyBuild/SkyBuild-MapMarkerFramework-LICENSE.txt"

        VERBATIM
    )

    # Copy config files to:
    #
    # SkyBuild/
    #
    # This only runs when the config directory exists.
    if(EXISTS "${SOURCE_CONFIG_FOLDER}")
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD

            COMMAND
                "${CMAKE_COMMAND}"
                -E copy_directory
                "${SOURCE_CONFIG_FOLDER}"
                "${OUTPUT_FOLDER}/${CONFIG_FOLDER}"

            VERBATIM
        )
    endif()

    # Inventory Injector rules deploy independently of the plugin DLL.
    add_custom_target(
        DeployInventoryInjector ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${DLL_FOLDER}/InventoryInjector"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/inventory-injector/SkyBuild.json"
            "${DLL_FOLDER}/InventoryInjector/SkyBuild.json"
        COMMENT "Deploying Inventory Injector hammer icon rule"
        VERBATIM
    )

    # Native ImGui textures; no browser entrypoints or scripts are deployed.
    if(EXISTS "${NATIVE_UI_SOURCE_FOLDER}")
        add_custom_target(DeployUIAssets ALL
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${NATIVE_UI_OUTPUT_FOLDER}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${NATIVE_UI_SOURCE_FOLDER}" "${NATIVE_UI_OUTPUT_FOLDER}"
            COMMENT "Deploying native UI assets"
            VERBATIM)
        add_dependencies(DeployUIAssets ${PROJECT_NAME})
    endif()

    # Copy debugging symbols into the mod output for Debug builds only.
    # Release symbols remain available in the CMake build directory.
    if(MSVC)
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD

            COMMAND
                "$<$<CONFIG:Debug>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_PDB_FILE:${PROJECT_NAME}>;${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>>"

            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
else()
    message(
        STATUS
        "Automatic plugin deployment is disabled or no output folder is configured."
    )
endif()

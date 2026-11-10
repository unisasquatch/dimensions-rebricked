cmake_minimum_required(VERSION 3.24)
project(LegoDimensionsPort LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED COMPONENTS shaderc_combined)
if(WIN32)
  find_package(SDL3 CONFIG REQUIRED PATHS "${CMAKE_SOURCE_DIR}/../tools/SDL3-3.4.18/cmake")
else()
  find_package(SDL3 CONFIG REQUIRED)  # built from source by deck/build_linux.sh
endif()

add_executable(ldport
    src/main.cpp
    src/loader/orbis_elf.cpp
    src/loader/nid.cpp
    src/loader/game_patches.cpp
    src/host/memory.cpp
    src/host/thread_tls.cpp
    src/host/guest_memory.cpp
    src/host/shadps4_import.cpp
    src/host/save_transfer.cpp
    src/hle/registry.cpp
    src/hle/sysv_va.cpp
    src/hle/libc_core.cpp
    src/hle/libc_stdio.cpp
    src/hle/libc_cxx.cpp
    src/hle/libkernel.cpp
    src/hle/dialogs.cpp
    src/hle/port_settings.cpp
    src/hle/options_menu.cpp
    src/ui/overlay.cpp
    src/ui/settings_items.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
    src/hle/toypad_web.cpp
    src/hle/clock.cpp
    src/hle/libkernel_thread.cpp
    src/hle/system_services.cpp
    src/hle/offline_stubs.cpp
    src/hle/libfios2.cpp
    src/hle/libscepad.cpp
    src/hle/keyboard_mouse.cpp
    src/hle/button_icons.cpp
    src/hle/libsceusbd.cpp
    src/hle/toypad.cpp
    src/hle/toypad_usb.cpp
    src/hle/toypad_source.cpp
    src/hle/libsceaudioout.cpp
    src/hle/libscengs2.cpp
    src/hle/equeue.cpp
    src/hle/libscevideoout.cpp
    src/hle/libscegnmdriver.cpp
    src/gpu/command_processor.cpp
    src/gpu/capture.cpp
    src/gpu/tiling.cpp
    src/gpu/vk/device.cpp
    src/gpu/vk/dlss.cpp
    src/gpu/vk/fsr.cpp
    src/gpu/vk/formats.cpp
    src/gpu/vk/renderer.cpp
    src/gpu/gcn/decoder.cpp
    src/gpu/gcn/translator.cpp
)
target_compile_definitions(ldport PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_include_directories(ldport PRIVATE src third_party/imgui third_party/fsr1 third_party/fsr2/include)
# AMD FidelityFX FSR 2.2.1 (MIT), Vulkan backend.
if(WIN32)
  # Built by build_fsr2.cmd into third_party/fsr2/lib.
  target_link_libraries(ldport PRIVATE ${CMAKE_SOURCE_DIR}/third_party/fsr2/lib/ffx_fsr2_api_vk_x64.lib
                                       ${CMAKE_SOURCE_DIR}/third_party/fsr2/lib/ffx_fsr2_api_x64.lib)
else()
  # Linux (Steam Deck): compiled from third_party/fsr2/linux, with the SPIR-V headers the Windows build generated.
  set(FSR2_LINUX ${CMAKE_SOURCE_DIR}/third_party/fsr2/linux)
  add_library(ffx_fsr2_vk STATIC
      ${FSR2_LINUX}/src/ffx_assert.cpp
      ${FSR2_LINUX}/src/ffx_fsr2.cpp
      ${FSR2_LINUX}/src/vk/ffx_fsr2_vk.cpp
      ${FSR2_LINUX}/src/vk/shaders/ffx_fsr2_shaders_vk.cpp)
  target_include_directories(ffx_fsr2_vk PRIVATE ${FSR2_LINUX}/src ${FSR2_LINUX}/src/vk ${FSR2_LINUX}/gen)
  target_compile_options(ffx_fsr2_vk PRIVATE -include ${FSR2_LINUX}/msvc_shim.h -w)
  target_link_libraries(ffx_fsr2_vk PRIVATE Vulkan::Vulkan)
  target_link_libraries(ldport PRIVATE ffx_fsr2_vk)
endif()
target_link_libraries(ldport PRIVATE SDL3::SDL3 Vulkan::Vulkan Vulkan::shaderc_combined)
# NVIDIA DLSS (NGX SDK v310.9.16, NVIDIA RTX SDKs licence): headers, the loader library, and nvngx_dlss.dll next to
# the executable (NGX loads it from there).
if(WIN32)
  set(DLSS_SDK ${CMAKE_SOURCE_DIR}/third_party/dlss)
  target_include_directories(ldport PRIVATE ${DLSS_SDK}/include)
  target_link_libraries(ldport PRIVATE ${DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib)
  add_custom_command(TARGET ldport POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll $<TARGET_FILE_DIR:ldport>)
endif()
if(WIN32)
  target_link_libraries(ldport PRIVATE ws2_32)
else()
  target_link_libraries(ldport PRIVATE pthread dl)
  # Find SDL3 next to the executable in the Steam Deck package.
  set_target_properties(ldport PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
add_custom_command(TARGET ldport POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:ldport>)
# AMD FidelityFX FSR 1 shader headers (MIT): compiled at start-up by the presenter.
add_custom_command(TARGET ldport POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/third_party/fsr1 $<TARGET_FILE_DIR:ldport>/fsr1)

# Shader disassembler for captured shaders.
add_executable(gcndis src/tools/gcndis.cpp src/gpu/gcn/decoder.cpp)
target_include_directories(gcndis PRIVATE src)

# Offline shader translation check: gcn2glsl <vs|ps> shader.bin [fetch.bin] > out.glsl
add_executable(gcn2glsl src/tools/gcn2glsl.cpp src/gpu/gcn/translator.cpp src/gpu/gcn/decoder.cpp)
target_include_directories(gcn2glsl PRIVATE src)

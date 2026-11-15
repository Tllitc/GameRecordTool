# ============================================================================
# GameRecordTool —— OBS Studio 32.2.2 原生前端插件工程骨架
# ----------------------------------------------------------------------------
# 依赖说明（构建前请确认以下产物已就位）：
#   1. OBS 开发文件（头文件 + 导入库），默认位于 third_party/obs-dev：
#        include/libobs/            —— libobs 公共头文件（含手工生成的 obsconfig.h）
#        include/obs-frontend-api/  —— 前端 API 头文件
#        lib/obs.lib                —— 由便携版 obs.dll 导出表生成的导入库
#        lib/obs-frontend-api.lib   —— 由 obs-frontend-api.dll 导出表生成的导入库
#      可通过 -DOBS_DEV_DIR=<路径> 覆盖。
#   2. Qt 6.11.1（win64_msvc2022_64，至少包含 qtbase / Widgets 模块），
#      默认位于 third_party/Qt/6.11.1/msvc2022_64（该目录下需有 lib/cmake/Qt6），
#      可通过 -DQT_DIR=<路径> 覆盖。
#   3. 工具链：Visual Studio 2022（MSVC 14.37+）、CMake 3.21+，语言标准 C++20。
# 构建命令：
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
# 构建产物：build/Release/game_record_tool.dll
#   部署时复制到便携版 OBS 的 obs-plugins/64bit 目录（见 scripts/deploy.ps1）。
# ============================================================================

cmake_minimum_required(VERSION 3.21)

project(GameRecordTool LANGUAGES CXX)

# ---- 依赖路径变量（均可在命令行通过 -D 覆盖）------------------------------
set(OBS_DEV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-dev"
    CACHE PATH "OBS 开发文件根目录（包含 include/ 与 lib/）")
set(QT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Qt/6.11.1/msvc2022_64"
    CACHE PATH "Qt 6 msvc2022_64 安装目录（包含 lib/cmake/Qt6）")

# 供 find_package(Qt6) 定位 QtConfig
list(APPEND CMAKE_PREFIX_PATH "${QT_DIR}")

# ---- 全局编译选项 -----------------------------------------------------------
# 统一使用 C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 与 OBS 官方二进制保持一致，使用动态运行库（Release: /MD，Debug: /MDd）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# 本任务只需链接 Qt6 Widgets
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ---- 可配置选项 -------------------------------------------------------------
# 打开后 GRT_LOG_DEBUG 宏实际输出（默认关闭，Release 不产生调试日志代码）
option(GRT_VERBOSE_LOG "启用 GameRecordTool DEBUG 级别详细日志" OFF)

# ---- 插件 DLL 目标 ----------------------------------------------------------
# 注意：源文件显式列出，不使用 file(GLOB)
add_library(game_record_tool SHARED
    src/plugin/plugin-main.cpp
    src/core/clock.cpp
    src/core/log.cpp
    src/core/plugin_config.cpp
    src/input/raw_input_service.cpp
    src/input/mouse_accel_service.cpp
    src/input/key_names.cpp
    src/input/global_hotkey_manager.cpp
    src/video/video_time_calibrator.cpp
    src/video/offset_estimator.cpp
    src/capture/window_enumerator.cpp
    src/capture/game_capture_scene.cpp
    src/capture/output_configurator.cpp
    src/session/spool_writer.cpp
    src/session/movement_tracker.cpp
    src/session/looking_aggregator.cpp
    src/session/action_mapper.cpp
    src/session/session_manager.cpp
    src/export/export_pipeline.cpp
    src/verify/session_verifier.cpp
    src/ui/dock_controller.cpp
    src/ui/main_panel.cpp
    src/ui/async_verify_runner.cpp
    src/ui/settings_panel.cpp
    src/ui/history_panel.cpp
    src/ui/history_store.cpp
    src/ui/overlay_controller.cpp
    src/ui/keymap_panel.cpp
)

# 含 Q_OBJECT 的 UI 头文件需要 moc 元对象代码（AUTOMOC 自动扫描）
set_target_properties(game_record_tool PROPERTIES AUTOMOC ON)

# 头文件包含目录：
#   - src：插件内部头文件（以 "core/xxx.h" 形式包含）
#   - OBS 开发头文件（obs-frontend-api.h 内部会 #include <obs.h>）
target_include_directories(game_record_tool PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${OBS_DEV_DIR}/include/libobs"
    "${OBS_DEV_DIR}/include/obs-frontend-api"
)

# shell32：会话回滚时用 SHFileOperationW 静默删除空会话目录
target_link_libraries(game_record_tool PRIVATE
    "${OBS_DEV_DIR}/lib/obs.lib"
    "${OBS_DEV_DIR}/lib/obs-frontend-api.lib"
    Qt6::Widgets
    Qt6::Gui
    Qt6::Core
    user32
    gdi32
    shell32
)

# 使用 Unicode 宏，与 OBS / Windows TCHAR 约定一致
target_compile_definitions(game_record_tool PRIVATE
    UNICODE
    _UNICODE
)

# GRT_VERBOSE_LOG 打开时编译 GRT_LOG_DEBUG 实际输出
if (GRT_VERBOSE_LOG)
    target_compile_definitions(game_record_tool PRIVATE GRT_VERBOSE_LOG)
endif()

if (MSVC)
    # /utf-8：源码与执行字符集均为 UTF-8（日志中的中文依赖此项）
    # /EHsc：启用 C++ 标准异常模型（OBS 头文件需要）
    # /Zc:__cplusplus：让 __cplusplus 宏报告真实标准版本（Qt6 头文件强制要求）
    target_compile_options(game_record_tool PRIVATE /utf-8 /EHsc /Zc:__cplusplus)
endif()

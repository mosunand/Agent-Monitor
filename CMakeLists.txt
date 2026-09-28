cmake_minimum_required(VERSION 3.21)
project(agent-monitor LANGUAGES CXX)

# ── 构建环境完全来自环境变量（PATH / CMAKE_PREFIX_PATH），不硬编码任何路径 ──

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Concurrent)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(agent-monitor WIN32
    src/main.cpp
    resources.rc
    agent-monitor.ico
    # util
    src/util/Format.h
    src/util/Format.cpp
    src/util/Async.h
    src/util/Frost.h
    src/util/Frost.cpp
    src/util/CrashHandler.h
    src/util/CrashHandler.cpp
    # core
    src/core/Types.h
    src/core/Paths.h
    src/core/Paths.cpp
    src/core/AgentAdapter.h
    src/core/AgentAdapter.cpp
    src/core/CodexAdapter.h
    src/core/CodexAdapter.cpp
    src/core/ClaudeAdapter.h
    src/core/ClaudeAdapter.cpp
    src/core/GeminiLikeAdapter.h
    src/core/GeminiLikeAdapter.cpp
    src/core/OpenCodeAdapter.h
    src/core/OpenCodeAdapter.cpp
    src/core/PiAdapter.h
    src/core/PiAdapter.cpp
    src/core/ZCodeAdapter.h
    src/core/ZCodeAdapter.cpp
    src/core/Store.h
    src/core/Store.cpp
    # ui shared
    src/ui/Theme.h
    src/ui/Theme.cpp
    src/ui/UiUtil.h
    src/ui/UiUtil.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    # widgets
    src/ui/widgets/KpiCard.h
    src/ui/widgets/KpiCard.cpp
    src/ui/widgets/MiniChart.h
    src/ui/widgets/MiniChart.cpp
    src/ui/widgets/LiveFeed.h
    src/ui/widgets/LiveFeed.cpp
    src/ui/widgets/UsageHeatmap.h
    src/ui/widgets/UsageHeatmap.cpp
    src/ui/widgets/UsageDonut.h
    src/ui/widgets/UsageDonut.cpp
    # pages
    src/ui/pages/OverviewPage.h
    src/ui/pages/OverviewPage.cpp
    src/ui/pages/UsageStatsPage.h
    src/ui/pages/UsageStatsPage.cpp
    src/ui/pages/SessionsPage.h
    src/ui/pages/SessionsPage.cpp
    src/ui/pages/ModelsPage.h
    src/ui/pages/ModelsPage.cpp
    src/ui/pages/RawPage.h
    src/ui/pages/RawPage.cpp
    src/ui/pages/SourcesPage.h
    src/ui/pages/SourcesPage.cpp
    # selftest
    src/selftest/SelfTest.h
    src/selftest/SelfTest.cpp
)

target_link_libraries(agent-monitor PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
    dwmapi
)

target_include_directories(agent-monitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

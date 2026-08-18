cmake_minimum_required(VERSION 3.20)

project(hprp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HPRP_ENABLE_SANITIZERS "Build with address/undefined sanitizers" OFF)

add_executable(hprp
  src/main.cpp
  src/config.cpp
  src/http_utils.cpp
  src/metrics.cpp
  src/backend_pool.cpp
  src/health_checker.cpp
  src/metrics_server.cpp
  src/proxy_server.cpp
)

target_include_directories(hprp PRIVATE include)
target_compile_options(hprp PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_link_libraries(hprp PRIVATE pthread)

if(HPRP_ENABLE_SANITIZERS)
  target_compile_options(hprp PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(hprp PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME smoke_integration
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/smoke_test.py --binary $<TARGET_FILE:hprp>
)

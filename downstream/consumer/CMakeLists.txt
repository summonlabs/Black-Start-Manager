cmake_minimum_required(VERSION 3.21)

# Independent out-of-tree consumer of the installed Black Start Manager package. This
# project is not part of the Black Start Manager build tree: it is configured separately
# with CMAKE_PREFIX_PATH pointing at an install prefix.

project(BlackStartManagerConsumer
  VERSION 1.0.0
  DESCRIPTION "Out-of-tree consumer of the installed Black Start Manager package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(BlackStartManager 1.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE BlackStartManager::black_start_manager)

if(MSVC)
  target_compile_options(consumer PRIVATE /W4 /permissive- /utf-8 /WX)
else()
  target_compile_options(consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Runs the consumer through a real minimal lifecycle against the installed artifact: open
# a durable store, establish a session, record readiness evidence, assess, verify, close.
add_custom_target(run_consumer
  COMMAND consumer "${CMAKE_CURRENT_BINARY_DIR}/consumer-store"
  DEPENDS consumer
  COMMENT "Running the installed-package consumer lifecycle"
  VERBATIM)

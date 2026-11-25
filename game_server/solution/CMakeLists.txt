cmake_minimum_required(VERSION 3.11)

project(game_server CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

find_package(Boost 1.78.0 REQUIRED)
if(Boost_FOUND)
  include_directories(${Boost_INCLUDE_DIRS})
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Статическая библиотека с игровой моделью.
# Собирается один раз и используется и сервером, и тестами.
add_library(game_model STATIC
    src/model.h
    src/model.cpp
    src/geom.h
    src/collision_detector.h
    src/collision_detector.cpp
    src/loot_generator.h
    src/loot_generator.cpp
    src/game_state.h
    src/game_state.cpp
    src/tagged.h
)
# Заголовки модели видны всем, кто линкуется с библиотекой.
target_include_directories(game_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Модель использует std::mutex, поэтому Threads нужен и зависимым целям.
target_link_libraries(game_model PUBLIC Threads::Threads)

# Сервер: только сетевой код, загрузка конфигурации и точка входа.
add_executable(game_server
    src/main.cpp
    src/http_server.cpp
    src/http_server.h
    src/sdk.h
    src/extra_data.h
    src/ticker.h
    src/boost_json.cpp
    src/json_loader.h
    src/json_loader.cpp
    src/request_handler.h
    src/logger.h
    src/logger.cpp
    src/records.h
    src/records_db.h
    src/records_db.cpp
)
target_link_libraries(game_server PRIVATE game_model Threads::Threads CONAN_PKG::boost CONAN_PKG::libpqxx)

# Тесты: только собственные файлы.
add_executable(game_server_tests
    tests/model-tests.cpp
    tests/loot_generator_tests.cpp
    tests/state-tests.cpp
    tests/retirement-tests.cpp
)
target_link_libraries(game_server_tests PRIVATE game_model CONAN_PKG::catch2)

# Интеграция с CTest.
include(CTest)
include(${CONAN_BUILD_DIRS_CATCH2}/Catch.cmake)
catch_discover_tests(game_server_tests)

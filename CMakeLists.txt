cmake_minimum_required(VERSION 3.20)
project(catan_bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(catan
    src/Board.cpp
    src/Game.cpp
    src/Model.cpp
    src/Rules.cpp
)
target_include_directories(catan PUBLIC include)
target_compile_options(catan PRIVATE -Wall -Wextra -Wpedantic)

find_package(Python3 QUIET COMPONENTS Interpreter)
find_program(NODE_EXECUTABLE node)
if(Python3_Interpreter_FOUND)
    set(CATAN_PYTHON "${Python3_EXECUTABLE}")
else()
    set(CATAN_PYTHON "python3")
endif()

add_library(catan_mcts
    src/Belief.cpp
    src/Mcts.cpp
)
target_include_directories(catan_mcts PUBLIC include)
target_link_libraries(catan_mcts PUBLIC catan)
target_compile_options(catan_mcts PRIVATE -Wall -Wextra -Wpedantic)

add_library(catan_debug src/Debug.cpp)
target_include_directories(catan_debug PUBLIC include)
target_link_libraries(catan_debug PUBLIC catan_mcts)
target_compile_options(catan_debug PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(catan_debug PRIVATE
    CATAN_RENDER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tools/render_state.py"
    CATAN_PYTHON_EXECUTABLE="${CATAN_PYTHON}"
)

add_executable(catan_cli src/main.cpp)
target_link_libraries(catan_cli PRIVATE catan_debug)

enable_testing()
add_executable(catan_tests
    tests/TestMain.cpp
    tests/BoardAndSetupTests.cpp
    tests/RankedAndRobberTests.cpp
    tests/EconomyAndBuildingTests.cpp
    tests/DevelopmentAndAwardsTests.cpp
    tests/BalancedDiceTests.cpp
    tests/BeliefTests.cpp
    tests/IntegrationTests.cpp
    tests/MctsTests.cpp
)
target_link_libraries(catan_tests PRIVATE catan_mcts)
target_compile_options(catan_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME catan_tests COMMAND catan_tests)

add_executable(catan_full_game_system_test tests/FullGameSystemTest.cpp)
target_link_libraries(catan_full_game_system_test PRIVATE catan)
target_compile_options(catan_full_game_system_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME full_game_system COMMAND catan_full_game_system_test)
set_tests_properties(full_game_system PROPERTIES LABELS system TIMEOUT 30)

add_executable(catan_snapshot tests/WriteSnapshot.cpp)
target_link_libraries(catan_snapshot PRIVATE catan_debug)
add_test(NAME debug_snapshot
    COMMAND catan_snapshot "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.json")
if(Python3_Interpreter_FOUND)
    add_test(NAME debug_renderer
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/render_state.py"
                --input "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.json"
                --output "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.svg")
    set_tests_properties(debug_renderer PROPERTIES DEPENDS debug_snapshot)
    add_test(NAME debug_renderer_ports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyRenderedPorts.py"
                "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.json"
                "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.svg")
    set_tests_properties(debug_renderer_ports PROPERTIES DEPENDS debug_renderer)
    add_test(NAME debug_renderer_mcts
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyRenderedMcts.py"
                "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.json"
                "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.svg")
    set_tests_properties(debug_renderer_mcts PROPERTIES DEPENDS debug_renderer)
    add_test(NAME stepper_random_chance_option
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyStepperRandom.py"
                "$<TARGET_FILE:catan_cli>")
    add_test(NAME live_viewer_controls
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyLiveViewerControls.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/render_state.py"
                "${CMAKE_CURRENT_BINARY_DIR}/debug_state_test.json")
    set_tests_properties(live_viewer_controls PROPERTIES DEPENDS debug_snapshot)
endif()
if(NODE_EXECUTABLE)
    add_test(NAME colonist_socket_capture_decoder
        COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/VerifyColonistSocketCapture.mjs")
endif()

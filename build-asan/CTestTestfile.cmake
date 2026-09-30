# CMake generated Testfile for 
# Source directory: /Users/nathan.ellis/Documents/catan-bot
# Build directory: /Users/nathan.ellis/Documents/catan-bot/build-asan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[catan_tests]=] "/Users/nathan.ellis/Documents/catan-bot/build-asan/catan_tests")
set_tests_properties([=[catan_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;58;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")
add_test([=[full_game_system]=] "/Users/nathan.ellis/Documents/catan-bot/build-asan/catan_full_game_system_test")
set_tests_properties([=[full_game_system]=] PROPERTIES  LABELS "system" TIMEOUT "30" _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;63;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")
add_test([=[debug_snapshot]=] "/Users/nathan.ellis/Documents/catan-bot/build-asan/catan_snapshot" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.json")
set_tests_properties([=[debug_snapshot]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;68;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")
add_test([=[debug_renderer]=] "/opt/homebrew/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "/Users/nathan.ellis/Documents/catan-bot/tools/render_state.py" "--input" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.json" "--output" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.svg")
set_tests_properties([=[debug_renderer]=] PROPERTIES  DEPENDS "debug_snapshot" _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;71;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")
add_test([=[debug_renderer_ports]=] "/opt/homebrew/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "/Users/nathan.ellis/Documents/catan-bot/tests/VerifyRenderedPorts.py" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.json" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.svg")
set_tests_properties([=[debug_renderer_ports]=] PROPERTIES  DEPENDS "debug_renderer" _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;76;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")
add_test([=[debug_renderer_mcts]=] "/opt/homebrew/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "/Users/nathan.ellis/Documents/catan-bot/tests/VerifyRenderedMcts.py" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.json" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.svg")
set_tests_properties([=[debug_renderer_mcts]=] PROPERTIES  DEPENDS "debug_renderer" _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;81;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")
add_test([=[stepper_random_chance_option]=] "/opt/homebrew/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "/Users/nathan.ellis/Documents/catan-bot/tests/VerifyStepperRandom.py" "/Users/nathan.ellis/Documents/catan-bot/build-asan/catan_cli")
set_tests_properties([=[stepper_random_chance_option]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;86;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")
add_test([=[live_viewer_controls]=] "/opt/homebrew/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "/Users/nathan.ellis/Documents/catan-bot/tests/VerifyLiveViewerControls.py" "/Users/nathan.ellis/Documents/catan-bot/tools/render_state.py" "/Users/nathan.ellis/Documents/catan-bot/build-asan/debug_state_test.json")
set_tests_properties([=[live_viewer_controls]=] PROPERTIES  DEPENDS "debug_snapshot" _BACKTRACE_TRIPLES "/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;89;add_test;/Users/nathan.ellis/Documents/catan-bot/CMakeLists.txt;0;")

cmake_minimum_required(VERSION 3.28)

# Standalone JSONL log generator. Lives in the test tree (sibling of
# `test/lib` and `test/app`) because its only consumers are local fixture
# generation and manual streaming-mode smoke tests; it is intentionally not
# wired into CTest.
add_executable(log_generator src/main.cpp)

target_link_libraries(log_generator PRIVATE test_common argparse project_warnings)

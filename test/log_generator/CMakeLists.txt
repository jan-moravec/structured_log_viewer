cmake_minimum_required(VERSION 3.28)

# Standalone multi-format log generator. Lives in the test tree
# (sibling of `test/lib` and `test/app`) because its only consumers
# are local fixture generation and manual streaming-mode smoke tests;
# intentionally not wired into CTest.
#
# `loglib` link is only needed for `BuiltinRegexTemplates()` lookups
# powering `--regex-template <NAME>` and `--list-regex-templates`.
# Other formats route entirely through `test_common::LogFormat`.
add_executable(log_generator src/main.cpp)

target_link_libraries(log_generator PRIVATE test_common loglib argparse project_warnings)

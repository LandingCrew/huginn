// Entry point of huginn_core_tests. doctest's main returns non-zero when any
// test fails, which is what ctest and a CI-like caller check.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

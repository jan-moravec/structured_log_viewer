// Stub TU that exists solely to anchor compile flags for the sibling header
// `loglib/internal/theme_glaze_meta.hpp`. See the matching stub for
// `log_configuration_glaze_meta.cpp` for the full rationale (clang-tidy on
// PR-changed headers needs the basename in the compile database to infer
// include paths). The TU intentionally has no runtime content.
#include "loglib/internal/theme_glaze_meta.hpp"

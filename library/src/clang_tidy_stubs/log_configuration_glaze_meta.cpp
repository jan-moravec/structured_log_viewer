// Stub TU that exists solely to anchor compile flags for the sibling header
// `loglib/internal/log_configuration_glaze_meta.hpp`. Without a `.cpp` with
// the same basename, clang-tidy (as invoked by `cpp-linter-action` on
// PR-changed headers in `.github/workflows/build.yml`) cannot infer include
// paths and reports a spurious
// `clang-diagnostic-error: 'loglib/log_configuration.hpp' file not found`.
// Registering this file places the header's basename in the compile database
// with the full loglib + glaze include set, so the inference succeeds. The
// TU intentionally has no runtime content.
#include "loglib/internal/log_configuration_glaze_meta.hpp"

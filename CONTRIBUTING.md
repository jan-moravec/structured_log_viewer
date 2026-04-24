# Contributing

Thanks for your interest in contributing! This document covers building from source, running tests, code style, and the release process.

## Prerequisites

- **CMake** 3.19 or newer
- **Qt** 6.1 or newer (Qt 6.8 is used in CI)
- A C++23-capable toolchain:
  - Linux: GCC 13+ or Clang 17+
  - Windows: MSVC 2022 (Visual Studio 2022)
  - macOS: Xcode 15+ / Apple Clang

Most third-party C++ dependencies (`date`, `fmt`, `Catch2`, `mio`, `glaze`, `simdjson`, `robin-map`) are fetched automatically via `FetchContent`. To use system copies instead, pass e.g. `-DUSE_SYSTEM_FMT=ON` when configuring.

## Building

### Linux / macOS

```sh
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
```

### Windows

Open the **Developer PowerShell for VS 2022** or Developer Command Prompt and run:

```powershell
mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

Qt Creator works out of the box with the top-level `CMakeLists.txt`.

## Running tests

From the `build/` directory:

```sh
ctest --output-on-failure -C Release
```

On headless environments (CI, remote VMs), set the Qt platform to offscreen first:

```sh
export QT_QPA_PLATFORM=offscreen   # Linux / macOS
$env:QT_QPA_PLATFORM = "offscreen" # Windows PowerShell
```

The suite contains:

- `tests` – `loglib` unit tests and a JSON parser benchmark (Catch2).
- `apptest` – Qt Test-based smoke tests for `MainWindow`.

## Code style and pre-commit

All C/C++ sources are formatted with **clang-format** and CMake files with **cmake-format**. These are enforced via pre-commit hooks:

```sh
pip install pre-commit
pre-commit install
```

Manually format the whole tree:

```sh
pre-commit run --all-files
```

The pinned tool versions live in [`.pre-commit-config.yaml`](.pre-commit-config.yaml).

## Pull requests

1. Fork the repository and create a topic branch from `main`.
1. Keep commits focused; rebase onto `main` before opening a PR.
1. Ensure `pre-commit run --all-files` passes locally.
1. Make sure CI (Linux, Windows, macOS) is green — tests and packaging both must succeed.
1. Describe the motivation and user-visible changes in the PR description.

## Release process

Releases are cut from tags matching `v*` (e.g. `v0.7.0`). The [`Build` workflow](.github/workflows/build.yml) detects tag pushes, packages artifacts for all platforms, and attaches them to a GitHub Release.

### Steps

1. Make sure `main` is green and all desired changes are merged.

1. Bump the version in the top-level [`CMakeLists.txt`](CMakeLists.txt):

   ```cmake
   project(
       structured_log_viewer
       VERSION 0.7.0
       LANGUAGES CXX)
   ```

1. Commit the bump and open a PR; merge once CI passes.

1. Tag the merge commit and push the tag:

   ```sh
   git checkout main
   git pull
   git tag -a v0.7.0 -m "Release v0.7.0"
   git push origin v0.7.0
   ```

1. The `Build` workflow will run on the tag and, on success, attach the following assets to the GitHub Release named after the tag:

   | Platform | Artifact                                    | Checksum                                           |
   | -------- | ------------------------------------------- | -------------------------------------------------- |
   | Linux    | `StructuredLogViewer-x86_64.AppImage`       | `StructuredLogViewer-x86_64.AppImage.sha256`       |
   | Linux    | `StructuredLogViewer-x86_64.AppImage.zsync` | `StructuredLogViewer-x86_64.AppImage.zsync.sha256` |
   | Windows  | `StructuredLogViewer.zip`                   | `StructuredLogViewer.zip.sha256`                   |
   | macOS    | `StructuredLogViewer.dmg`                   | `StructuredLogViewer.dmg.sha256`                   |

1. Edit the auto-created GitHub Release to add release notes (highlights, breaking changes, known issues) and publish it.

### Verifying a release

End users can verify a download with:

```sh
# Linux
sha256sum -c StructuredLogViewer-x86_64.AppImage.sha256

# macOS
shasum -a 256 -c StructuredLogViewer.dmg.sha256

# Windows (PowerShell)
$expected = (Get-Content StructuredLogViewer.zip.sha256).Split(" ")[0]
$actual   = (Get-FileHash -Algorithm SHA256 StructuredLogViewer.zip).Hash.ToLower()
if ($expected -eq $actual) { "OK" } else { "MISMATCH" }
```

### AppImage delta updates (zsync)

The Linux AppImage embeds an `UPDATE_INFORMATION` entry pointing at the `latest` GitHub Release, and a sibling `.zsync` file is published alongside every release. Users can update an installed AppImage incrementally with [`appimageupdate`](https://github.com/AppImageCommunity/AppImageUpdate):

```sh
./appimageupdate-x86_64.AppImage StructuredLogViewer-x86_64.AppImage
```

### Hotfix / re-tagging

If you need to re-run packaging for an existing tag (e.g. after fixing a CI issue), delete and re-push the tag:

```sh
git tag -d v0.7.0
git push origin :refs/tags/v0.7.0
git tag -a v0.7.0 -m "Release v0.7.0"
git push origin v0.7.0
```

Avoid doing this once a release has been downloaded by users — checksums and zsync metadata will no longer match their local copies.

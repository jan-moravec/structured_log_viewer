# Structured Log Viewer

[![Build](https://github.com/jan-moravec/structured_log_viewer/workflows/Build/badge.svg)](https://github.com/jan-moravec/structured_log_viewer/actions?query=workflow%3ABuild)
[![GitHub Releases](https://img.shields.io/github/release/jan-moravec/structured_log_viewer.svg)](https://github.com/jan-moravec/structured_log_viewer/releases)

## Overview

Structured Log Viewer is a C++ application that consists of a library for handling structured log data and a Qt-based GUI application for viewing and interacting with the log data.

Currently, only JSON logs are supported.

## Application

### Supported Platforms

- **Linux**
- **Windows**

### Installation

#### Linux

1. Download the latest release of `StructuredLogViewer.AppImage`.
2. Make the AppImage executable:

   ```sh
   chmod +x StructuredLogViewer.AppImage
   ```

3. Run the AppImage:

   ```sh
   ./StructuredLogViewer.AppImage
   ```

#### Windows

1. Download the latest release of `StructuredLogViewer.zip`.
2. Unzip the archive.
3. Run the `StructuredLogViewer.exe` executable.

### Usage

For information regarding using the application, see [README](doc/README.md).

## Develolment

### Project Structure

The project is organized into two main components: the `library` and the `gui` application.

```plaintext
structured_log_viewer/
├── library/
│   ├── include/
│   │   └── loglib/
│   │       └── // Library headers
│   ├── src/
│   │   └── // Library source files
│   └── CMakeLists.txt
├── gui/
│   ├── include/
│   │   └── // GUI headers
│   ├── src/
│   │   └── // GUI source files
│   └── CMakeLists.txt
├── .github/
│   └── workflows/
│       └── // GitHub workflows
├── CMakeLists.txt
└── README.md
```

### Library

The `library` component provides the core functionality for handling structured log data. It includes classes such as `LogLine` and `LogTable` for managing log entries and tables of log data.

### GUI Application

The `gui` component is a Qt-based application that provides a graphical interface for viewing and interacting with the log data. It uses the `library` component to manage the log data and display it in a user-friendly format.

### Build Instructions

#### Prerequisites

- **CMake**: Used for configuring and building the project.
- **Qt5**: Required for the GUI application.
- **C++ Build Tools**

#### Building

It is recommended to use Qt Creator for development and building.

To manually build the project, use the usual CMake commands:

```sh
mkdir build
cd build
cmake ..
cmake --build .
```

On Windows, use the Developer PowerShell or Command Prompt.

### Contributing

Contributions are welcome! Please fork the repository and submit pull requests with your changes.

### Developer Tools

All code is automatically formatted. The C/C++ code is formatted with clang-format, the Python code is formatted with black formatter.

Please install pre-commit for this repository:

```sh
pip install pre-commit
pre-commit install
```

To manually format the code with pre-commit, run:

```sh
pre-commit run --all-files
```

The versions of formatting tools are in [.pre-commit-config.yaml](.pre-commit-config.yaml).

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgments

- <https://github.com/nlohmann/json>
- <https://github.com/HowardHinnant/date>
- <https://github.com/fmtlib/fmt>
- <https://github.com/catchorg/Catch2.git>
- [Icon by Ilham Fitrotul Hayat](https://www.freepik.com/icon/file_5392654)

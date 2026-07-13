# C++ CMake 示例工程（带 GUI 支持）

这是一个最小可运行的 C++ 工程，使用 CMake 构建，默认支持 Windows 和 Linux。

**v2.0 更新：新增 wxWidgets 跨平台 GUI 支持！** 现在支持图形界面预览，同时保留命令行模式。

## 功能特性

- ✅ 跨平台（Windows / Linux）
- ✅ CLI 模式：命令行输出平台检测结果
- ✅ GUI 模式：窗口应用，交互式显示平台信息
- ✅ Google Test 单元测试
- ✅ CMake 现代化构建
- ✅ VS Code 开箱即用

```text
.
|-- .github
|   `-- workflows
|       `-- ci.yml
|-- .vscode
|   |-- extensions.json
|   |-- launch.json
|   `-- tasks.json
|-- .gitignore
|-- CMakeLists.txt
|-- CMakePresets.json
|-- INSTALL_WINDOWS.md
|-- build.ps1
|-- include
|   `-- hello_cross_platform
|       `-- platform.h
|-- README.md
|-- src
|   |-- main.cpp
|   `-- platform.cpp
`-- tests
    `-- hello_core_test.cpp
```

## 构建

Windows 环境安装可以直接参考 [INSTALL_WINDOWS.md](INSTALL_WINDOWS.md)。

如果你在当前这台 Windows 机器上直接使用，最省事的方式是运行：

```powershell
PowerShell -ExecutionPolicy Bypass -File .\build.ps1
```

### Windows

如果你安装了 Visual Studio 或 Build Tools：

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\hello_cross_platform.exe
```

如果你使用 MinGW：

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
.\build\hello_cross_platform.exe
```

### Linux

```bash
cmake -S . -B build
cmake --build build
./build/hello_cross_platform
```

## 运行测试

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## 安装

```powershell
cmake -S . -B build
cmake --build build
cmake --install build --prefix install
```

安装后，头文件会放到 `include/hello_cross_platform`，库文件放到 `lib`，可执行文件放到 `bin`。

## 持续集成

仓库已经包含 GitHub Actions 工作流：

- Windows 使用 `windows-latest`
- Linux 使用 `ubuntu-latest`
- 每次 push 和 pull request 都会自动执行配置、编译和测试

## VS Code 使用

安装好 CMake 和 C++ 编译器后，可以直接在 VS Code 中使用：

- 运行任务 `CMake: Build` 完成配置和编译。
- 运行任务 `CMake: Test` 执行基础测试。
- 使用调试配置 `Debug hello_cross_platform` 启动调试。
- 也可以使用 `CMakePresets.json` 中的 preset 进行命令行构建。
- 工作区会推荐安装 C++ 和 CMake 扩展。

## 说明

- 工程使用 C++17。
- `HELLO_BUILD_SHARED=ON` 时会把 `hello_core` 构建为动态库，否则默认是静态库。
- `hello_core` 是可复用的基础库，`hello_cross_platform` 是示例可执行程序。
- 示例程序会输出当前检测到的平台。
- `tests/hello_core_test.cpp` 是一个最小测试示例，后续可以替换成 GoogleTest 或 Catch2。
- 当前工作区已经包含基础的 `.vscode` 配置和 `.gitignore`。
# Windows GUI 环境准备

这个项目现在支持 GUI 应用。在 Windows 上运行需要先装 wxWidgets 3.0。

## 方案 1：使用 vcpkg（推荐）

vcpkg 是 Microsoft 官方维护的 C++ 包管理器，最容易集成到 CMake。

### 1. 安装 vcpkg

```powershell
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
```

### 2. 安装 wxWidgets

```powershell
# x64 only
.\vcpkg install wxwidgets:x64-windows
```

> 第一次编译会比较久（20-30 分钟），因为 wxWidgets 源码比较大。

### 3. 集成到项目

运行脚本时指定 vcpkg 工具链：

```powershell
# 在项目根目录
cmake -S . -B build-vcpkg `
  -DCMAKE_TOOLCHAIN_FILE="C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET="x64-windows"

cmake --build build-vcpkg --config Release
```

或者改造 `build.ps1`，在里面自动加上 `-DCMAKE_TOOLCHAIN_FILE` 参数。

## 方案 2：预编译二进制

从 [wxWidgets 官网](https://wxwidgets.org/downloads/) 下载预编译 Windows 二进制，解压后设置 wxWidgets_ROOT_DIR 环境变量。

## 方案 3：从源码编译（不推荐，比较复杂）

从 GitHub 克隆 wxWidgets，按照其文档编译。

---

## 运行 GUI 应用

安装完 wxWidgets 后，运行：

```powershell
.\build.ps1           # 自动生成 GUI 应用并启动
.\build.ps1 -Configuration Debug     # Debug 模式
```

或者手工编译和运行：

```powershell
$executable = "build-vs2017-x64\Release\hello_cross_platform.exe"
& $executable --gui
```

不带 `--gui` 参数时，会进入命令行模式。

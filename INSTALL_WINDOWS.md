# Windows 安装与使用指南

这份文档已经按当前这台机器实际验证过的环境更新。

## 已确认可用的环境

- CMake 已安装：`C:\Program Files\CMake\bin\cmake.exe`
- CMake 版本：`4.3.1`
- Visual Studio 已安装：`Visual Studio Professional 2017`
- MSVC 编译器可用：`19.16.27054`
- MinGW、`g++`、`clang++` 当前未安装

## 当前机器上的关键问题

工具不是没装，而是普通 PowerShell 环境没有自动加载：

- `CMake` 没加入当前终端的 `PATH`
- `cl` 需要先加载 Visual Studio 的开发者环境

所以你在普通终端里直接输入 `cmake` 或 `cl`，可能会显示找不到命令。

## 已验证通过的构建方式

当前机器已经实际验证过以下流程可以成功完成：

- CMake 配置
- Release 编译
- CTest 测试
- 运行可执行程序

### 方式 1：直接运行一键脚本

项目根目录已经提供：`build.ps1`

在项目目录执行：

```powershell
Set-Location "c:\code test"
PowerShell -ExecutionPolicy Bypass -File .\build.ps1
```

这个脚本会自动完成：

- 查找 `cmake.exe`
- 查找 `ctest.exe`
- 查找 Visual Studio 的 `vcvars64.bat`
- 配置工程
- 编译 Release
- 运行测试
- 执行程序

如果要编译 Debug 版本：

```powershell
PowerShell -ExecutionPolicy Bypass -File .\build.ps1 -Configuration Debug
```

### 方式 2：手动执行已验证命令

```powershell
cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && "C:\Program Files\CMake\bin\cmake.exe" -S "c:\code test" -B "c:\code test\build-vs2017-x64" -A x64 && "C:\Program Files\CMake\bin\cmake.exe" --build "c:\code test\build-vs2017-x64" --config Release && "C:\Program Files\CMake\bin\ctest.exe" --test-dir "c:\code test\build-vs2017-x64" -C Release --output-on-failure && "c:\code test\build-vs2017-x64\Release\hello_cross_platform.exe" --gui'
```

## 如果你想把环境修正得更干净

### 让 cmake 在普通 PowerShell 里直接可用

把下面目录加入系统 `PATH`：

```text
C:\Program Files\CMake\bin
```

然后重新打开 VS Code 或 PowerShell，再执行：

```powershell
cmake --version
```

### 让 cl 更容易使用

推荐使用以下任意一种终端启动方式：

- Developer PowerShell for Visual Studio 2017
- x64 Native Tools Command Prompt for VS 2017
- 或先调用 `vcvars64.bat` 再编译

当前已验证存在的开发环境脚本：

```text
C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat
```

## 如果将来需要重装或补装

这台机器也有这些包管理工具：

- `winget`
- `choco`

安装 CMake：

```powershell
winget install Kitware.CMake
```

或：

```powershell
choco install cmake -y
```

如果未来要补装更新版 MSVC，优先建议：

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools
```

## VS Code 推荐扩展

- `ms-vscode.cpptools`
- `ms-vscode.cmake-tools`
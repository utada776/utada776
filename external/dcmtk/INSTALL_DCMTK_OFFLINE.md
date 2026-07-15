# DCMTK 离线安装指南

## 概述
DCMTK（DICOM Toolkit）是医学影像 DICOM 标准的实现库。本指南用于在 `c:\code test\external\dcmtk\` 本地编译和安装。

## 下载源代码

### 方式 1：命令行下载（推荐）
```powershell
cd c:\code test\external\dcmtk

# 下载 DCMTK 3.7.0 源代码（~50MB）
Invoke-WebRequest -Uri "https://github.com/DCMTK/dcmtk/archive/refs/tags/DCMTK-3.7.0.tar.gz" `
    -OutFile "DCMTK-3.7.0.tar.gz" -UseBasicParsing

# 解压
tar -xzf DCMTK-3.7.0.tar.gz
Move-Item "dcmtk-DCMTK-3.7.0" "src"
```

### 方式 2：手动下载
1. 打开浏览器访问：https://github.com/DCMTK/dcmtk/releases/tag/DCMTK-3.7.0
2. 下载 `Source code (tar.gz)`
3. 保存到：`c:\code test\external\dcmtk\DCMTK-3.7.0.tar.gz`
4. 解压到：`c:\code test\external\dcmtk\src\`

## 编译和安装

### 前置条件
- Visual Studio 2017 或更新版本
- CMake 3.16+
- Zlib（通常已包含或从 vcpkg 获取）

### 编译步骤

#### 步骤 1：创建编译目录
```powershell
cd c:\code test\external\dcmtk
mkdir build
mkdir install
cd build
```

#### 步骤 2：配置 CMake
```powershell
cmake ..\src `
    -G "Visual Studio 15 2017" -A x64 `
    -DCMAKE_INSTALL_PREFIX="c:\code test\external\dcmtk\install" `
    -DBUILD_SHARED_LIBS=ON `
    -DDCMTK_WITH_ZLIB=ON `
    -DBUILD_TESTING=OFF `
    -DDCMTK_ENABLE_PRIVATE_TAGS=ON `
    -DCMAKE_BUILD_TYPE=Release
```

**关键选项说明：**
- `CMAKE_INSTALL_PREFIX`: 安装路径
- `BUILD_SHARED_LIBS=ON`: 编译为 DLL
- `DCMTK_WITH_ZLIB=ON`: 启用 ZIP 压缩支持
- `DDCMTK_ENABLE_PRIVATE_TAGS=ON`: 支持私有标签

#### 步骤 3：编译（约 15-30 分钟）
```powershell
# 编译所有配置
cmake --build . --config Release --parallel 4

# 或使用 Visual Studio
msbuild DCMTK.sln /p:Configuration=Release /m:4
```

#### 步骤 4：安装
```powershell
cmake --install . --config Release
# 或
msbuild INSTALL.vcxproj /p:Configuration=Release
```

## 验证安装

编译完成后，应存在以下目录结构：
```
c:\code test\external\dcmtk\
├── src/              # 源代码
├── build/            # 编译目录（可删除）
└── install/          # 安装目录
    ├── bin/          # dcmtk 工具和 DLL
    ├── lib/          # 库文件（dcmdata.lib, dcmimgle.lib, ofstd.lib）
    ├── include/      # 头文件
    └── lib/cmake/dcmtk/  # CMake 配置文件 ✅ 重要
```

验证关键文件存在：
```powershell
Test-Path "c:\code test\external\dcmtk\install\lib\cmake\dcmtk\DCMTKConfig.cmake"
# 应返回 True

Test-Path "c:\code test\external\dcmtk\install\lib\dcmdata.lib"
# 应返回 True
```

## CMakeLists.txt 集成

在主项目 CMakeLists.txt 中添加：
```cmake
# 使用本地 DCMTK（而不是 vcpkg）
set(DCMTK_DIR "c:/code test/external/dcmtk/install/lib/cmake/dcmtk" CACHE PATH "DCMTK CMake config")
find_package(DCMTK CONFIG REQUIRED)

target_compile_definitions(hello_cross_platform PRIVATE FDK_HAS_DCMTK=1)
target_link_libraries(hello_cross_platform PRIVATE DCMTK::dcmdata DCMTK::dcmimgle DCMTK::ofstd)
```

## Zlib 依赖处理

DCMTK 需要 Zlib。有两种方式提供：

### 方式 A：使用 vcpkg 的 Zlib
```powershell
# DCMTK CMake 会自动搜索系统中的 Zlib
# 如果使用 vcpkg 工具链，Zlib 会自动找到
```

### 方式 B：离线编译 Zlib（可选）
```powershell
# 如果 Zlib 找不到，从以下位置下载编译：
# https://github.com/madler/zlib/releases/tag/v1.3.1
# 编译后放在 c:\code test\external\zlib\install\
```

## 故障排查

### 编译错误：Zlib 未找到
```
解决：在 CMakeLists.txt 中明确指定 Zlib
set(ZLIB_ROOT "C:/tools/vcpkg/installed/x64-windows" CACHE PATH "Zlib location")
```

### 编译失败：内存不足
```
解决：减少并行数
cmake --build . --config Release --parallel 2
```

### CMake 无法找到 DCMTK
```
确认：c:\code test\external\dcmtk\install\lib\cmake\dcmtk\DCMTKConfig.cmake 存在
或在 CMakeLists.txt 中明确设置 DCMTK_DIR
```

### DICOM 导出功能不可用
```
检查编译定义：FDK_HAS_DCMTK=1 是否设置
检查链接：DCMTK::dcmdata DCMTK::dcmimgle DCMTK::ofstd 是否链接
```

## 文件清理

编译完成后，可删除以节省空间：
```powershell
Remove-Item "c:\code test\external\dcmtk\build" -Recurse    # ~0.5GB（可选）
Remove-Item "c:\code test\external\dcmtk\src" -Recurse      # ~0.1GB（保留源以便重新编译）
```

## 更新 DCMTK 版本

若要使用不同版本（如最新 3.8.0）：
```powershell
# 在 GitHub 查看最新版本
# https://github.com/DCMTK/dcmtk/releases

# 修改下载 URL 中的版本号后重复步骤 1-4
```

## 核心模块说明

DCMTK 由多个模块组成，我们主要使用的是：

| 模块 | 功能 | 使用场景 |
|-----|------|---------|
| **dcmdata** | 低级 DICOM 数据操作 | 创建、读取、修改 DICOM 文件 |
| **dcmimgle** | 医学影像显示和处理 | 像素数据读取、窗口/级别调整 |
| **ofstd** | 标准库扩展 | 字符串、文件操作、日期时间 |

## 文件交叉引用

本安装后，以下项目文件将引用本地 DCMTK：
- `src/dicom/dicom_exporter.cpp` - 包含 DCMTK 头文件
- `src/dicom/dicom_exporter.h` - DCMTK 导出接口
- `CMakeLists.txt` - DCMTK 查找和链接配置

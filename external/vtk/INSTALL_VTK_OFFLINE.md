# VTK 离线安装指南

## 概述
VTK（Visualization Toolkit）是 3D 渲染和医学影像可视化库。本指南用于在 `c:\code test\external\vtk\` 本地编译和安装。

## 下载源代码

### 方式 1：命令行下载（推荐）
```powershell
cd c:\code test\external\vtk

# 下载 VTK 9.x 源代码（~130MB）
Invoke-WebRequest -Uri "https://github.com/Kitware/VTK/archive/refs/tags/v9.3.0.tar.gz" `
    -OutFile "VTK-9.3.0.tar.gz" -UseBasicParsing

# 解压
tar -xzf VTK-9.3.0.tar.gz
Move-Item "VTK-9.3.0" "src"
```

### 方式 2：手动下载
1. 打开浏览器访问：https://github.com/Kitware/VTK/releases/tag/v9.3.0
2. 下载 `Source code (tar.gz)`
3. 保存到：`c:\code test\external\vtk\VTK-9.3.0.tar.gz`
4. 解压到：`c:\code test\external\vtk\src\`

## 编译和安装

### 前置条件
- Visual Studio 2017 或更新版本
- CMake 3.16+
- OpenGL 支持

### 编译步骤

#### 步骤 1：创建编译目录
```powershell
cd c:\code test\external\vtk
mkdir build
mkdir install
cd build
```

#### 步骤 2：配置 CMake
```powershell
cmake ..\src `
    -G "Visual Studio 15 2017" -A x64 `
    -DCMAKE_INSTALL_PREFIX="c:\code test\external\vtk\install" `
    -DBUILD_SHARED_LIBS=ON `
    -DVTK_GROUP_RENDERING=ON `
    -DVTK_GROUP_IMAGING=ON `
    -DVTK_WRAP_PYTHON=OFF `
    -DBUILD_TESTING=OFF `
    -DCMAKE_BUILD_TYPE=Release
```

**关键选项说明：**
- `CMAKE_INSTALL_PREFIX`: 安装路径
- `BUILD_SHARED_LIBS=ON`: 编译为 DLL（推荐）
- `VTK_GROUP_RENDERING=ON`: 包含渲染模块
- `VTK_GROUP_IMAGING=ON`: 包含医学影像模块
- `VTK_WRAP_PYTHON=OFF`: 不需要 Python 绑定

#### 步骤 3：编译（约 30-60 分钟）
```powershell
# 编译所有配置
cmake --build . --config Release --parallel 4

# 或使用 Visual Studio
msbuild VTK.sln /p:Configuration=Release /m:4
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
c:\code test\external\vtk\
├── src/              # 源代码
├── build/            # 编译目录（可删除）
└── install/          # 安装目录
    ├── bin/          # DLL 文件
    ├── lib/          # 库文件
    ├── include/      # 头文件
    └── lib/cmake/VTK/  # CMake 配置文件 ✅ 重要
```

验证关键文件存在：
```powershell
Test-Path "c:\code test\external\vtk\install\lib\cmake\VTK\VTKConfig.cmake"
# 应返回 True
```

## CMakeLists.txt 集成

在主项目 CMakeLists.txt 中添加：
```cmake
# 使用本地 VTK（而不是 vcpkg）
set(VTK_DIR "c:/code test/external/vtk/install/lib/cmake/VTK" CACHE PATH "VTK CMake config")
find_package(VTK CONFIG REQUIRED)
```

## 故障排查

### 编译错误：找不到 OpenGL
```
解决：安装 Visual Studio OpenGL 组件或设置：
-DVTK_ENABLE_OPENGL=OFF
```

### 编译失败：内存不足
```
解决：减少并行数
cmake --build . --config Release --parallel 2
```

### CMake 无法找到 VTK
```
确认：c:\code test\external\vtk\install\lib\cmake\VTK\VTKConfig.cmake 存在
或在 CMakeLists.txt 中明确设置 VTK_DIR
```

## 文件清理

编译完成后，可删除以节省空间：
```powershell
Remove-Item "c:\code test\external\vtk\build" -Recurse    # ~2GB（可选）
Remove-Item "c:\code test\external\vtk\src" -Recurse      # ~0.5GB（保留源以便重新编译）
```

## 更新 VTK 版本

若要使用不同版本，修改版本号后重复步骤 1-4：
```powershell
# 例如更新到 v9.4.0
$version = "9.4.0"
```

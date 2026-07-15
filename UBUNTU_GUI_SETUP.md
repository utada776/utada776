# Ubuntu GUI 环境准备

本项目支持 wxWidgets GUI、VTK 医学影像可视化和 DCMTK DICOM 处理。

## 安装依赖

### 推荐安装（包含所有功能）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake \
  libwxgtk3.2-dev \
  libvtk9-dev \
  libdcmtk-dev        # DICOM 处理（可选，用于导出 DICOM 文件）
```

### 仅安装必需依赖（无 DCMTK）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake \
  libwxgtk3.2-dev \
  libvtk9-dev
```

### GTK2 版本（备选）

如果上面的命令不适用（较旧的 Ubuntu 版本），改用：

```bash
sudo apt-get install -y libwxgtk3.0-gtk3-dev libvtk9-dev libdcmtk-dev
```

### 验证安装

验证 wxWidgets 已安装：

```bash
wx-config --version
# 应输出类似 3.0.x 的版本号
```

验证 VTK 已安装：

```bash
pkg-config --modversion vtk9
# 应输出 VTK 版本
```

验证 DCMTK 已安装（可选）：

```bash
dcmfile --version 2>/dev/null || echo "DCMTK 未安装"
```

## 编译和运行

### 使用脚本（推荐）

```bash
chmod +x build.sh
./build.sh                           # Release，自动判断 GUI/CLI
./build.sh Debug                     # Debug 模式
./build.sh --gui                     # 强制 GUI 模式
./build.sh --cli                     # 强制 CLI 模式
./build.sh --no-run                  # 仅编译和测试，不启动程序
./build.sh --configuration Debug     # 显式指定构建类型
```

脚本会自动检查：
- `cmake`
- `ctest`
- `g++` / `clang++`
- `wxWidgets`
- `VTK`
- `DCMTK`（可选）

如果没有图形显示环境（例如纯 SSH 终端），脚本会自动回退到 CLI 模式。

### 手工编译

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel 4
ctest --test-dir build-linux --output-on-failure
./build-linux/hello_cross_platform --gui
```

## CLI（命令行模式）

如果没有图形环境或想在终端中运行：

```bash
./build-linux/hello_cross_platform
# 或
./build.sh --cli
```

## FDK 3D 重建功能

本项目包含 FDK（Feldkamp、Davis、Kress）锥束 CT 快速重建模块：

- **在线重建** - 实时显示重建进度和 3D 体积
- **交互式查看** - 鼠标控制切片、缩放、平移
- **自动 Window/Level** - 自动调整显示范围
- **DICOM 导出** - 将重建结果保存为 DICOM 序列
- **参数保存** - 自动保存和恢复重建参数

GUI 菜单已集成 FDK 重建功能。

## 常见问题

### CMake 找不到 wxWidgets

确保安装了开发包（带 `-dev` 后缀），不仅仅是运行时库：

```bash
sudo apt-get install -y libwxgtk3.2-dev
```

### CMake 找不到 VTK

```bash
sudo apt-get install -y libvtk9-dev
```

### CMake 找不到 DCMTK

DCMTK 是可选的。如果不需要 DICOM 导出功能，可以跳过安装。

```bash
# 如果需要
sudo apt-get install -y libdcmtk-dev
```

### GUI 窗口不显示 / 远程终端

如果在远程 SSH 连接中运行：

1. **X11 转发**（需要本地 X 服务器）：
   ```bash
   ssh -X user@host
   ```

2. **虚拟帧缓冲（Xvfb）**：
   ```bash
   sudo apt-get install -y xvfb
   xvfb-run ./build-linux/hello_cross_platform --gui
   ```

3. **纯 CLI 模式**（推荐）：
   ```bash
   ./build.sh --cli
   ```

### 编译错误：找不到 wx/wx.h

运行以下命令检查 wxWidgets 路径是否正确：

```bash
pkg-config --cflags wxwidgets-3.0
# 应输出 -I 路径
```

### 编译错误：VTK not found

安装 VTK 开发包：

```bash
sudo apt-get install -y libvtk9-dev
```

## 故障排查

### 编译变慢

检查系统资源：

```bash
free -h         # 检查内存
nproc          # 检查 CPU 核心数
top            # 查看实时资源使用
```

减少并行编译数：

```bash
cmake --build build-linux --parallel 2  # 使用 2 核
```

### 链接错误

清理旧的编译缓存：

```bash
rm -rf build-linux CMakeCache.txt
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

### 运行时错误

查看详细错误信息：

```bash
./build-linux/hello_cross_platform --verbose 2>&1 | head -50
```

## 更多帮助

- [README.md](./README.md) - 项目总览
- [external/README.md](external/README.md) - 外部依赖缓存说明
- [INSTALL_WINDOWS.md](./INSTALL_WINDOWS.md) - Windows 安装指南

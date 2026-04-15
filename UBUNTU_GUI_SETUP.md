# Ubuntu GUI 环境准备

这个项目现在支持 GUI 应用。在 Ubuntu 上运行需要先装 wxWidgets 3.0 开发库。

## 安装依赖

### GTK3 版本（推荐）

```bash
sudo apt-get update
sudo apt-get install libwxgtk3.0-gtk3-dev
```

### GTK2 版本（备选）

```bash
sudo apt-get install libwxgtk3.0-dev
```

验证安装：

```bash
wx-config --version
```

应该输出类似 `3.0.x` 的版本号。

## 编译和运行

### 使用脚本

```bash
chmod +x build.sh
./build.sh           # Release 模式，自动启动 GUI
./build.sh Debug     # Debug 模式
```

脚本会自动检查 wxWidgets 是否安装。

### 手工编译

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
./build-linux/hello_cross_platform --gui
```

## 命令行模式

不带 `--gui` 参数时是命令行模式：

```bash
./build-linux/hello_cross_platform
```

## 常见问题

### CMake 找不到 wxWidgets

确保安装了开发包，不仅仅是运行时库。上面的命令包含 `-dev` 包。

### GUI 窗口不显示

- 如果在远程 SSH 连接中运行，需要转发 X11 或使用虚拟帧缓冲（Xvfb）
- 如果在容器中运行，容器需要配置 X11 转发或使用 Wayland

### 编译错误：找不到 wx/wx.h

运行 `pkg-config --cflags wxwidgets-3.0` 检查路径是否正确。

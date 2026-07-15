# RTK FDK 重建实现说明

本文档描述当前工程中的 FDK 重建路径。当前实现不再包含旧自研 FFT 滤波、Parker 权重或手写反投影内核；重建算法统一由 RTK 执行。

## 当前职责划分

- `fdk_main.cpp`：保留原有 wxWidgets 参数窗口、重建进度窗口、三视图预览和 DICOM 导出入口。
- `fdk_recon.cpp`：负责读取参数、读取投影、构造 RTK 输入，并调用 RTK FDK pipeline。
- `fdk_types.h`：保留 UI、投影读取、RTK 后端和导出流程共享的数据结构。
- `fdk_dicom_exporter.*`：负责把重建后的 `Volume3D` 导出为 DICOM 或 RAW fallback。

## RTK 重建流程

1. `LoadFrameInfo` 读取帧编号、机架角和探测器中心偏移。
2. `LoadProjections` 读取 HIS 投影，完成增益归一化、`-log(I/I0)` 线积分转换和准直裁剪。
3. `FdkEngine::Reconstruct` 将 `ProjectionImage` 序列打包成 `itk::Image<float, 3>` projection stack。
4. `CreateRtkGeometry` 使用 `rtk::ThreeDCircularProjectionGeometry` 构造每帧几何，距离单位从 cm 转为 mm。
5. RTK pipeline 执行：
   - `rtk::DisplacedDetectorForOffsetFieldOfViewImageFilter`
   - `rtk::ParkerShortScanImageFilter`，仅在 `short_scan=true` 时启用
   - `rtk::FDKConeBeamReconstructionFilter`
6. `PostProcess` 只做应用层输出映射：`HU = value * scale_out + offset_out`，以及可选阈值截断。

## 保留的输入预处理

投影读取和校正仍在工程内完成，因为这些属于数据接入层，不是重建算法内核：

- HIS 文件头自动识别。
- gain 图像按投影分辨率复用或重采样。
- 原始计数裁剪到有效范围，避免 `log(0)`。
- 图像边缘准直区域按参数清零。
- XML 中的 `UCentre` / `VCentre` 会缩放到当前投影尺寸，并转换为 RTK 的探测器偏移量。

## 构建依赖

CMake 在找到 `external/rtk/build` 和 `external/itk/build` 后会定义 `FDK_HAS_RTK=1` 并链接 RTK/ITK 静态库。若 RTK 不可用，重建会明确报错，不会回退到旧算法。

## 已移除的旧路径

以下旧逻辑不再存在于当前 FDK 重建路径中：

- 工程自写 FFT ramp filter。
- 工程自写 Parker 权重。
- 工程自写逐体素反投影。
- 工程自写角步距权重和在线增量反投影。
- 任何旧参考实现的重建算法分支。

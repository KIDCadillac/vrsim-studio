# 第三方源码与归属

OpenXR 三个生成头保留原 Copyright 2017–2026 Khronos 与 SPDX `Apache-2.0 OR MIT`。本归档附原 MIT 许可全文；头文件本身保留实际版权归属。官方来源：[OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK)，[MIT](https://github.com/KhronosGroup/OpenXR-SDK/blob/main/LICENSES/MIT.txt)。

`ObsD3D9LayoutQA.cpp` 中的 x64 signature/mask/scan 源于 OBS Studio `plugins/win-capture/get-graphics-offsets/d3d9-offsets.cpp`，commit `30b63c68490be1b0e0cb250a61c9eca1b01c9076`，原源码标记 GPLv2+。完整 GPLv2 COPYING 已放在三个归档副本注释所指的 `reference/COPYING`。它被真实 bridge/helper include 链引用，不能将整个闭包冒称为纯自有 MIT 源码。官方：[原文件](https://github.com/obsproject/obs-studio/blob/30b63c68490be1b0e0cb250a61c9eca1b01c9076/plugins/win-capture/get-graphics-offsets/d3d9-offsets.cpp)，[COPYING](https://github.com/obsproject/obs-studio/blob/30b63c68490be1b0e0cb250a61c9eca1b01c9076/COPYING)。

GpuProjection shader include 是保留的项目 shader 编译输出；其生成来源和字节哈希列于清单。本仓库不含商业 SDK 的实现、反编译材料或二进制，也不含官方打包模板素材。项目自身许可由仓库维护者单独确定，归属说明不替代相应许可条件。
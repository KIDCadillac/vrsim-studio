# 构建归档与依赖

本仓库先保留实际构建过的源码字节，尚未验证从公开 checkout 直接构建或打包。此次导出没有运行编译器、SDK、SIM、XR、安装器或硬件。

## 两套源码

- `src/managed`：当前 4K QA 的 20 个冻结 C# 输入，对应 managed SHA256 `C2CE02D2DD21C1BAF2559A7958953DFAE00B751553CF789B50794FA294F79D67`。
- `src/native` 与 `src/vendor`：当前 native 构建的 23 个冻结输入，对应 native SHA256 `1E60BBB092BC8A38072428B814E4C5C7A89132352FE26DDB426539831535418D`。
- `baselines/0.1.4.0`：已安装版本的 20 个 managed 和 22 个 native 输入，另保留两个实际生成的 shader include、原构建和打包脚本。其正式构建为 SHARED_2048；当前 4K QA 并不是该安装包。
- `source-manifest.json`：相对文件路径、SHA256、字节数和来源构建摘要哈希。没有原内部运行回执。

## 外部依赖

Windows x64；.NET Framework 4.8 Developer Pack 与 Roslyn C# 编译器；PowerShell 7.5（历史归档编排环境）。Native 证据使用 MSVC 14.44.35207、Windows SDK 10.0.22621.0、C++17、系统 Direct3D/DXGI 库。OpenXR loader/runtime 由本机安装提供。

Managed 当前实际引用 10 个 .NET/WPF BCL 和 4 个合法安装的 iiQWorks.Sim 1.3 依赖：Create3D.Shared、UX.Shared、UX.Shared.Infrastructure、安装附带的 Caliburn.Micro。名称与实际哈希见 `build/archive/build-summary.json`；这些第三方 DLL 和系统工具不随仓库分发。

旧 NC01 readback 与 NC02 pair-stage 的自有工程/源码保存在基线内。正式 KOP 打包依赖 iiQWorks.App Builder 及其本机官方模板。本仓库不分发模板图标、KRCSetup、第三方 KOP、SDK 二进制、场景或安装目录；`package-identity.json` 仅记录原包的版本与哈希，不是可下载软件包。

## 工具各自负责什么

| 工具／接口 | 用途 |
| --- | --- |
| Roslyn C# 编译器／.NET Framework 4.8 | 编译托管插件 DLL。 |
| MSVC／Windows SDK | 编译 C++ 原生渲染与 OpenXR 桥接 DLL。 |
| iiQWorks.App Builder 1.2 | 创建插件应用目录与 KOP 包；历史脚本使用 CatalogCreator、KopBuilder 和 KopImporter。 |
| iiQWorks.Sim SDK／API | 开发依赖与宿主场景、相机、界面的可调用接口。 |
| OpenXR loader／runtime | 对接头显会话、跟踪和画面提交。 |
| ADB | Quest 调试、录像与文件拉取；用于测试工作流。 |

## 当前可复现限制

源码和归档脚本保留当时字节，包括本机 QA helper DLL 路径、历史 include 路径、输出目录以及内部 pin/plan 输入约束。它们是历史开发路径，不是便携构建入口。不能用这些路径在其他机器直接运行，也不能因为源码已入库就声称完整公开构建通过。下一步需在独立候选中参数化路径、补齐所需自有 helper 与相对构建配方，再独立编译核验；本次没有改写产品源码。

当前七个 managed 变体为五个合法编译、两个 CS1029 宏拒绝；native 为四个原变体及 ABI/预处理核验。实际变体摘要见 `build/archive/build-summary.json`。已编 shader include 原字节随源码保留；当前 native 构建没有重编 shader，不能只凭 HLSL 文本推定同一字节码。基线 generated include 放在 `vrsim-native/generated`，需要由独立构建配方配置 include 路径。

视觉运动破片和规则摩尔纹尚未修复，120 个不同新内容帧/秒未通过。编译通过、XR 提交率、录像编码帧率均不替代这些验收目标。

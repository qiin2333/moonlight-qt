# D3D11 VP 诊断工具

手动检查 AMD 显卡驱动能否创建并使用 NV12/P010 视频处理器输入、RGBA 输出及相关视图，
覆盖 720p→1080p 和 1080p→720p。仅依赖 Windows SDK 和 C++17 编译器，不依赖 Qt、FFmpeg 或 AMF。

此工具独立搭建 VP 管线，只证明驱动接受这些资源和处理命令，不验证 Moonlight 渲染器的
回退状态切换，也不检查输出像素质量；不能替代生产路径的回归测试。它不加入应用构建或 CI。

在安装 Windows SDK 的 Visual Studio 开发者命令提示符中，从仓库根目录构建运行：

```bat
cmake -S diagnostics/d3d11-vp -B build/d3d11-vp -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/d3d11-vp
build\d3d11-vp\d3d11_vp_probe.exe
```

输出每个 AMD 适配器与资源组合的 PASS/FAIL。退出码：0=全部通过，1=创建或处理失败，2=没有可用 AMD 适配器。

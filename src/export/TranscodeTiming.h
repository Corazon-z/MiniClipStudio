// S03的ExportMp4和TranscodeTrack用这个小工具记录解码、过滤、编码、封装API花了多久。
// 创建ScopedSeconds时记起点，离开作用域时自动把耗时加到指定指标，相当于给一段调用计时。
// 记录的是实际经过的墙钟时间，不是CPU采样；累计目标destination只是借用，必须比计时对象活得久。
#pragma once
#include <chrono>
namespace miniclip {
// 利用析构自动记账，无论正常返回还是抛异常离开代码块，这一段经过的时间都会被累计。
class ScopedSeconds {
public:
    // 保存要累计的指标引用，并用steady_clock记起点，避免系统日历时间调整影响耗时计算。
    explicit ScopedSeconds(double& destination) : destination_(destination), start_(std::chrono::steady_clock::now()) {}
    // 离开作用域时将“现在减起点”的秒数加到指标；其他调用可能发生在内部，因此阶段耗时不能简单当CPU热点。
    ~ScopedSeconds() { destination_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count(); }
private:
    double& destination_;
    std::chrono::steady_clock::time_point start_;
};
}

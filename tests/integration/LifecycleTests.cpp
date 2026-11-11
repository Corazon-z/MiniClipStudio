// 独立控制台验证：资源不可复制、WAV调用顺序、控制器同步与重入保护。
// 不访问麦克风；本文件不加入产品EXE，避免两个main入口。
#include "../../src/media/mux/WavWriter.h"
#include "../../src/application/RecordingController.h"
#include <filesystem>
#include <type_traits>
#include <iostream>
using namespace miniclip;
static_assert(!std::is_copy_constructible_v<InputResources>);
static_assert(!std::is_copy_constructible_v<WavWriter>);
static_assert(!std::is_copy_constructible_v<RecordingController>);
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Provide a new output WAV path");
        std::filesystem::path path(argv[1]);
        Require(!std::filesystem::exists(path), "Test path already exists");
        {
            WavWriter writer(path.wstring());
            bool rejected = false;
            try { writer.Push(nullptr); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Push before Open must fail");
            rejected = false;
            try { writer.Finish(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Finish before Open must fail");
            writer.Open();
            rejected = false;
            try { writer.Push(nullptr); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Null audio frame must fail safely");
            rejected = false;
            try { writer.Open(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Repeated Open must fail");
            rejected = false;
            try { writer.Finish(); } catch (const NoSamples&) { rejected = true; }
            Require(rejected, "Empty output must not publish");
            rejected = false;
            try { writer.Finish(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Repeated Finish must fail");
        }
        Require(!std::filesystem::exists(path), "Empty output was published");
        RecordingController controller;
        for (int i = 0; i < 3; ++i) {
            auto job = std::make_unique<RecordingJob>();
            job->cancel.store(true); // 启动前取消：不打开设备。
            controller.Start(std::move(job));
            bool rejected = false;
            try { controller.Start(std::make_unique<RecordingJob>()); }
            catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Uncleared task must prevent reentry");
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!controller.TryJoin()) {
                Require(std::chrono::steady_clock::now() < deadline, "Cancelled task did not finish");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Require(controller.Current()->result.cancelled, "Cancelled result missing");
            controller.ClearFinished();
            Require(!controller.Current(), "Finished task was retained");
        }
        controller.Shutdown();
        controller.Shutdown();
        std::cout << "PASS: ownership, WAV state checks, controller cancellation/reentry/restart\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#pragma once

// ══ bench / 探针（跑完即退）的请求与装配（设计 14 §10）════════════════════
//   ★ 2026-10-19：**只剩 `--gpuscale-probe`**（它造 8×8 已知图走 raw GL，1:1 直通，
//     不需要任何图源）。四条"按条目号取图"的实验（`--bench-thumbs` / `--bench-parallel` /
//     `--bench-gpu` / `--dump-scaled`）随 `ImageProvider` 那套**自建序列**一起删了。
namespace devbench {

struct Request
{
    bool gpuProbe = false;      // --gpuscale-probe
    bool wanted() const { return gpuProbe; }
};

// 调用方只在 `wanted()` 为真时调用；返回退出码（0 = 正常）。
int runBench(const Request &req);

} // namespace devbench

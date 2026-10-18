#include "devbench.h"

#include "gpuscalecheck.h"

namespace devbench {

int runBench(const Request &req)
{
    if (req.gpuProbe) return runGpuScaleProbe();
    return 0;
}

} // namespace devbench

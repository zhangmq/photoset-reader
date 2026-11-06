#include "qmlregister.h"

#include "cpuscaleritem.h"
#include "gpuscaleritem.h"

#include <qqml.h>

namespace app {

void registerQmlTypes()
{
    // GPU 缩放渲染组件（产品路径）：`FullscreenView` 的 `GpuScalerItem`
    qmlRegisterType<GpuScalerItem>("AppUi", 1, 0, "GpuScalerItem");
    // CPU 缩放项：**Smooth (Qt) 这一项**与**软件后端降级**都用它（设计 16 §五）
    qmlRegisterType<CpuScalerItem>("AppUi", 1, 0, "CpuScalerItem");
}

}   // namespace app

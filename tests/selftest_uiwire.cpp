#include "testsupport.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringLiteral>
#include <QVector>

#include "fetchpipeline.h"
#include "gpuscaleritem.h"
#include "sequence.h"
#include "viewermodel.h"

// ══ 接线的**最小用例**（ib-test-uiwire；原先由 `image-browser --selftest-uiwire` 跑）══
//   T0 第四批机械搬迁：函数体**一字未改**（含输出文案与 13 项口径）。
//   ★ 入口样板（QApplication / 应用名）在 `testsupport::suiteMain`（设计 14 §11）。
using testsupport::waitFor;

static int runUiWireSelfTest()
{
    using namespace fetch;
    // 统一断言器（T0 收尾批：footer 带项数、rc 0/1）；打印格式与旧 lambda 一字不差
    testsupport::Check check(QStringLiteral("uiwire"));

    auto decode = [](const Sequence::Item &) -> QImage {
        QImage img(60, 100, QImage::Format_RGB32);       // 竖版（宽/高 = 0.6）
        img.fill(Qt::darkCyan);
        return img;
    };
    QVector<Sequence::Item> items;
    for (int i = 0; i < 6; ++i) {
        Sequence::Item it;
        it.kind = Sequence::Kind::Image;
        it.ord = i;
        it.label = QStringLiteral("k%1").arg(i);
        it.displayName = it.label;
        it.size = QSize(1200, 1800);                     // 不解码读到的尺寸（判宽窄用）
        items << it;
    }

    Sequence seq;
    seq.buildFromEntries(QStringLiteral("/synth"), items);
    // ★ T2d-1c-①：**有界窗口（`fetch::Window`）由 viewer 自己建** ⇒ 用例只把序列交给它（不再有 `setWindow`）。
    ViewerModel vm(7, Mode::Single, nullptr, decode);                  // 生产时间源（真实时钟）
    vm.setSequence(&seq);

    int curSig = 0, runSig = 0, dblSig = 0;
    QObject::connect(&vm, &ViewerModel::currentChanged, [&curSig] { ++curSig; });
    QObject::connect(&vm, &ViewerModel::slideshowRunningChanged, [&runSig] { ++runSig; });
    QObject::connect(&vm, &ViewerModel::doublePageChanged, [&dblSig] { ++dblSig; });

    // ① 进入 = jump
    vm.jump(2);
    check(waitFor([&] { return vm.keyCount() > 0; }),
          QStringLiteral("进入(jump) 后 keys 非空（%1 张）").arg(vm.keyCount()));
    check(waitFor([&] { return !vm.imageA().isNull(); }),
          QStringLiteral("进入后 imageA 拿得到图（key→图 接线通）"));
    check(waitFor([&] { return curSig > 0; }),
          QStringLiteral("currentChanged 真的 emit（NOTIFY 没写错名）"));

    // ② next 真的推进
    const int c0 = vm.cursor();
    const QVector<QString> k0 = vm.keys();
    vm.next();
    const bool moved = waitFor([&] { return vm.cursor() != c0 || vm.keys() != k0; });
    check(moved, QStringLiteral("next 后位置变了（游标 %1 → %2）").arg(c0).arg(vm.cursor()));

    // ③ 单/双幅 = 会话级
    vm.setDoublePage(true);                                     // 换版面 ⇒ **viewer 自己**重造窗口
    check(waitFor([&] { return vm.doublePage() && dblSig > 0; }),
          QStringLiteral("setDoublePage(true) 生效且 doublePageChanged emit"));
    vm.jump(0);
    const bool two = waitFor([&] { return vm.keyCount() == 2; });
    check(two, QStringLiteral("双幅 ⇒ 本屏 2 张（实得 %1）").arg(vm.keyCount()));
    check(waitFor([&] { return !vm.imageB().isNull(); }),
          QStringLiteral("双幅 ⇒ imageB 也拿得到图"));
    vm.setDoublePage(false);                                    // 换回单幅 ⇒ 同样自己重造窗口

    // ④ 幻灯片 start/stop（键盘 Space / Esc 就是调这两个）—— 走**生产时间源**
    const int cPlay = vm.cursor();
    vm.startSlideshow();
    check(waitFor([&] { return vm.slideshowRunning() && runSig > 0; }),
          QStringLiteral("startSlideshow ⇒ running=true 且 emit 了"));
    const bool adv = waitFor([&] { return vm.cursor() > cPlay; });
    check(adv, QStringLiteral("起播 ⇒ 向前推进了（游标 %1 → %2）").arg(cPlay).arg(vm.cursor()));
    vm.stopSlideshow();
    const bool stopOk = waitFor([&] { return !vm.slideshowRunning(); });
    check(stopOk, QStringLiteral("stopSlideshow ⇒ running=false"));
    check(!vm.keys().isEmpty() && !vm.imageA().isNull(),
          QStringLiteral("停下后 keys 与图仍在（**留在当前屏**，没被清空）"));

    // ⑤ 渲染项能通过属性拿到图（QML 侧就是 `sourceImage: ui.imageA`）
    {
        GpuScalerItem item;
        item.setViewport(QSize(400, 300));
        item.setSourceImage(vm.imageA());                // 同步属性（不需要等）
        check(item.sourceSize() == vm.imageA().size(),
              QStringLiteral("GpuScalerItem.sourceImage 收到图（%1x%2 == VM 给的那张）")
                  .arg(item.sourceSize().width()).arg(item.sourceSize().height()));
        // ⚠️ **不**在这里断言 targetSize：`GpuScalerItem` 的 fit/plan 需要**真实场景图**
        //   （无窗口时它算不出来，实测偶发 -1×-1）—— 那是**组件内部**、不是本条要验的"接线"。
        //   有窗口时的画面正确性由**截图自检**覆盖（跑真实应用三次 + `--shot` 三屏）。
        check(item.viewport() == QSize(400, 300),
              QStringLiteral("viewport 传得进去（%1x%2）")
                  .arg(item.viewport().width()).arg(item.viewport().height()));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runUiWireSelfTest);
}

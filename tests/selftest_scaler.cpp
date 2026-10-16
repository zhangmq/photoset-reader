#include "testsupport.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include <cmath>

#include "layout.h"
#include "scaler.h"

// ── 核心正确性自检（可手算的期望值）──────────────────────────
// 50/50 黑白棋盘做 2:1 缩小：
//   正确答案（线性光平均）: 线性 0.5 → sRGB = 1.055*0.5^(1/2.4)-0.055 = 0.7354 → **188**
//   错误答案（gamma 空间平均）: (0+255)/2 = **128**
// 两者差 **60 级** —— 这就是"线性光缩放"能被一眼看出的原因。
// 实测（2026-10-09）：我们 188.0（精确正确）／Qt SmoothTransformation 127.0（gamma 空间）。
// ══ 核心正确性自检（ib-test-scaler；原先由 `image-browser --selftest` 跑）══════════
//   ★ 注意：旧名就叫 `--selftest`（缩放 / 光守恒 / 版面感知）——与别的套件无关。
static int runSelfTest()
{
    const int N = 64;
    QImage img(N, N, QImage::Format_RGB32);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
            img.setPixel(x, y, ((x + y) & 1) ? qRgb(255, 255, 255) : qRgb(0, 0, 0));

    auto probe = [&](scaler::Mode m, scaler::Quality q, const char *label) {
        const QImage out = scaler::resize(img, QSize(N / 2, N / 2), scaler::params(m, q));
        if (out.isNull()) { qWarning() << label << "返回空"; return; }
        // 取中心区域均值（避开边缘）
        long sum = 0, cnt = 0;
        for (int y = N / 8; y < N / 2 - N / 8; ++y)
            for (int x = N / 8; x < N / 2 - N / 8; ++x) {
                const QRgb c = out.pixel(x, y); sum += qRed(c); ++cnt;
            }
        const double avg = double(sum) / double(cnt);
        qInfo().noquote() << QStringLiteral("  %1 → 灰度均值 %2  (期望 188=线性光正确 / 128=gamma空间)")
                                 .arg(QString::fromLatin1(label), QString::number(avg, 'f', 1));
    };

    qInfo().noquote() << "[selftest] 50/50 黑白棋盘 2:1 缩小：";
    probe(scaler::Mode::LinearLight, scaler::Quality::Fast,     "linear-light + box     ");
    probe(scaler::Mode::LinearLight, scaler::Quality::Balanced, "linear-light + lanczos3");
    probe(scaler::Mode::QtSmooth,    scaler::Quality::Balanced, "qt-smooth (对照)       ");

    // ── 判定条件二：光能量守恒 ───────────────────────────────────
    // 正确的降采样判定条件是「输出的**线性光**均值 == 输入的线性光均值」。
    // 不能用「l2s(线性均值) == 均值(编码值)」——两者因 Jensen 不等式本就不等。
    auto linMean = [](const QImage &im) {
        double acc = 0; long n = 0;
        const QImage s = im.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < s.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(s.constScanLine(y));
            for (int x = 0; x < s.width(); ++x) {
                const int ch[3] = {qRed(row[x]), qGreen(row[x]), qBlue(row[x])};
                for (int c = 0; c < 3; ++c) {
                    const double v = ch[c] / 255.0;
                    acc += (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
                    ++n;
                }
            }
        }
        return n ? acc / n : 0.0;
    };

    const double inLin = linMean(img);   // 棋盘：线性均值应为 0.5
    qInfo().noquote() << QStringLiteral("\n[selftest] 光能量守恒（输入线性均值 %1，理论 0.5）：")
                             .arg(inLin, 0, 'f', 4);
    int failures = 0, checks = 0;      // checks = 总项数（失败数≠项数：footer 要报项数）
    auto check = [&](scaler::Mode m, scaler::Quality q, const char *label, double tolPct) {
        ++checks;
        const QImage out = scaler::resize(img, QSize(N / 2, N / 2), scaler::params(m, q));
        const double err = (linMean(out) - inLin) / inLin * 100.0;
        const bool ok = std::abs(err) <= tolPct;
        if (!ok) ++failures;
        qInfo().noquote() << QStringLiteral("  %1 误差 %2%  %3")
                                 .arg(QString::fromLatin1(label),
                                      QString::number(err, 'f', 2).rightJustified(7),
                                      ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    };
    check(scaler::Mode::LinearLight, scaler::Quality::Fast,     "linear-light + box     ", 1.0);
    check(scaler::Mode::LinearLight, scaler::Quality::Balanced, "linear-light + lanczos3", 1.0);
    // 阴性对照：qt-smooth 走 gamma 空间，**必须**显著丢光。
    // 它若"通过"（误差落在容差内），说明这个自检**失去了分辨力** —— 那时才该报 FAIL。
    {
        const QImage out = scaler::resize(img, QSize(N / 2, N / 2),
                                          scaler::params(scaler::Mode::QtSmooth, scaler::Quality::Balanced));
        const double err = (linMean(out) - inLin) / inLin * 100.0;
        const bool lostAsExpected = std::abs(err) > 1.0;
        ++checks;
        if (!lostAsExpected) ++failures;
        qInfo().noquote() << QStringLiteral("  %1 误差 %2%  %3")
                                 .arg(QStringLiteral("qt-smooth 阴性对照     "),
                                      QString::number(err, 'f', 2).rightJustified(7),
                                      lostAsExpected ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }

    // ── 判定条件三：版面感知（相邻页宽高比组合是最易出错处）─────────
    qInfo().noquote() << QStringLiteral("\n[selftest] 版面感知：");
    int lf = 0;
    auto expectKinds = [&](const char *label, const QVector<QSize> &sz,
                           const QVector<layout::Kind> &want) {
        const auto pages = layout::build(sz);
        QString got;
        bool ok = (pages.size() == want.size());
        for (int i = 0; i < pages.size(); ++i) {
            got += QString::fromLatin1(layout::kindName(pages.at(i).kind)) + QLatin1Char(' ');
            if (i < want.size() && pages.at(i).kind != want.at(i)) ok = false;
        }
        ++checks;
        if (!ok) ++lf;
        qInfo().noquote() << QStringLiteral("  %1 → %2  %3")
                                 .arg(QString::fromLatin1(label), got.trimmed(),
                                      ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    };

    // ① 本项目素材：全横向（跨页扫描）→ 全部单独成页，不配对
    expectKinds("全横向（跨页扫描）",
                {QSize(2893,2200), QSize(2893,2200), QSize(2893,2200)},
                {layout::Kind::WideSingle, layout::Kind::WideSingle, layout::Kind::WideSingle});
    // ② 两张连续竖图 → 配对
    expectKinds("竖+竖 → 配对",
                {QSize(1400,2000), QSize(1400,2000)},
                {layout::Kind::Spread});
    // ③ 竖 + 横 → 不配对（横的单独成页）
    expectKinds("竖+横 → 不配对",
                {QSize(1400,2000), QSize(2893,2200)},
                {layout::Kind::Single, layout::Kind::WideSingle});
    // ④ 三张竖图 → 配对 + 落单
    expectKinds("竖×3 → 配对+落单",
                {QSize(1400,2000), QSize(1400,2000), QSize(1400,2000)},
                {layout::Kind::Spread, layout::Kind::Single});
    // ⑤ 条漫（极端长宽比）→ 单独，不配对
    expectKinds("条漫 800×40000",
                {QSize(800,40000), QSize(1400,2000)},
                {layout::Kind::Webtoon, layout::Kind::Single});
    // ⑥ 条漫按绝对高度识别
    expectKinds("高 5000 的窄图 → 条漫",
                {QSize(1200,5000)},
                {layout::Kind::Webtoon});
    // ⑦ 空序列
    expectKinds("空序列", {}, {});
    // ⑧ RTL 配对顺序：右页在前
    {
        const auto pages = layout::build({QSize(1400,2000), QSize(1400,2000)});
        const bool ok = pages.size()==1 && pages[0].items.size()==2
                        && pages[0].items[0]==1 && pages[0].items[1]==0 && pages[0].rtlPair;
        ++checks;
        if (!ok) ++lf;
        qInfo().noquote() << QStringLiteral("  RTL 配对顺序（右页在前）→ %1  %2")
                                 .arg(pages.isEmpty() ? QStringLiteral("?")
                                      : QStringLiteral("%1,%2").arg(pages[0].items.value(0)).arg(pages[0].items.value(1)),
                                      ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    }

    qInfo().noquote() << (lf == 0 ? QStringLiteral("[selftest] 版面感知全部通过")
                                  : QStringLiteral("[selftest] ⚠️ 版面感知有 %1 项失败").arg(lf));

    // ★ T0 收尾批（真缺陷）：旧实现 **rc 恒 0**（失败也当通过），而且末行只看 `failures`、
    //   漏掉版面感知的 `lf` ⇒ 这里统一成 failures + lf == 0 ⇒ 0，否则 1；项数 = checks。
    const int total = failures + lf;
    qInfo().noquote() << (total == 0
        ? QStringLiteral("[selftest] 全部通过（%1 项，含 qt-smooth 阴性对照：按预期丢光）").arg(checks)
        : QStringLiteral("[selftest] ⚠️ 有 %1 项失败（%2 项）").arg(total).arg(checks));
    return total == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runSelfTest);
}

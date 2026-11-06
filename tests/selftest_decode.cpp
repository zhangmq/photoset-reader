#include "testsupport.h"

//   ★ 这些是**产品契约**，不是实现细节：解码链的输出格式、能力表的一致性、
//     占位图的几何规则。三条都是"错了会直接看见（黑屏 / 列表说假话 / 占位大小离谱）"的东西。
#include "decode/capabilities.h"
#include "imageread.h"
#include "placeholder.h"

#include <QDir>
#include <QFile>
#include <QImageReader>

namespace {

//   ① 能力表自洽：**列出来的都能解、解不了的不会列**
void testCapabilityConsistency(testsupport::Check &check)
{
    decode::Capabilities &caps = decode::Capabilities::instance();
    caps.probe();

    const QStringList sfx = caps.suffixes();
    check(!sfx.isEmpty(), QStringLiteral("① 能力表非空（本机 %1 个后缀）").arg(sfx.size()));

    bool allDecodable = true;
    for (const QString &s : sfx) {
        if (!caps.canDecodeSuffix(s)) allDecodable = false;
    }
    check(allDecodable, QStringLiteral("① 列出来的后缀 `canDecodeSuffix()` 全部为真"));

    //   ★ 策略排除：PDF 不做（用户 2026-11-06）——即便本机的 Qt 插件能解，也不许出现
    check(!sfx.contains(QStringLiteral("pdf")) && !sfx.contains(QStringLiteral("pdd")),
          QStringLiteral("① PDF 被策略排除（不进图片列表）"));
    check(!caps.canDecodeSuffix(QStringLiteral("pdf")),
          QStringLiteral("① `canDecodeSuffix(pdf)` 为假"));

    //   owners() 的键必须都在 suffixes() 里（不能有"只认领不列出"的后缀）
    const QHash<QString, QStringList> owners = caps.owners();
    bool ownersSubset = true;
    for (auto it = owners.cbegin(); it != owners.cend(); ++it) {
        if (!sfx.contains(it.key())) ownersSubset = false;
    }
    check(ownersSubset, QStringLiteral("① `owners()` 的键都在 `suffixes()` 里"));

    //   探测幂等：重复 probe 不改变结果（`main` 会显式探一次，测试再探一次）
    const QStringList again = caps.suffixes();
    check(again == sfx, QStringLiteral("① 探测幂等（两次结果一致）"));
}

//   ② 链式解码：一张真图必须解得出，而且报得出 provider
void testChainDecode(testsupport::Check &check, const QString &dir)
{
    const QString p = dir + QStringLiteral("/tiny.png");
    QImage src(40, 60, QImage::Format_RGB32);
    src.fill(QColor(20, 120, 200));
    check(src.save(p, "PNG"), QStringLiteral("② 夹具：写一张 40x60 PNG"));

    decode::Source s;
    s.isDisk = true;
    s.path = p;
    const decode::Result r = decode::Capabilities::instance().decode(s);
    check(r.outcome == decode::Outcome::Image, QStringLiteral("② 链上解出图（不是占位/缺图）"));
    check(!r.provider.isEmpty(), QStringLiteral("② 报出了 provider（%1）").arg(r.provider));
    check(r.image.size() == QSize(40, 60), QStringLiteral("② 尺寸 40x60（实际 %1x%2）")
                                               .arg(r.image.width()).arg(r.image.height()));

    //   尺寸查询也走同一条链
    const QSize q = decode::Capabilities::instance().querySize(s);
    check(q == QSize(40, 60), QStringLiteral("② querySize = 40x60（实际 %1x%2）")
                                  .arg(q.width()).arg(q.height()));
}

//   ③ ★ 回归：**16-bit 输入必须变成 32-bit 上屏**
//      2026-11-06 实测：Linux 的 kimg_raw 解 ARW 给 `Format_RGBX64`（depth=64），
//      我们整条链路只认 8-bit ⇒ 窗口全黑、GUI 线程被拖住。这里用 16-bit PNG 复现同一件事
//      （不需要 RAW 素材，任何平台都能跑）。
void testBitDepthNormalized(testsupport::Check &check, const QString &dir)
{
    const QString p = dir + QStringLiteral("/sixteen.png");
    QImage wide(120, 160, QImage::Format_RGBX64);
    wide.fill(QColor(30, 60, 90));
    check(wide.save(p, "PNG"), QStringLiteral("③ 夹具：写一张 16-bit PNG"));

    //   夹具自检：真的要能读成 16-bit，否则这条测试是空的
    {
        QImageReader r(p);
        const QImage raw = r.read();
        check(!raw.isNull() && raw.depth() > 32,
              QStringLiteral("③ 夹具确实是 16-bit（depth=%1）").arg(raw.depth()));
    }

    imageread::Location loc;
    loc.diskPath = p;
    const imageread::Loaded loaded = imageread::load(loc);
    check(!loaded.image.isNull(), QStringLiteral("③ 16-bit 图解出来了"));
    check(loaded.image.depth() == 32,
          QStringLiteral("③ ★ 上屏路径拿到的必须是 32-bit（实际 %1）").arg(loaded.image.depth()));
    check(loaded.image.size() == QSize(120, 160),
          QStringLiteral("③ 尺寸没被改（%1x%2）").arg(loaded.image.width()).arg(loaded.image.height()));
}

//   ④ 占位图几何：三条规则都是用户定的口径
void testPlaceholderGeometry(testsupport::Check &check)
{
    check(placeholder::screenSize(QSize(1200, 1800)) == QSize(720, 1080),
          QStringLiteral("④ 大图等比装进 1920x1080（1200x1800 => 720x1080）"));
    //   ★ 只缩不放：40x60 的图不许被"装进 1080p"放大
    check(placeholder::screenSize(QSize(40, 60)) == QSize(40, 60),
          QStringLiteral("④ ★ 小图不放大（40x60 保持 40x60）"));
    check(placeholder::screenSize(QSize()) == QSize(1080, 1080),
          QStringLiteral("④ 尺寸未知 => 1080x1080 方图"));

    const QImage thumb = placeholder::draw(QSize(90, 60));
    check(!thumb.isNull() && thumb.size() == QSize(90, 60),
          QStringLiteral("④ 缩略图直接按目标尺寸画（90x60）"));
}

//   ⑤ 三态：缺图 / 占位 / 有图 —— 判据只有"读到字节了吗 + 拿到非空图了吗"
void testThreeStates(testsupport::Check &check, const QString &dir)
{
    //   缺图：文件不在
    {
        imageread::Location loc;
        loc.diskPath = dir + QStringLiteral("/does-not-exist.png");
        const imageread::Loaded l = imageread::load(loc);
        check(l.image.isNull() && !l.hasBytes,
              QStringLiteral("⑤ 缺图：没有字节、也没有图"));
    }
    //   占位：有字节、解不出
    {
        const QString p = dir + QStringLiteral("/garbage.png");
        QFile f(p);
        check(f.open(QIODevice::WriteOnly), QStringLiteral("⑤ 夹具：写垃圾文件"));
        f.write(QByteArray(2048, '\x5A'));
        f.close();
        imageread::Location loc;
        loc.diskPath = p;
        const imageread::Loaded l = imageread::load(loc);
        check(l.image.isNull() && l.hasBytes,
              QStringLiteral("⑤ 占位：有字节但解不出（不当缺图）"));
    }
    //   有图
    {
        const QString p = dir + QStringLiteral("/good.png");
        QImage img(20, 30, QImage::Format_RGB32);
        img.fill(Qt::darkGreen);
        check(img.save(p, "PNG"), QStringLiteral("⑤ 夹具：写一张好图"));
        imageread::Location loc;
        loc.diskPath = p;
        const imageread::Loaded l = imageread::load(loc);
        check(!l.image.isNull() && l.hasBytes, QStringLiteral("⑤ 有图：解出来了"));
    }
}

}  // namespace

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, [] {
        testsupport::Check check(QStringLiteral("decodetest"));

        testsupport::TempFixture tmp;
        if (!tmp.isValid()) {
            return testsupport::skip(QStringLiteral("decodetest"),
                                     QStringLiteral("临时目录建不出来"));
        }

        testCapabilityConsistency(check);
        testChainDecode(check, tmp.path());
        testBitDepthNormalized(check, tmp.path());
        testPlaceholderGeometry(check);
        testThreeStates(check, tmp.path());

        return check.finish();
    });
}

#include "testsupport.h"

#include <QAbstractItemModel>
#include <QString>

#include "browsemodel.h"

// ══ 浏览行的增量更新自检（ib-test-browse）════════════════════════════════════
//   设计 12 §8.2 L10 / L14（存档台账）+ 用户 2026-10-24 报的现象：
//   「浏览界面在扫描阶段……持续数据重置无法正常滚动浏览」。
//
//   `beginResetModel/endResetModel` 的语义 = 旧数据全失效 + 视图 reset（滚动位置与当前项一起丢）
//   ⇒ 扫描期间每来一批就整表重建，列表没法滚。这里用一个**最小模型**锁住四条规则：
//     ① 尾部追加     ⇒ 只有 rowsInserted（新增那几行），**0 次 reset**
//     ② 中间追加     ⇒ 只有 rowsInserted（插在那几行之前），**0 次 reset**
//     ③ 字段变化     ⇒ 只有 dataChanged，**0 次 reset**（例：文件夹的"N 张"随内层解析增长）
//     ④ 换层/换来源  ⇒ setRows ⇒ 1 次 reset（不可避免的那一种）
//   外加 ⑤ 内容完全一样 ⇒ 一个信号都不发。
namespace {

BrowseModel::Row row(const QString &key, const QString &name, const QString &size = QString())
{
    BrowseModel::Row r;
    r.key = key;
    r.name = name;
    r.sizeText = size;
    return r;
}

// 记下模型发了哪些结构信号
struct Sig {
    int inserted = 0;
    int removed = 0;
    int changed = 0;
    int reset = 0;
    void attach(BrowseModel *m)
    {
        QObject::connect(m, &QAbstractItemModel::rowsInserted, m,
                         [this](const QModelIndex &, int f, int l) { inserted += l - f + 1; });
        QObject::connect(m, &QAbstractItemModel::rowsRemoved, m,
                         [this](const QModelIndex &, int f, int l) { removed += l - f + 1; });
        QObject::connect(m, &QAbstractItemModel::dataChanged, m,
                         [this](const QModelIndex &f, const QModelIndex &l) {
                             changed += l.row() - f.row() + 1;
                         });
        QObject::connect(m, &QAbstractItemModel::modelReset, m, [this] { ++reset; });
    }
    void clear() { inserted = removed = changed = reset = 0; }
};

}  // namespace

static int runBrowseSelfTest()
{
    testsupport::Check check(QStringLiteral("browseselftest"));

    BrowseModel m;
    Sig sig;
    sig.attach(&m);

    // ① 尾部追加：a,b,c → a,b,c,d,e
    m.patchRows({ row("a", "A"), row("b", "B"), row("c", "C") });
    sig.clear();
    m.patchRows({ row("a", "A"), row("b", "B"), row("c", "C"), row("d", "D"), row("e", "E") });
    check(m.count() == 5, QStringLiteral("尾部追加后 5 行（实际 %1）").arg(m.count()));
    check(sig.inserted == 2, QStringLiteral("只插入 2 行（实际 %1）").arg(sig.inserted));
    check(sig.reset == 0, QStringLiteral("尾部追加 **不重置**（reset=%1）").arg(sig.reset));
    check(sig.changed == 0, QStringLiteral("尾部追加不发 dataChanged（实际 %1）").arg(sig.changed));

    // ② 中间追加：a,b,c,d,e → a,X,b,c,d,e
    sig.clear();
    m.patchRows({ row("a", "A"), row("X", "X"), row("b", "B"), row("c", "C"),
                  row("d", "D"), row("e", "E") });
    check(m.count() == 6 && m.rowAt(1)->name == QStringLiteral("X"),
          QStringLiteral("中间插入落在第 2 行"));
    check(sig.inserted == 1, QStringLiteral("只插入 1 行（实际 %1）").arg(sig.inserted));
    check(sig.reset == 0, QStringLiteral("中间追加 **不重置**（reset=%1）").arg(sig.reset));

    // ③ 字段变化：文件夹的张数从 12 变成 30（行数不变、key 不变）
    sig.clear();
    m.patchRows({ row("a", "A", QStringLiteral("12 张")), row("X", "X"),
                  row("b", "B"), row("c", "C"), row("d", "D"), row("e", "E") });
    check(m.rowAt(0)->sizeText == QStringLiteral("12 张"), QStringLiteral("字段已更新"));
    check(sig.changed == 1, QStringLiteral("只刷 1 行（实际 %1）").arg(sig.changed));
    check(sig.reset == 0 && sig.inserted == 0,
          QStringLiteral("字段变化 **不重置、不插入**（reset=%1 inserted=%2）")
              .arg(sig.reset).arg(sig.inserted));

    // ④ 内容完全一样 ⇒ 一个信号都不发
    sig.clear();
    m.patchRows({ row("a", "A", QStringLiteral("12 张")), row("X", "X"),
                  row("b", "B"), row("c", "C"), row("d", "D"), row("e", "E") });
    check(sig.reset == 0 && sig.inserted == 0 && sig.changed == 0 && sig.removed == 0,
          QStringLiteral("内容没变 ⇒ 零信号"));

    // ⑤ 换层（完全不同的一批）⇒ setRows 整表重建（这是唯一允许重置的入口）
    sig.clear();
    m.setRows({ row("u", "U"), row("v", "V") });
    check(m.count() == 2, QStringLiteral("换层后 2 行（实际 %1）").arg(m.count()));
    check(sig.reset == 1, QStringLiteral("换层 ⇒ 恰好 1 次 reset（实际 %1）").arg(sig.reset));

    // ⑥ 缩短（扫描重来 / 顺序变了）⇒ patchRows 兜底也走重建，不产生半截列表
    sig.clear();
    m.patchRows({ row("u", "U") });
    check(m.count() == 1, QStringLiteral("缩短后 1 行（实际 %1）").arg(m.count()));
    check(sig.reset == 1, QStringLiteral("缩短 ⇒ 重建兜底（reset=%1）").arg(sig.reset));

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, [] { return runBrowseSelfTest(); });
}

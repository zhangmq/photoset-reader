#include "testsupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <climits>

#include "browsemodel.h"
#include "browsemodelview.h"       // ★ T2d-1b：`ui.browse`（浏览界面模型）
#include "seqwindow.h"
#include "sequence.h"
#include "uimodel.h"             // ★ 三屏/设置（这里只用它的类型与入口形状）
#include "viewermodel.h"

// ══ ⑰ 子树记录的**参考实现（oracle）**═══════════════════════════════════════
//   ★ **不再用旧递归当 oracle** —— 旧递归把"没有第一条"当成 `0`，于是空目录会把
//     所有祖先的"第一条"污染成 0（`qMin(0, …)`）。这里的口径 = `sequence.h` 上
//     `subtreeFirst/Count` 的注释（**单一事实源**）：
//       · `subtreeFirst(node)` = 该节点子树里**第一条真图**的下标；**没有图 ⇒ -1**；
//       · `subtreeCount(node)` = 该节点子树里**真图的条数**；没有 ⇒ 0；
//       · 越界（`node < 0 || node >= tree().size()`）⇒ 0；
//       · `isElement(node)` = **已扫完** 且 **子树里至少有一条图**（L40）。
//   ★ 算法与 `Sequence` 的实现**不同源**：这里**直接**枚举 `m_items`，看每条条目的
//     `parentNode` 沿父链能不能走到该节点（= "子树"的定义本身），**不做链式累加**；
//     "扫完"也从 `rows()`（行流）**按定义重算** —— 不是去读 `m_nodeClosed`。
namespace {

// `x` 的祖先链（含 x 自己）上有没有 `node`
bool ownsNode(const Sequence &s, int x, int node)
{
    const QVector<Sequence::TreeNode> &t = s.tree();
    if (node < 0 || node >= t.size()) return false;
    for (int g = 0; x >= 0 && x < t.size() && g <= t.size(); x = t.at(x).parent, ++g)
        if (x == node) return true;
    return false;
}

// 该节点子树里**第一条真图**的下标；没有图 ⇒ -1（越界 ⇒ 0，既有边界约定）
int refSubtreeFirst(const Sequence &s, int node)
{
    if (node < 0 || node >= s.tree().size()) return 0;
    const QVector<Sequence::Item> &items = s.items();
    for (int i = 0; i < items.size(); ++i)          // 下标升序 ⇒ 第一个命中的就是最小
        if (ownsNode(s, items.at(i).parentNode, node)) return i;
    return -1;                                     // ★ 没有第一条 = -1（不是 0）
}

// 该节点子树里**真图的条数**
int refSubtreeCount(const Sequence &s, int node)
{
    if (node < 0 || node >= s.tree().size()) return 0;
    const QVector<Sequence::Item> &items = s.items();
    int n = 0;
    for (int i = 0; i < items.size(); ++i)
        if (ownsNode(s, items.at(i).parentNode, node)) ++n;
    return n;
}

// 行 `i` 是不是"行 `rootOrd` 那个节点的后代"（沿行的 parentOrd 链上溯；行是前序 DFS）
bool rowUnderRow(const QVector<Sequence::Row> &rows, const QHash<int, int> &byOrd,
                 int i, int rootOrd)
{
    for (int cur = rows.at(i).parentOrd, g = 0; g <= rows.size(); ++g) {
        if (cur == rootOrd) return true;
        const int k = byOrd.value(cur, -1);
        if (k < 0) return false;
        cur = rows.at(k).parentOrd;
    }
    return false;
}

// 节点是否**已扫完**（= 后续再无属于它子树的条目）。
//   · 行流已结束（`scanFinished()`）⇒ 全部扫完（`finishAppend` 把剩下的打开节点全关掉）；
//   · 否则：**有一条排在它子树之后的行**（= 它的子树不是"行流的尾巴"）才算扫完
//     —— 只看"后面还有没有行"是不对的（最后一行所属的节点**仍然开着**）。
bool refClosed(const Sequence &s, int node)
{
    const QVector<Sequence::TreeNode> &t = s.tree();
    if (node < 0 || node >= t.size()) return false;
    if (s.scanFinished()) return true;
    const QVector<Sequence::Row> &rows = s.rows();
    int nodeRow = -1;
    for (int i = 0; i < rows.size(); ++i)
        if (rows.at(i).ord == t.at(node).ord) { nodeRow = i; break; }
    if (nodeRow < 0) return false;
    QHash<int, int> byOrd;
    for (int i = 0; i < rows.size(); ++i) byOrd.insert(rows.at(i).ord, i);
    for (int i = nodeRow + 1; i < rows.size(); ++i)
        if (!rowUnderRow(rows, byOrd, i, t.at(node).ord)) return true;
    return false;
}

bool refIsElement(const Sequence &s, int node)
{
    if (node < 0 || node >= s.tree().size()) return false;
    return refClosed(s, node) && refSubtreeCount(s, node) > 0;
}

// ★ `containerOf / containerRange / prev·nextContainerFirst` 的参考实现 = **照抄
//   `Sequence` 里的那四条算法**，只把"子树记录"换成上面的参考值 —— 用来验证
//   "记录换成新语义（-1 = 没有第一条）之后，这四条算法一行不改也仍然成立"
//   （空节点 / 未扫完节点在旧语义里是 0/0，新语义里是 -1/0，两边都必须被排除）。
int refContainerOf(const Sequence &s, int index)
{
    if (index < 0 || index >= s.count()) return -1;
    const QVector<Sequence::TreeNode> &t = s.tree();
    int best = -1, bestFirst = -1;
    for (int i = 0; i < t.size(); ++i) {
        const int first = refSubtreeFirst(s, i);
        const int cnt = refSubtreeCount(s, i);
        if (index < first || index >= first + cnt) continue;
        if (first > bestFirst) { best = i; bestFirst = first; }
    }
    return best;
}

QPair<int, int> refContainerRange(const Sequence &s, int index)
{
    const int node = refContainerOf(s, index);
    if (node < 0) return { 0, s.count() };
    return { refSubtreeFirst(s, node), refSubtreeCount(s, node) };
}

int refNextContainerFirst(const Sequence &s, int index)
{
    const QVector<Sequence::TreeNode> &t = s.tree();
    int best = -1, bestFirst = INT_MAX;
    for (int i = 0; i < t.size(); ++i) {
        const int first = refSubtreeFirst(s, i);
        if (first <= index) continue;
        if (first < bestFirst) { bestFirst = first; best = i; }
    }
    return best < 0 ? -1 : bestFirst;
}

int refPrevContainerFirst(const Sequence &s, int index)
{
    const QVector<Sequence::TreeNode> &t = s.tree();
    const int node = refContainerOf(s, index);
    const int cur = (node < 0) ? 0 : refSubtreeFirst(s, node);
    int best = -1;
    for (int i = 0; i < t.size(); ++i) {
        const int first = refSubtreeFirst(s, i);
        if (first >= cur) continue;
        if (first > best) best = first;
    }
    return best;
}

// 逐值比对：**所有节点**（含 -1 / size / size+1 三个越界值）× subtreeFirst/Count/isElement，
// 以及**所有下标**（含 -1 / count()）× containerOf/containerRange/prev/next。
//   `firstBad` = 第一处不一致的可读描述（空 = 全一致）。
bool subtreeIndexMatches(const Sequence &s, int &nodeValues, int &indexValues, QString &firstBad)
{
    const QVector<Sequence::TreeNode> &t = s.tree();
    for (int i = -1; i <= t.size() + 1; ++i) {          // 含越界：-1 / size / size+1
        const int a = s.subtreeFirst(i);
        const int b = refSubtreeFirst(s, i);
        if (a != b) {
            firstBad = QStringLiteral("subtreeFirst(%1)：新 %2 ≠ 参考 %3（节点 %4 个）")
                           .arg(i).arg(a).arg(b).arg(t.size());
            return false;
        }
        const int c = s.subtreeCount(i);
        const int d = refSubtreeCount(s, i);
        if (c != d) {
            firstBad = QStringLiteral("subtreeCount(%1)：新 %2 ≠ 参考 %3（节点 %4 个）")
                           .arg(i).arg(c).arg(d).arg(t.size());
            return false;
        }
        const bool e = s.isElement(i);
        const bool f = refIsElement(s, i);
        if (e != f) {
            firstBad = QStringLiteral("isElement(%1)：新 %2 ≠ 参考 %3（扫完 %4 / 图 %5）")
                           .arg(i).arg(e).arg(f).arg(refClosed(s, i)).arg(refSubtreeCount(s, i));
            return false;
        }
        nodeValues += 3;
    }
    for (int i = -1; i <= s.count(); ++i) {             // 含 -1 与 count()（边界外）
        const int co = s.containerOf(i);
        const int ro = refContainerOf(s, i);
        if (co != ro) {
            firstBad = QStringLiteral("containerOf(%1)：新 %2 ≠ 参考 %3").arg(i).arg(co).arg(ro);
            return false;
        }
        const QPair<int, int> cr = s.containerRange(i);
        const QPair<int, int> rr = refContainerRange(s, i);
        if (cr != rr) {
            firstBad = QStringLiteral("containerRange(%1)：新 {%2,%3} ≠ 参考 {%4,%5}")
                           .arg(i).arg(cr.first).arg(cr.second).arg(rr.first).arg(rr.second);
            return false;
        }
        const int pv = s.prevContainerFirst(i);
        const int rp = refPrevContainerFirst(s, i);
        if (pv != rp) {
            firstBad = QStringLiteral("prevContainerFirst(%1)：新 %2 ≠ 参考 %3").arg(i).arg(pv).arg(rp);
            return false;
        }
        const int nx = s.nextContainerFirst(i);
        const int rn = refNextContainerFirst(s, i);
        if (nx != rn) {
            firstBad = QStringLiteral("nextContainerFirst(%1)：新 %2 ≠ 参考 %3").arg(i).arg(nx).arg(rn);
            return false;
        }
        indexValues += 5;                                // co / cr.first / cr.second / pv / nx
    }
    return true;
}

// 合成行（`ord` 由调用方按**前序**给：父行一定在前）
Sequence::Row rowOf(int ord, int parentOrd, const QString &kind, const QString &label)
{
    Sequence::Row r;
    r.ord = ord;
    r.parentOrd = parentOrd;
    r.kind = kind;
    r.label = label;
    r.pageIndex = -1;
    if (kind == QLatin1String("image")) { r.width = 1200; r.height = 1800; }
    return r;
}

} // namespace

// ══ 查看序列自检（ib-test-viewseq；原先由 `image-browser --selftest-viewseq` 跑）══
//   验的是**序列本身**（状态推进 / 边界 / 从头锚定分组 / 取消 / 容器展开），不接 QML。
//   · 真实素材优先：`.testdata/vol01`（117 张）、`test.7z` / `test.tar`、`edge/multi.tiff`；
//     合成数据只用于**分组规则**（尺寸/缺失可以任意摆）。
//   · 断言方式：状态推进用"**轮询到条件成立 + 超时**"；规则类用纯函数直查。
static int runViewSeqSelfTest()
{
    using namespace fetch;
    using testsupport::waitFor;
    testsupport::Check check(QStringLiteral("viewseq"));
    // 轮询到扫描结束（或超时）
    auto runToEnd = [](Sequence &seq, int timeoutMs = 30000) {
        QElapsedTimer t;
        t.start();
        while (!seq.scanFinished() && t.elapsed() < timeoutMs) {
            seq.drain();
            QThread::msleep(1);
        }
        seq.drain();
        return seq.scanFinished();
    };

    const QString vol01 = QStringLiteral(".testdata/vol01");
    const QString zip7  = QStringLiteral(".testdata/test.tar");   // tar：可随机读 ✓（test.7z 是 solid ✗ 会被拒）
    const QString multi = QStringLiteral(".testdata/edge/multi.tiff");
    const QString png   = QStringLiteral(".testdata/edge/p0.png");

    // ① 真实文件夹：状态推进 + 图在追加 + 扫完计数完备
    {
        Sequence seq;
        seq.startScan(vol01, 5, false);
        check(seq.state() == Sequence::State::NotReady
                  || seq.state() == Sequence::State::ImageFound,
              QStringLiteral("① 起扫：状态 = NotReady 或 ImageFound（单调的起点）"));
        // ★ T2d-1b：`Sequence::waitForIndex` 已删（它只服务"引擎等序列"那条路）⇒
        //   用例改成**轮询 count**（同一件事：等第 0 条可用）
        const bool got = waitFor([&] { seq.drain(); return seq.count() > 0; }, 20000);
        check(got && seq.state() == Sequence::State::ImageFound,
              QStringLiteral("① **允许查看的门槛**：第一批「条目+分组都准确」 ⇒ ImageFound"));
        const int c1 = seq.count();
        bool grew = false;
        QElapsedTimer t;
        t.start();
        while (!seq.scanFinished() && t.elapsed() < 20000) {
            seq.drain();
            if (seq.count() > c1) { grew = true; break; }
            QThread::msleep(1);
        }
        check(grew, QStringLiteral("① **图在追加**：count 从 %1 继续增长").arg(c1));
        check(runToEnd(seq), QStringLiteral("① 扫完"));
        check(seq.state() == Sequence::State::Scaned, QStringLiteral("① 扫完 ⇒ Scaned"));
        check(seq.count() == 117, QStringLiteral("① 计数完备：117 张（实得 %1）").arg(seq.count()));

        int withSize = 0;
        for (int i = 0; i < seq.count(); ++i)
            if (seq.at(i).size.isValid()) ++withSize;
        check(withSize == seq.count(),
              QStringLiteral("① 像素尺寸：%1/%2 条在**扫描那一趟**拿到了").arg(withSize).arg(seq.count()));

        bool allAnchored = true;
        for (int i = 0; i < seq.count(); ++i)
            if (seq.anchorOf(i, Sequence::Mode::Double) < 0) { allAnchored = false; break; }
        check(allAnchored, QStringLiteral("① 分组完备：每条都能答出所属组的起点"));

        check(!seq.tree().isEmpty(),
              QStringLiteral("① 文件树索引（**占位**）：%1 个节点").arg(seq.tree().size()));
    }

    // ② 边界：要的位置还没扫到 ⇒ 等；扫完了 ⇒ "到头"（不是永远等）
    {
        Sequence seq;
        seq.startScan(vol01, 5, false);
        check(waitFor([&] { seq.drain(); return seq.count() > 0; }, 20000),
              QStringLiteral("② 边界：等到第 0 条可用"));
        //   超出已知范围的位置**永远不会**出现；等"扫完"就知道到头了（与旧的等待同一件事）
        const bool reachedEnd = waitFor([&] { seq.drain(); return seq.scanFinished(); }, 30000);
        const bool beyond = reachedEnd && seq.count() > 100000;
        check(!beyond && seq.scanFinished(),
              QStringLiteral("② 边界：超出范围 ⇒ 到头（不是永远等）"));
        check(seq.state() == Sequence::State::Scaned, QStringLiteral("② 边界：此时状态 = Scaned"));
    }

    // ③ **从头锚定**的分组规则（合成数据：窄 窄 宽 窄 窄 窄 未知）
    {
        Sequence seq;
        QVector<Sequence::Item> v;
        auto add = [&v](bool wide, bool unknown = false) {
            Sequence::Item it;
            it.kind = Sequence::Kind::Image;
            it.ord = v.size();
            it.label = QStringLiteral("k%1").arg(v.size());
            it.displayName = it.label;
            if (!unknown) it.size = wide ? QSize(3000, 1200) : QSize(1200, 1800);
            v << it;
        };
        add(false); add(false);            // 0,1 ⇒ 一组
        add(true);                         // 2   ⇒ 宽图单独
        add(false); add(false);            // 3,4 ⇒ 一组
        add(false);                        // 5   ⇒ 尾部单独
        add(false, true);                  // 6   ⇒ 尺寸未知 ⇒ 单独
        seq.buildFromEntries(QStringLiteral("/x"), v);

        check(seq.count() == 7, QStringLiteral("③ 静态构建：7 条都可导航"));
        check(seq.groupAt(0, Sequence::Mode::Double).members == QVector<int>({0, 1}),
              QStringLiteral("③ 双幅：窄 + 窄 ⇒ 一组 2 张"));
        check(seq.groupAt(1, Sequence::Mode::Double).cursor == 0,
              QStringLiteral("③ **从头锚定**：落到配对第二张 ⇒ 回到那一组（游标 0）"));
        check(seq.groupAt(2, Sequence::Mode::Double).members == QVector<int>({2}),
              QStringLiteral("③ 宽图 ⇒ 单独成屏"));
        check(seq.advance(0, Sequence::Mode::Double, Sequence::Dir::Next) == 2,
              QStringLiteral("③ 双幅 next ⇒ 下一组起点 2"));
        check(seq.advance(2, Sequence::Mode::Double, Sequence::Dir::Prev) == 0,
              QStringLiteral("③ 双幅 prev ⇒ 上一组起点 0（与 next 完全对称）"));
        check(seq.advance(0, Sequence::Mode::Single, Sequence::Dir::Next) == 1,
              QStringLiteral("③ 单幅 next ⇒ +1"));
        check(seq.groupAt(0, Sequence::Mode::Single).members.size() == 1,
              QStringLiteral("③ 同一游标：单幅 1 张 / 双幅 2 张 ⇒ keys **不可由游标派生**"));
        check(seq.groupAt(6, Sequence::Mode::Double).members == QVector<int>({6}),
              QStringLiteral("③ 尺寸未知 ⇒ 单独一组（缺失也不重组）"));
        check(seq.advance(6, Sequence::Mode::Double, Sequence::Dir::Next) == -1,
              QStringLiteral("③ 到尾 ⇒ -1"));
        check(seq.anchorOf(5, Sequence::Mode::Double) == 5,
              QStringLiteral("③ 尾部单独那条 ⇒ 自己就是组起点"));
    }

    // ④ 缺失：组内缺失只发存在的；整组缺失整组跳过（**游标照常按组大小算**）
    {
        Sequence seq;
        QVector<Sequence::Item> v;
        for (int i = 0; i < 6; ++i) {
            Sequence::Item it;
            it.kind = Sequence::Kind::Image;
            it.ord = i;
            it.label = QStringLiteral("k%1").arg(i);
            it.displayName = it.label;
            it.size = QSize(1200, 1800);
            it.missing = (i == 1 || i == 4 || i == 5);
            v << it;
        }
        seq.buildFromEntries(QStringLiteral("/x"), v);
        const Sequence::Group g0 = seq.groupAt(0, Sequence::Mode::Double);
        check(g0.members == QVector<int>({0}) && g0.cursor == 0,
              QStringLiteral("④ 组内缺失 ⇒ 只发存在的那些，游标照常 = 0"));
        check(seq.advance(0, Sequence::Mode::Double, Sequence::Dir::Next) == 2,
              QStringLiteral("④ 缺失也占位 ⇒ 前进量仍是本组大小（2）"));
        const Sequence::Group g4 = seq.groupAt(4, Sequence::Mode::Double);
        check(g4.members.isEmpty(),
              QStringLiteral("④ 整组缺失（4、5）⇒ 后面没有可用组 ⇒ 空"));
        const Sequence::Group g2 = seq.groupAt(2, Sequence::Mode::Double);
        check(g2.cursor == 2 && g2.members == QVector<int>({2, 3}),
              QStringLiteral("④ 整组缺失整组跳过：从 0 找 ⇒ 落到第 2 组"));
    }

    // ⑤ 用户终止扫描 ⇒ ScanStoped（**处理规则与 Scaned 完全相同**）
    {
        Sequence seq;
        seq.startScan(vol01, 5, false);
        check(waitFor([&] { seq.drain(); return seq.count() > 0; }, 20000),
              QStringLiteral("⑤ 取消：先等到有图"));
        const int before = seq.count();
        seq.cancelScan();
        check(seq.state() == Sequence::State::ScanStoped, QStringLiteral("⑤ 取消 ⇒ ScanStoped"));
        check(seq.scanFinished(), QStringLiteral("⑤ ScanStoped 也算「扫描已结束」"));
        check(seq.count() >= before && before > 0,
              QStringLiteral("⑤ 已扫到的条目仍在（%1 → %2）").arg(before).arg(seq.count()));
        check(!seq.groupAt(0, Sequence::Mode::Double).members.isEmpty(),
              QStringLiteral("⑤ 取消后仍能正常取组（规则同 Scaned）"));
    }

    // ⑥ 压缩包：**等同于文件夹**（展开、要里面的图）
    {
        Sequence seq;
        seq.startScan(zip7, 5, false);
        check(runToEnd(seq), QStringLiteral("⑥ 压缩包：扫完"));
        check(seq.state() == Sequence::State::Scaned && seq.count() == 3,
              QStringLiteral("⑥ 压缩包 ⇒ 3 个条目（实得 %1）").arg(seq.count()));
        const Sequence::Item &it0 = seq.at(0);
        check(it0.members() == QStringList{ QStringLiteral("p0.png") },
              QStringLiteral("⑥ 成员链 = [p0.png]（实得 %1）").arg(it0.members().join(QLatin1Char('/'))));
        check(!it0.diskPath().isEmpty() && QFileInfo(it0.diskPath()).exists(),
              QStringLiteral("⑥ 磁盘路径 = 最外层压缩包，且真实存在"));
        check(it0.size.isValid(), QStringLiteral("⑥ 压缩包内条目的像素尺寸也读到了（%1x%2）")
                                      .arg(it0.size.width()).arg(it0.size.height()));
        check(!Sequence::image(it0).isNull(),
              QStringLiteral("⑥ 压缩包内条目能解码（唯一实现 QImageReader）"));
        check(seq.at(0).cacheKey() != seq.at(1).cacheKey(),
              QStringLiteral("⑥ 缓存键（ord）各不相同"));
    }

    // ⑦ 嵌套压缩包：**解到临时文件**再当普通压缩包读（深度上限 = recurseDepth）
    {
        QTemporaryDir fx;
        bool built = false;
        if (fx.isValid()) {
            // 用 7z 造"压缩包里的压缩包"（测试机架；7z 不在就**跳过**，不误报通过）
            QProcess p;
            p.setWorkingDirectory(QStringLiteral(".testdata/edge"));
            p.start(QStringLiteral("7z"),
                    { QStringLiteral("a"), QStringLiteral("-t7z"), QStringLiteral("-ms=off"),
                      fx.filePath(QStringLiteral("inner.7z")),
                      QStringLiteral("p0.png"), QStringLiteral("p1.png") });
            p.waitForFinished(60000);
            built = (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0
                     && QFileInfo::exists(fx.filePath(QStringLiteral("inner.7z"))));
            if (built) {
                QProcess q;
                q.setWorkingDirectory(fx.path());
                q.start(QStringLiteral("7z"),
                        { QStringLiteral("a"), QStringLiteral("-t7z"), QStringLiteral("-ms=off"),
                          fx.filePath(QStringLiteral("outer.7z")), QStringLiteral("inner.7z") });
                q.waitForFinished(60000);
                built = (q.exitStatus() == QProcess::NormalExit && q.exitCode() == 0
                         && QFileInfo::exists(fx.filePath(QStringLiteral("outer.7z"))));
            }
        }
        if (!built) {
            qInfo().noquote() << QStringLiteral("[viewseq] 跳过：造不出嵌套压缩包样例（需要 7z）");
        } else {
            Sequence seq;
            seq.startScan(fx.filePath(QStringLiteral("outer.7z")), 5, false);
            check(runToEnd(seq), QStringLiteral("⑦ 嵌套：扫完"));
            check(seq.count() == 2, QStringLiteral("⑦ 嵌套压缩包 ⇒ 2 个条目（实得 %1）").arg(seq.count()));
            if (seq.count() >= 1) {
                const Sequence::Item &it = seq.at(0);
                check(it.members() == QStringList({ QStringLiteral("inner.7z"),
                                                    QStringLiteral("p0.png") })
                          && QFileInfo(it.diskPath()).exists(),
                      QStringLiteral("⑦ 内层条目：成员链 = [inner.7z, p0.png]，磁盘路径 = 外层包"));
                check(!Sequence::image(it).isNull(),
                      QStringLiteral("★ ⑦ 嵌套压缩包**不解到临时文件**也能解码"));
                check(seq.tree().size() == 2 && seq.tree().at(1).parent == 0
                          && seq.tree().at(1).kind == Sequence::TreeNode::Kind::Archive,
                      QStringLiteral("⑦ 文件树：内层压缩包是外层的**子节点**"));
            }
        }
    }

    // ⑧ 多页文件 = 容器（N 页 = N 个条目）
    {
        Sequence seq;
        seq.startScan(multi, 5, false);
        check(runToEnd(seq), QStringLiteral("⑧ 多页文件：扫完"));
        check(seq.state() == Sequence::State::Scaned, QStringLiteral("⑧ 多页文件 ⇒ Scaned"));
        check(seq.count() > 1, QStringLiteral("⑧ 多页文件当容器 ⇒ %1 个条目").arg(seq.count()));
        if (seq.count() > 1) {
            check(seq.at(0).pageIndex() == 0 && seq.at(1).pageIndex() == 1,
                  QStringLiteral("⑧ 页号递增（%1 → %2）").arg(seq.at(0).pageIndex()).arg(seq.at(1).pageIndex()));
            check(seq.at(0).size.isValid(), QStringLiteral("⑧ 页尺寸读到了"));
            check(!Sequence::image(seq.at(1)).isNull(), QStringLiteral("⑧ 第 2 页能解码"));
        }
    }

    // ⑨ 单张图 ⇒ 1 条序列
    {
        Sequence seq;
        seq.startScan(png, 5, false);
        check(runToEnd(seq), QStringLiteral("⑨ 单张图：扫完"));
        check(seq.state() == Sequence::State::Scaned && seq.count() == 1,
              QStringLiteral("⑨ 单张图 ⇒ 1 条，Scaned（实得 %1）").arg(seq.count()));
        check(seq.advance(0, Sequence::Mode::Double, Sequence::Dir::Next) == -1,
              QStringLiteral("⑨ 单张图：没有下一组"));
    }

    // ⑩ 图像查看页的 **Loading**：只有两种情况才 loading（正常"图还没解码好"不进 loading）
    {
        using namespace pipetest;

        // ① 序列还在 `notReady`（还没有任何图片数据）⇒ loading
        //   ★ T2d-1c-①：**判定条件的唯一实现在 `ViewerModel`**（L37：窗口里 `count()` /
        //     `scanFinished()` / 这段覆盖完不完整 + 引擎的"正在等推进"）⇒ 用例只看 viewer 的 `loading()`。
        Sequence notReady;                          // 默认构造 = notReady 且"扫描未结束"
        ViewerModel vm1(7, Mode::Single, nullptr, decoder({}));
        vm1.setSequence(&notReady);
        vm1.jump(0);                                // 生产顺序：装好来源就"要第一屏"
        //   ★ T2d-1d-①：判定条件 = **要显示的那一屏还有"未备好"的** —— 序列一条都还没有
        //     ⇒ 那一屏答不出来 ⇒ loading。
        check(vm1.loading(), QStringLiteral("⑩ notReady（还没有任何图片数据）⇒ loading = true"));

        // ② 序列信息完备（静态构建 ⇒ Scaned）⇒ 不 loading
        Sequence done;
        fill(done, mk(3));
        ViewerModel vm2(7, Mode::Single, nullptr, decoder({}));
        vm2.setSequence(&done);
        vm2.jump(0);
        check(waitFor([&] { return vm2.keyCount() > 0; }), QStringLiteral("⑩ 完备序列：先上屏"));
        check(!vm2.loading(), QStringLiteral("⑩ 序列信息完备 ⇒ loading = false"));

        // ③ **到头 ≠ loading**：到最后一屏再 next ⇒ 维持现状，不许进 loading（L43 的边界）
        vm2.jump(2);
        check(waitFor([&] { return vm2.cursor() == 2; }), QStringLiteral("⑩ 跳到最后一条"));
        const QVector<QString> keep = vm2.keys();
        vm2.next();
        check(!waitFor([&] { return vm2.loading(); }, 200),
              QStringLiteral("⑩ **到头 ≠ loading**（维持现状，不闪 loading）"));
        check(vm2.keys() == keep, QStringLiteral("⑩ 到头后当前屏不变（维持现状）"));
    }

    // ⑪ 文件树索引（**浏览界面的唯一数据源**）：节点 = 目录 / 压缩包 / 多页文件；
    //    `firstItem/itemCount` = 该节点**直接**产出的图（子容器的另算 ⇒ 分节不重叠）
    {
        Sequence seq;
        seq.startScan(vol01, 5, false);
        runToEnd(seq);
        check(seq.tree().size() == 1 && seq.tree().at(0).parent == -1,
              QStringLiteral("⑪ 文件夹：根目录一个节点（实得 %1 个）").arg(seq.tree().size()));
        check(seq.tree().at(0).firstItem == 0 && seq.tree().at(0).itemCount == seq.count(),
              QStringLiteral("⑪ 根节点的**直接**条目 = 全部 %1 条").arg(seq.tree().at(0).itemCount));
    }
    {
        Sequence seq;
        seq.startScan(zip7, 5, false);
        runToEnd(seq);
        check(seq.tree().size() == 1
                  && seq.tree().at(0).kind == Sequence::TreeNode::Kind::Archive,
              QStringLiteral("⑪ 压缩包：一个归档节点"));
        check(seq.tree().at(0).itemCount == 3, QStringLiteral("⑪ 归档节点的直接条目 = 3"));
    }
    {
        Sequence seq;
        seq.startScan(multi, 5, false);
        runToEnd(seq);
        check(seq.tree().size() == 1
                  && seq.tree().at(0).kind == Sequence::TreeNode::Kind::Archive
                  && seq.tree().at(0).itemCount == seq.count(),
              QStringLiteral("⑪ 多页文件：一个 Archive 容器节点（%1 页）").arg(seq.count()));
    }
    {
        // 子目录：每层一个节点 + 父链指向前面的节点 + **各节点直接条目之和 = 总数**（分节不重叠）
        QTemporaryDir fx;
        if (!fx.isValid()) {
            qInfo().noquote() << QStringLiteral("[viewseq] 跳过：建不出临时目录");
        } else {
            QDir().mkpath(fx.filePath(QStringLiteral("a/b")));
            QDir().mkpath(fx.filePath(QStringLiteral("c")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("r.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p1.png"), fx.filePath(QStringLiteral("a/x.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p2.png"), fx.filePath(QStringLiteral("a/b/y.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("c/z.png")));
            Sequence seq;
            seq.startScan(fx.path(), 5, false);
            runToEnd(seq);
            check(seq.tree().size() == 4,
                  QStringLiteral("⑪ 子目录：4 个节点（根 + a + a/b + c，实得 %1）").arg(seq.tree().size()));
            int roots = 0, totalItems = 0;
            bool parentOk = true;
            for (int i = 0; i < seq.tree().size(); ++i) {
                const Sequence::TreeNode &n = seq.tree().at(i);
                if (n.parent < 0) ++roots;
                else if (n.parent >= i) parentOk = false;
                totalItems += n.itemCount;
            }
            check(roots == 1 && parentOk,
                  QStringLiteral("⑪ 树：一个根 + 父链都指向**前面**的节点"));
            check(totalItems == seq.count(),
                  QStringLiteral("⑪ 各节点**直接**条目之和 = 条目总数（%1/%2）")
                      .arg(totalItems).arg(seq.count()));
        }
    }

    // ⑫ 浏览界面（**层级浏览**）：数据源 = 文件树索引；一次只装"当前这一层"
    {
        using namespace pipetest;
        QTemporaryDir fx;
        if (!fx.isValid()) {
            qInfo().noquote() << QStringLiteral("[viewseq] 跳过：建不出临时目录");
        } else {
            QDir().mkpath(fx.filePath(QStringLiteral("第01话")));
            QDir().mkpath(fx.filePath(QStringLiteral("第02话")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("cover.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("第01话/001.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p1.png"), fx.filePath(QStringLiteral("第01话/002.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p2.png"), fx.filePath(QStringLiteral("第02话/003.png")));
            QFile::copy(QStringLiteral(".testdata/test.7z"), fx.filePath(QStringLiteral("extra.7z")));

            Sequence seq;
            seq.startScan(fx.path(), 5, false);
            runToEnd(seq);
            // ★ T2d-1c-①：浏览 = **新家 `ui.browse`**（`app::BrowseModel`）——它**只吃
            //   "序列 + 当前条目"**（那一层由 `fetch::browseLayerOf` 从序列取，**不经过有界窗口（`fetch::Window`）**）。
            //   用例里没有 `UiModel` ⇒ 这一段是它的**最小等价接线**（进入容器时把游标落到
            //   本层第一条；选中 ⇒ 让 viewer 跳过去），照着 `UiModel` 的那两个槽写。
            ViewerModel vm(7, Mode::Single, nullptr, decoder({}));
            app::BrowseModel browse;
            browse.setSequence(&seq);
            QObject::connect(&browse, &app::BrowseModel::layerChanged, [&](int reason) {
                if (reason != 1) return;
                const int node = browse.node();
                const int first = seq.subtreeFirst(node);
                const int count = seq.subtreeCount(node);
                if (count <= 0 || first < 0) return;
                const int cur = vm.cursor();
                if (cur < first || cur >= first + count) vm.jump(first);
            });
            QObject::connect(&browse, &app::BrowseModel::rowSelected,
                             [&](int itemIndex) { vm.jump(itemIndex); });
            QObject::connect(&vm, &ViewerModel::currentChanged,
                             [&] { browse.setCursor(vm.cursor()); });
            vm.setSequence(&seq);
            vm.jump(0);
            browse.setCursor(0);
            waitFor([&] { return vm.keyCount() > 0; });

            // 根这一层：图片（自然序）→ 压缩包 → 子目录
            const int rootRows = browse.items()->rowCount();
            check(rootRows == 4, QStringLiteral("⑫ 根这一层 4 行（实得 %1）").arg(rootRows));
            const auto kindAt = [&](int r) {
                return browse.items()->index(r, 0).data(::BrowseModel::KindRole).toString();
            };
            const auto nameAt = [&] (int r) {
                return browse.items()->index(r, 0).data(::BrowseModel::NameRole).toString();
            };
            check(kindAt(0) == QLatin1String("image") && nameAt(0) == QLatin1String("cover.png")
                      && kindAt(1) == QLatin1String("archive")
                      && kindAt(2) == QLatin1String("folder") && kindAt(3) == QLatin1String("folder"),
                  QStringLiteral("⑫ 行的种类与顺序：图片 → 压缩包 → 子目录（%1 / %2 / %3 / %4）")
                      .arg(nameAt(0), nameAt(1), nameAt(2), nameAt(3)));
            check(!browse.canGoUp() && browse.crumbs().size() == 1
                      && browse.rangeLabel() == QFileInfo(fx.path()).fileName(),
                  QStringLiteral("⑫ 根：不能上一级、面包屑 1 段、范围 = %1").arg(browse.rangeLabel()));
            check(browse.browseRow() == 0,
                  QStringLiteral("⑫ 高亮行 = **派生**（当前条目 0 落在第 0 行）"));

            // 进入第 01 话（第 2 行）⇒ 这一层只剩它的两条
            browse.activate(2);
            const int subRows = browse.items()->rowCount();
            check(subRows == 2 && browse.canGoUp(),
                  QStringLiteral("⑫ 进入子目录 ⇒ 只剩 2 行、可上一级（实得 %1）").arg(subRows));
            check(browse.crumbs().size() == 2
                      && browse.crumbs().at(1).toMap().value(QStringLiteral("label")).toString()
                             == QStringLiteral("第01话"),
                  QStringLiteral("⑫ 面包屑 = 根 / 第01话"));
            // 第 01 话的第一条图 = 序列里的第 **4** 条（前面还有 cover.png + extra.7z 的 3 条）
            const bool landed = waitFor([&] { return vm.cursor() == 4; });
            waitFor([&] { return browse.browseRow() == 0; }, 500);
            check(landed && browse.browseRow() == 0,
                  QStringLiteral("⑫ 进入后游标落到本层第一条图（条目 %1 / 高亮行 %2）")
                      .arg(vm.cursor()).arg(browse.browseRow()));

            // 上一级 ⇒ 回到根
            browse.up();
            const int upRows = browse.items()->rowCount();
            check(upRows == 4 && !browse.canGoUp(),
                  QStringLiteral("⑫ 上一级 ⇒ 回到根（4 行、不能上一级）"));

            // 选中一条图（第 0 行 = cover.png）⇒ **唯一的那个游标**跟着走
            browse.activate(0);
            check(waitFor([&] { return vm.cursor() == 0; }),
                  QStringLiteral("⑫ 选中图行 ⇒ 游标 = 那条图（%1）").arg(vm.cursor()));

            // 面包屑点击回到某一层
            browse.activate(3);                         // 进第 02 话
            check(browse.canGoUp(), QStringLiteral("⑫ 进第 02 话"));
            browse.crumb(0);
            check(browse.items()->rowCount() == 4 && !browse.canGoUp(),
                  QStringLiteral("⑫ 点面包屑首段 ⇒ 回到根"));
        }
    }

    // ⑬ 容器查询 + 屏号（**上下文件夹 / 文件夹循环 / 顶栏序号**要用）
    {
        using namespace pipetest;
        QTemporaryDir fx;
        if (!fx.isValid()) {
            qInfo().noquote() << QStringLiteral("[viewseq] 跳过：建不出临时目录");
        } else {
            QDir().mkpath(fx.filePath(QStringLiteral("第01话")));
            QDir().mkpath(fx.filePath(QStringLiteral("第02话")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("cover.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("第01话/001.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p1.png"), fx.filePath(QStringLiteral("第01话/002.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p2.png"), fx.filePath(QStringLiteral("第02话/003.png")));
            QFile::copy(QStringLiteral(".testdata/test.7z"), fx.filePath(QStringLiteral("extra.7z")));
            Sequence seq;
            seq.startScan(fx.path(), 5, false);
            runToEnd(seq);
            // 条目顺序：cover(0) · extra.7z 的三条(1,2,3) · 第01话 两条(4,5) · 第02话 一条(6)
            check(seq.containerOf(0) >= 0 && seq.containerOf(1) >= 0
                      && seq.containerOf(1) != seq.containerOf(4),
                  QStringLiteral("⑬ 最内层容器：0 与 1 同根、4 属于另一个容器"));
            check(seq.nextContainerFirst(0) == 1 && seq.nextContainerFirst(4) == 6
                      && seq.nextContainerFirst(6) == -1,
                  QStringLiteral("⑬ [下一文件夹]：0 → 1（extra.7z）、4 → 6（第02话）、6 → 到头 −1"));
            check(seq.prevContainerFirst(4) == 1 && seq.prevContainerFirst(1) == 0,
                  QStringLiteral("⑬ [上一文件夹]：4 → 1、1 → 0（根）"));
            const QPair<int, int> r = seq.containerRange(5);
            check(r.first == 4 && r.second == 2,
                  QStringLiteral("⑬ 文件夹循环范围：第01话 = [%1, +%2)").arg(r.first).arg(r.second));
            check(seq.containerRange(0).second == seq.count(),
                  QStringLiteral("⑬ 根的范围 = 整条序列（%1）").arg(seq.containerRange(0).second));
        }

        // 屏号 / 屏数（合成窄图：5 条 ⇒ 双幅 3 屏）
        Sequence s2;
        fill(s2, mk(5));
        check(s2.screenCount(Mode::Single) == 5 && s2.screenCount(Mode::Double) == 3,
              QStringLiteral("⑬ 屏数：单幅 5 屏 / 双幅 3 屏"));
        check(s2.screenOf(0, Mode::Double) == 0 && s2.screenOf(1, Mode::Double) == 0
                  && s2.screenOf(2, Mode::Double) == 1 && s2.screenOf(4, Mode::Double) == 2,
              QStringLiteral("⑬ 屏号：0/1 → 屏 0、2 → 屏 1、4 → 屏 2（配对第二张与第一张同屏）"));
    }

    // ⑭ 归档内**子目录**：每层都要扫到（曾经只扫到第一个目录 —— 真 bug，已修）
    {
        QTemporaryDir fx;
        if (!fx.isValid()) {
            qInfo().noquote() << QStringLiteral("[viewseq] 跳过：建不出临时目录");
        } else {
            QDir().mkpath(fx.filePath(QStringLiteral("ch01")));
            QDir().mkpath(fx.filePath(QStringLiteral("ch02")));
            QDir().mkpath(fx.filePath(QStringLiteral("ch03")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("cover.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("ch01/001.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p1.png"), fx.filePath(QStringLiteral("ch01/002.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p2.png"), fx.filePath(QStringLiteral("ch02/001.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("ch03/001.png")));
            const QString arc = fx.filePath(QStringLiteral("packed.7z"));
            bool built = false;
            {
                QProcess z;
                z.setWorkingDirectory(fx.path());
                z.start(QStringLiteral("7z"), { QStringLiteral("a"), QStringLiteral("-t7z"),
                                                QStringLiteral("-ms=off"), arc,
                                                QStringLiteral("cover.png"), QStringLiteral("ch01"),
                                                QStringLiteral("ch02"), QStringLiteral("ch03") });
                z.waitForFinished(60000);
                built = (z.exitStatus() == QProcess::NormalExit && z.exitCode() == 0
                         && QFileInfo::exists(arc));
            }
            if (!built) {
                qInfo().noquote() << QStringLiteral("[viewseq] 跳过：造不出带子目录的压缩包（需要 7z）");
            } else {
                Sequence seq;
                seq.startScan(arc, 5, false);
                runToEnd(seq);
                check(seq.count() == 5,
                      QStringLiteral("⑭ 归档内子目录：5 条全扫到（实得 %1 —— 曾经只有第一个目录的 2 条）")
                          .arg(seq.count()));
                QSet<QString> names;
                for (int i = 0; i < seq.count(); ++i) names.insert(seq.at(i).displayName);
                check(names.contains(QStringLiteral("001.png")) && names.contains(QStringLiteral("002.png")),
                      QStringLiteral("⑭ 三个目录的图都在（%1）").arg(QStringList(names.values()).join(QLatin1Char(','))));
                check(seq.tree().size() == 4,
                      QStringLiteral("⑭ 文件树：归档根 + 3 个子目录（实得 %1）").arg(seq.tree().size()));
            }
        }
    }

    // ⑮ 支持策略：**能随机读的才支持**（用户 2026-10-13）
    {
        QTemporaryDir fx;
        if (!fx.isValid()) {
            qInfo().noquote() << QStringLiteral("[viewseq] 跳过：建不出临时目录");
        } else {
            QDir().mkpath(fx.filePath(QStringLiteral("d")));
            for (int i = 0; i < 6; ++i)
                QFile::copy(QStringLiteral(".testdata/edge/p0.png"),
                            fx.filePath(QStringLiteral("d/p%1.png").arg(i)));
            auto run7z = [&](const QStringList &args) {
                QProcess z;
                z.setWorkingDirectory(fx.path());
                z.start(QStringLiteral("7z"), args);
                z.waitForFinished(60000);
                return z.exitStatus() == QProcess::NormalExit && z.exitCode() == 0;
            };
            const QString solid = fx.filePath(QStringLiteral("solid.7z"));
            const QString loose = fx.filePath(QStringLiteral("loose.7z"));
            const bool b1 = run7z({ QStringLiteral("a"), QStringLiteral("-t7z"),
                                    QStringLiteral("-ms=on"), solid, QStringLiteral("d") });
            const bool b2 = run7z({ QStringLiteral("a"), QStringLiteral("-t7z"),
                                    QStringLiteral("-ms=off"), loose, QStringLiteral("d") });
            const bool b3 = run7z({ QStringLiteral("a"), QStringLiteral("-ttar"),
                                    QStringLiteral("-tgzip"), fx.filePath(QStringLiteral("d.tgz")),
                                    QStringLiteral("d") });
            if (!b1 || !b2 || !b3) {
                qInfo().noquote() << QStringLiteral("[viewseq] 跳过：造不出压缩包样例（需要 7z）");
            } else {
                {   // solid 7z ⇒ 不支持
                    Sequence seq;
                    seq.startScan(solid, 5, false);
                    check(seq.scanFinished() && seq.count() == 0
                              && !seq.unsupportedReason().isEmpty(),
                          QStringLiteral("⑮ solid 7z ⇒ **不支持**（%1）").arg(seq.unsupportedReason()));
                }
                {   // 非 solid 7z ⇒ 支持（6 张全扫到）
                    Sequence seq;
                    seq.startScan(loose, 5, false);
                    runToEnd(seq);
                    check(seq.unsupportedReason().isEmpty() && seq.count() == 6,
                          QStringLiteral("⑮ 非 solid 7z ⇒ **支持**（%1 张）").arg(seq.count()));
                }
                {   // tar.gz（流式）⇒ 不支持
                    Sequence seq;
                    seq.startScan(fx.filePath(QStringLiteral("d.tgz")), 5, false);
                    check(seq.scanFinished() && seq.count() == 0
                              && seq.unsupportedReason().contains(QLatin1String("流式")),
                          QStringLiteral("⑮ tar.gz ⇒ **不支持**（%1）").arg(seq.unsupportedReason()));
                }
            }
        }
    }

    // ⑯ ★ 交接不变量（设计 14 §9.4）：**终态不可被 ImageFound / Scaned 覆盖**
    //    · (a) 真并发：起扫 → 边跑边 drain；状态必须**单调**、终点必须是终态
    //          （生产者写终态 + 消费者只在 NotReady 时 CAS ⇒ 不存在"终态被顶回 ImageFound"）；
    //    · (b) **先置终态、再走一次 drain 的跃迁逻辑**：取消 ⇒ ScanStoped（生产者已 join）；
    //          随后的 drain（收尾 `finishAppend` + ImageFound 分支）**不得**把它翻成 Scaned。
    {
        auto rank = [](Sequence::State s) {
            return s == Sequence::State::NotReady ? 0
                 : s == Sequence::State::ImageFound ? 1 : 2;
        };
        {   // (a) 边扫边 drain：单调 + 终点是终态
            Sequence seq;
            seq.startScan(vol01, 5, false);
            int prev = rank(seq.state());
            bool monotone = true;
            QElapsedTimer t;
            t.start();
            while (!seq.scanFinished() && t.elapsed() < 30000) {
                seq.drain();
                const int now = rank(seq.state());
                if (now < prev) monotone = false;
                prev = now;
                QThread::msleep(1);
            }
            seq.drain();
            check(monotone && seq.state() == Sequence::State::Scaned,
                  QStringLiteral("⑯(a) 边扫边 drain：状态单调且终点 = Scaned（**终态不被 ImageFound 顶掉**）"));
        }
        {   // (b) 先置终态（ScanStoped）再 drain：状态一个字节都不许变
            Sequence seq;
            seq.startScan(vol01, 5, false);
            seq.cancelScan();                            // ⇒ 终态 ScanStoped（生产者已 join）
            const Sequence::State before = seq.state();
            seq.drain();                                 // 走一遍跃迁逻辑（收尾 + ImageFound 分支）
            check(before == Sequence::State::ScanStoped
                      && seq.state() == Sequence::State::ScanStoped,
                  QStringLiteral("⑯(b) 先置终态再 drain：ScanStoped 不得被翻成 Scaned/ImageFound（%1 ⇒ %2）")
                      .arg(int(before)).arg(int(seq.state())));
        }
    }

    // ⑰ ★ **子树记录**（`subtreeFirst/subtreeCount` + `isElement`）的新语义：
    //    与**从 `m_items` + `parentNode` + 树直接算的**参考实现（见文件头的 oracle；
    //    **不是**旧递归 —— 旧递归带"空容器污染 0"的毛病）在**所有节点 / 所有下标**上逐值比对；
    //    `containerOf / containerRange / prev / next` 是受益方，一并比（验证"算法一行不改"）。
    //    形状：多层嵌套（含空目录、深浅不一）/ 只有根 / 空文件夹 / 深链 / 无树来源 / 节点越界 /
    //          **扫描分批追加**（边扫边比 **+ 扫完前后的行为凭据**）/ **多次收尾** /
    //          换形状重建 / 拷贝赋值。
    {
        using namespace pipetest;
        int nodeValues = 0;      // 已比对的节点值处数（每个节点 3 个值：first/count/element）
        int indexValues = 0;     // 已比对的下标值处数（每个下标 5 个值：co/range×2/prev/next）

        auto eqCheck = [&](const Sequence &s, const QString &what) {
            QString bad;
            int nv = 0, iv = 0;
            const bool ok = subtreeIndexMatches(s, nv, iv, bad);
            nodeValues += nv;
            indexValues += iv;
            check(ok, ok ? QStringLiteral("⑰ %1：逐值等价（节点 %2 个 / 条目 %3 条；本轮比 %4+%5 处）")
                               .arg(what).arg(s.tree().size()).arg(s.count()).arg(nv).arg(iv)
                         : QStringLiteral("⑰ %1：**不一致** —— %2").arg(what, bad));
        };

        // (a) 多层嵌套文件夹 + 空目录（有节点无条目）+ 层级深浅不一 + 根级条目
        QVector<Sequence::Row> n1;
        {
            int o = 0;
            const int nA = o++, kA0 = o++, kA1 = o++, nA1 = o++, nA2 = o++, kA20 = o++,
                      nA2x = o++, kA2x0 = o++, kA2x1 = o++, kA2x2 = o++, nA2y = o++,
                      nB = o++, nC = o++, kC0 = o++, nC1 = o++, nC1a = o++, kC1a0 = o++,
                      kC1a1 = o++, kRoot = o++;
            n1 << rowOf(nA, -1, QStringLiteral("folder"), QStringLiteral("A"));
            n1 << rowOf(kA0, nA, QStringLiteral("image"), QStringLiteral("A-0"));
            n1 << rowOf(kA1, nA, QStringLiteral("image"), QStringLiteral("A-1"));
            n1 << rowOf(nA1, nA, QStringLiteral("folder"), QStringLiteral("A1"));      // 空目录
            n1 << rowOf(nA2, nA, QStringLiteral("folder"), QStringLiteral("A2"));
            n1 << rowOf(kA20, nA2, QStringLiteral("image"), QStringLiteral("A2-0"));
            n1 << rowOf(nA2x, nA2, QStringLiteral("folder"), QStringLiteral("A2x"));
            n1 << rowOf(kA2x0, nA2x, QStringLiteral("image"), QStringLiteral("A2x-0"));
            n1 << rowOf(kA2x1, nA2x, QStringLiteral("image"), QStringLiteral("A2x-1"));
            n1 << rowOf(kA2x2, nA2x, QStringLiteral("image"), QStringLiteral("A2x-2"));
            n1 << rowOf(nA2y, nA2, QStringLiteral("folder"), QStringLiteral("A2y"));   // 空目录（更深）
            n1 << rowOf(nB, -1, QStringLiteral("folder"), QStringLiteral("B"));        // 根级空目录
            n1 << rowOf(nC, -1, QStringLiteral("folder"), QStringLiteral("C"));
            n1 << rowOf(kC0, nC, QStringLiteral("image"), QStringLiteral("C-0"));
            n1 << rowOf(nC1, nC, QStringLiteral("folder"), QStringLiteral("C1"));      // 空目录
            n1 << rowOf(nC1a, nC1, QStringLiteral("folder"), QStringLiteral("C1a"));
            n1 << rowOf(kC1a0, nC1a, QStringLiteral("image"), QStringLiteral("C1a-0"));
            n1 << rowOf(kC1a1, nC1a, QStringLiteral("image"), QStringLiteral("C1a-1"));
            n1 << rowOf(kRoot, -1, QStringLiteral("image"), QStringLiteral("root-0"));
        }
        Sequence seqA;
        seqA.buildFromRows(QStringLiteral("/synth"), n1, false, QStringLiteral("root"));
        eqCheck(seqA, QStringLiteral("多层嵌套 + 空目录 + 深浅不一（10 节点 10 条目）"));

        // (b) 只有根：1 个节点、条目全挂在根上
        Sequence seqOnlyRoot;
        {
            QVector<Sequence::Row> r;
            r << rowOf(0, -1, QStringLiteral("image"), QStringLiteral("solo-0"))
              << rowOf(1, -1, QStringLiteral("image"), QStringLiteral("solo-1"))
              << rowOf(2, -1, QStringLiteral("image"), QStringLiteral("solo-2"));
            seqOnlyRoot.buildFromRows(QStringLiteral("/synth"), r, false, QStringLiteral("soloRoot"));
        }
        eqCheck(seqOnlyRoot, QStringLiteral("只有根（1 节点 3 条目）"));

        // (c) 空文件夹：有节点、**一个条目都没有**（嵌套三层）
        Sequence seqEmpty;
        {
            QVector<Sequence::Row> r;
            int o = 0;
            const int e1 = o++, e2 = o++, e3 = o++, e4 = o++;
            r << rowOf(e1, -1, QStringLiteral("folder"), QStringLiteral("E1"))
              << rowOf(e2, e1, QStringLiteral("folder"), QStringLiteral("E1a"))
              << rowOf(e3, e2, QStringLiteral("folder"), QStringLiteral("E1a-i"))
              << rowOf(e4, -1, QStringLiteral("folder"), QStringLiteral("E2"));
            seqEmpty.buildFromRows(QStringLiteral("/synth"), r, false, QStringLiteral("empty"));
        }
        eqCheck(seqEmpty, QStringLiteral("空文件夹（5 节点 0 条目）"));

        // (d) 层级深浅不一：12 祖先链 + 深节点下 8 个空子目录 + 根级 5 条
        Sequence seqDeep;
        {
            QVector<Sequence::Row> r;
            int o = 0, parent = -1;
            for (int d = 0; d < 12; ++d) {
                const int me = o++;
                r << rowOf(me, parent, QStringLiteral("folder"), QStringLiteral("L%1").arg(d));
                r << rowOf(o++, me, QStringLiteral("image"), QStringLiteral("L%1-k").arg(d));
                parent = me;
            }
            for (int i = 0; i < 8; ++i)
                r << rowOf(o++, parent, QStringLiteral("folder"), QStringLiteral("leaf%1").arg(i));
            for (int i = 0; i < 5; ++i)
                r << rowOf(o++, -1, QStringLiteral("image"), QStringLiteral("top%1").arg(i));
            seqDeep.buildFromRows(QStringLiteral("/synth"), r, false, QStringLiteral("deep"));
        }
        eqCheck(seqDeep, QStringLiteral("深浅不一（21 节点 17 条目）"));

        // (e) 无树来源（`m_tree` 为空）：没有节点 ⇒ 只有**越界**这条边界约定
        Sequence seqNoTree;
        fill(seqNoTree, mk(5));
        eqCheck(seqNoTree, QStringLiteral("无树来源（buildFromEntries ⇒ m_tree 空）"));
        check(seqNoTree.tree().isEmpty() && seqNoTree.subtreeFirst(0) == 0
                  && seqNoTree.subtreeCount(0) == 0 && seqNoTree.containerOf(0) == -1
                  && !seqNoTree.isElement(0) && !seqNoTree.isElement(-1)
                  && seqNoTree.containerRange(0) == QPair<int, int>(0, seqNoTree.count()),
              QStringLiteral("⑰ 无树：subtree 0（越界约定）/ isElement 假 / containerOf −1 / 范围 [0, count()=%1)")
                  .arg(seqNoTree.count()));

        // (f) 节点下标越界：-1 / size / size+99 ⇒ 0（两个访问器 + isElement 假）；
        //     无后代的节点：记录 = 它**直接**产出的那些（没有图 ⇒ subtreeFirst = **-1**）
        {
            const int n = seqA.tree().size();
            check(seqA.subtreeFirst(-1) == 0 && seqA.subtreeFirst(n) == 0
                      && seqA.subtreeFirst(n + 99) == 0 && seqA.subtreeCount(-1) == 0
                      && seqA.subtreeCount(n) == 0 && seqA.subtreeCount(n + 99) == 0,
                  QStringLiteral("⑰ 节点越界：−1 / size(%1) / size+99 ⇒ 0（subtreeFirst 与 subtreeCount）")
                      .arg(n));
            bool leafRule = true;
            int  emptyNodes = 0;
            for (int i = 0; i < n; ++i) {
                bool hasChild = false;
                for (int j = 0; j < n; ++j)
                    if (seqA.tree().at(j).parent == i) { hasChild = true; break; }
                if (hasChild) continue;
                const Sequence::TreeNode &leaf = seqA.tree().at(i);
                const int expectFirst = (leaf.itemCount > 0) ? leaf.firstItem : -1;
                if (seqA.subtreeFirst(i) != expectFirst
                    || seqA.subtreeCount(i) != leaf.itemCount)
                    leafRule = false;
            }
            for (int i = 1; i < n; ++i)                        // 空目录：itemCount 0（无后代）
                if (seqA.tree().at(i).itemCount == 0 && seqA.subtreeCount(i) == 0
                    && seqA.subtreeFirst(i) == -1)
                    ++emptyNodes;
            check(leafRule, QStringLiteral("⑰ 无后代的节点：subtreeCount = 直接 itemCount / "
                                           "subtreeFirst = firstItem（没有图 ⇒ −1）"));
            check(emptyNodes >= 2,
                  QStringLiteral("⑰ 空目录：itemCount 0 ⇒ subtreeFirst **−1** / subtreeCount 0（%1 个）")
                      .arg(emptyNodes));
            // ★★ **空容器不污染父节点**（旧递归的毛病，L41）：
            //    · seqDeep 的 L1（节点 2）自己那张图排在**下标 1**；它下面还有 12 层深链 +
            //      8 个空子目录 —— 新语义必须是 1，不是被空叶子拉成 0；
            //    · seqA 的 A2（节点 3）：自己的图在下标 2，子目录 A2y（节点 5）**是空的**
            //      —— 旧递归会 `qMin(2, 0)` = 0，新语义必须是 2；
            //    · seqA 的根级空目录 B（节点 6）：自己就是 −1，且不是元素。
            check(seqDeep.subtreeFirst(2) == 1 && seqDeep.subtreeCount(2) == 11,
                  QStringLiteral("⑰ **空容器不把父节点的「第一条」拉成 0**：深链 L1 = %1（期望 1）"
                                 " / %2 张（期望 11 = 它自己 + 下面 10 层）")
                      .arg(seqDeep.subtreeFirst(2)).arg(seqDeep.subtreeCount(2)));
            check(seqA.subtreeFirst(3) == 2 && seqA.subtreeCount(3) == 4
                      && seqA.subtreeFirst(5) == -1,
                  QStringLiteral("⑰ A2（含空子目录 A2y）：first %1（期望 2）/ count %2（期望 4）")
                      .arg(seqA.subtreeFirst(3)).arg(seqA.subtreeCount(3)));
            check(seqA.subtreeFirst(6) == -1 && seqA.subtreeCount(6) == 0
                      && !seqA.isElement(6),
                  QStringLiteral("⑰ 根级空目录 B（节点 6）：first −1 / count 0 / 不是元素"));
            // 空文件夹（连一个条目都没有）：每个节点都"没有第一条"、都不是元素
            bool allEmpty = true;
            for (int i = 0; i < seqEmpty.tree().size(); ++i)
                if (seqEmpty.subtreeFirst(i) != -1 || seqEmpty.subtreeCount(i) != 0
                    || seqEmpty.isElement(i))
                    allEmpty = false;
            check(seqEmpty.tree().size() == 5 && allEmpty,
                  QStringLiteral("⑰ 全空文件夹：5 个节点**全部** −1 / 0 / 不是元素"));
            // ★ 空文件夹里**一个元素都没有**（连根都不是：没有图）
            int elems = 0;
            for (int i = 0; i < seqEmpty.tree().size(); ++i)
                if (seqEmpty.isElement(i)) ++elems;
            check(elems == 0, QStringLiteral("⑰ 全空文件夹：元素数 = 0（实得 %1）").arg(elems));
        }

        // (g) **扫描分批追加**：边扫边比（索引"脏了就地重算"的那条路）+ 终态
        QTemporaryDir fx;
        if (!fx.isValid()) {
            qInfo().noquote() << QStringLiteral("[viewseq] 跳过：建不出临时目录");
        } else {
            QDir().mkpath(fx.filePath(QStringLiteral("d1/d1a")));
            QDir().mkpath(fx.filePath(QStringLiteral("d2")));
            QDir().mkpath(fx.filePath(QStringLiteral("d3/d3a")));
            QFile::copy(QStringLiteral(".testdata/edge/p0.png"), fx.filePath(QStringLiteral("cover.png")));
            QFile::copy(QStringLiteral(".testdata/edge/p1.png"), fx.filePath(QStringLiteral("r1.png")));
            // 够多的文件 ⇒ 生产者**不可能**在消费者看到第一个批次之前就收尾（保证"未收尾"现场）
            const QStringList dirs{ QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"),
                                    QStringLiteral("d1/d1a"), QStringLiteral("d3/d3a") };
            int made = 0;
            for (const QString &d : dirs) {
                for (int i = 0; i < 24; ++i) {
                    const QString src = QStringLiteral(".testdata/edge/p%1.png").arg(i % 3);
                    if (QFile::copy(src, fx.filePath(QStringLiteral("%1/%2.png")
                                                         .arg(d).arg(i, 2, 10, QLatin1Char('0')))))
                        ++made;
                }
            }

            // ★ 夹具：自然序 = 走查顺序 ⇒ 节点 0=根（合成）1=d1 2=d1a 3=d2 4=d3 5=d3a
            //   6=emptydir（空目录，永远不是元素）；条目 0,1 = 根的两张，d1 = 2..25，
            //   d1a = 26..49，d2 = 50..73，d3 = 74..97，d3a = 98..121。
            QDir().mkpath(fx.filePath(QStringLiteral("emptydir")));

            Sequence seqScan;
            // ★ **确定性地**抓三个现场（都是"扫描还没结束"）：
            //   ① d1 已经发现图片、但**自己那一支还没扫完** ⇒ 记录活着但**不是元素**（L40）；
            //   ② d2 的文件夹行到了 ⇒ **d1 扫完** ⇒ 数字一出现就是最终值（L41）+ 内部查询能用；
            //   ③ 终态。
            //   生产者还要读完剩下上百个文件 ⇒ **只跑一次**扫描，不重试、不兜底；
            //   万一这两个时刻没抓到，后面的 check 直接 FAIL（用例设计问题，不靠重试掩盖）。
            int midBad = 0, midNode = 0, midIdx = 0;
            QString midFirstBad;
            auto midCompare = [&](const QString &what) {
                QString bad;
                int nv = 0, iv = 0;
                if (!subtreeIndexMatches(seqScan, nv, iv, bad)) {
                    ++midBad;
                    if (midFirstBad.isEmpty()) midFirstBad = what + QStringLiteral("：") + bad;
                }
                midNode += nv;
                midIdx += iv;
            };
            bool caughtOpen = false, caughtClosed = false;
            int openCount = -1;                    // ① d1 已知张数（有图但不可见）
            int closedFirst = -1, closedCount = -1; // ②/终态：d1 的记录（必须逐值不变）
            int midNext0 = -2, midPrev2 = -2, midCont2 = -2, midNext2 = -2;
            int closedRows = -1;
            QString midContLabel;
            bool openNotVisible = false, openBrowseClean = false;
            bool closedVisible = false;
            int midSnapshots = 0, midUnfinished = 0;
            // ★ **单次尝试**：一次 startScan + 一个自旋循环，抓不到那两个时刻就 FAIL。
            seqScan.startScan(fx.path(), 5, false);
            for (int spin = 0; spin < 5000000 && !seqScan.scanFinished(); ++spin) {
                seqScan.drain();
                if (seqScan.scanFinished()) break;

                if (!caughtOpen && seqScan.tree().size() >= 2
                    && seqScan.subtreeCount(1) > 0 && seqScan.subtreeFirst(1) >= 0) {
                    caughtOpen = true;
                    openCount = seqScan.subtreeCount(1);
                    // ★ **扫完前不可见**：d1 已经有图、但那一支还没扫完 ⇒ 不是元素，
                    //   于是**任何枚举都拿不到它**（浏览行 / childrenOf）。
                    WindowBuilder wb;
                    NavState nav;
                    std::shared_ptr<const Window> w = wb.build(seqScan, nav);
                    bool inChildren = w->childrenOf(0).contains(1);
                    // ★ 浏览行**从序列取**（T2d-1c-①：不再随窗口）——层 -1 = 根
                    const fetch::BrowseLayer layer = browseLayerOf(seqScan, -1, 0);
                    bool inRows = false;
                    for (const BrowseModel::Row &r : layer.rows)
                        if (r.nodeIndex == 1) inRows = true;
                    openNotVisible = !seqScan.isElement(1) && !inChildren && !inRows;
                    openBrowseClean = layer.rows.size() == 2
                                      && layer.rows.at(0).kind == QStringLiteral("image")
                                      && layer.rows.at(1).kind == QStringLiteral("image");
                    ++midSnapshots;
                    ++midUnfinished;
                    midCompare(QStringLiteral("未收尾快照"));
                }

                if (!caughtClosed && seqScan.tree().size() >= 4) {   // d2 的文件夹行到了
                    caughtClosed = true;
                    closedFirst = seqScan.subtreeFirst(1);
                    closedCount = seqScan.subtreeCount(1);
                    // ★ 扫完那一刻起：d1 是元素 ⇒ 出现在枚举里，且数字就是最终值
                    WindowBuilder wb;
                    NavState nav;
                    std::shared_ptr<const Window> w = wb.build(seqScan, nav);
                    const fetch::BrowseLayer layer = browseLayerOf(seqScan, -1, 0);
                    bool rowOk = false;
                    for (const BrowseModel::Row &r : layer.rows)
                        if (r.nodeIndex == 1 && r.kind == QStringLiteral("folder")
                            && r.subtreeFirst == closedFirst
                            && r.subtreeCount == closedCount)
                            rowOk = true;
                    closedVisible = seqScan.isElement(1) && w->childrenOf(0).contains(1) && rowOk;
                    closedRows = layer.rows.size();
                    // ★ **扫描期间**这三样就能用（今天恒为 -1 / 拿不到）：
                    //   `nextContainerFirst(0)` = 最近的下一个容器起点；`containerOf(2)` =
                    //   第 2 条图落在哪个文件夹；`prevContainerFirst(2)` = 它前面那个容器起点。
                    midNext0 = seqScan.nextContainerFirst(0);
                    midCont2 = seqScan.containerOf(2);
                    midNext2 = seqScan.nextContainerFirst(2);
                    midPrev2 = seqScan.prevContainerFirst(2);
                    midContLabel = (midCont2 >= 0 && midCont2 < seqScan.tree().size())
                                       ? seqScan.tree().at(midCont2).label
                                       : QString();
                    ++midSnapshots;
                    ++midUnfinished;
                    midCompare(QStringLiteral("扫完那一刻"));
                }
                if (caughtOpen && caughtClosed) break;
            }
            check(caughtOpen && midBad == 0,
                  midBad == 0
                      ? QStringLiteral("⑰ 扫描分批追加：**未收尾**（有树有条目、还没收尾）时逐值等价"
                                       "（%1 个文件；比 %2+%3 处）")
                            .arg(made).arg(midNode).arg(midIdx)
                      : QStringLiteral("⑰ 扫描分批追加：**不一致** —— %1").arg(midFirstBad));
            // ★★ **扫完前不可见**（L40 的行为凭据）：d1 已经有 %1 张图（记录活着），
            //    但它那一支还没扫完 ⇒ 不是元素 ⇒ 浏览行 / childrenOf 都拿不到它。
            check(caughtOpen && openCount > 0 && openNotVisible,
                  QStringLiteral("⑰ **扫完前不可见**：d1 已知 %1 张（记录活着）但 isElement 假，"
                                 "且不在 childrenOf / 浏览行里")
                      .arg(openCount));
            check(caughtOpen && openBrowseClean,
                  QStringLiteral("⑰ 扫完前根层浏览行 = 只有 2 张图（没有任何目录行）"));
            check(caughtClosed && closedVisible,
                  QStringLiteral("⑰ **扫完那一刻起可见**：d1 成为元素、进入 childrenOf（第 %1 张 / %2 张）")
                      .arg(closedFirst).arg(closedCount));
            check(caughtClosed && closedRows == 3,
                  QStringLiteral("⑰ 扫完那刻根层浏览行 = 2 图 + 1 目录（实得 %1 行）").arg(closedRows));
            // ★★ **扫描期间的行为凭据**（今天必然不成立 —— 那时子树值恒为 0/0）：
            //    已扫完目录的数字**一出现就是最终值**；`prev/nextContainerFirst` 与
            //    `containerOf(当前图下标)` 在扫描期间就能用（供 OSD / 上一个·下一个文件夹）。
            check(caughtClosed && closedFirst == 2 && closedCount == 48,
                  QStringLiteral("⑰ 扫完即定值：d1 = first %1（期望 2）/ count %2（期望 48）")
                      .arg(closedFirst).arg(closedCount));
            check(caughtClosed && midNext0 == 2 && midNext2 == 26 && midPrev2 == 0,
                  QStringLiteral("⑰ **扫描中** prev/nextContainerFirst 可用：next(0)=%1（期望 2）"
                                 " / next(2)=%2（期望 26）/ prev(2)=%3（期望 0）")
                      .arg(midNext0).arg(midNext2).arg(midPrev2));
            check(caughtClosed && midCont2 == 1 && midContLabel == QStringLiteral("d1"),
                  QStringLiteral("⑰ **扫描中** containerOf(2) 定位到所属目录：节点 %1 =「%2」"
                                 "（期望 1 = d1）")
                      .arg(midCont2).arg(midContLabel));

            // 接着边扫边比到终态（每个批次后都比一次），最后再比终态
            QElapsedTimer t;
            t.start();
            while (!seqScan.scanFinished() && t.elapsed() < 30000) {
                seqScan.drain();
                ++midSnapshots;
                midCompare(QStringLiteral("扫描中快照"));
                if (!seqScan.scanFinished()) ++midUnfinished;
                QThread::msleep(1);
            }
            seqScan.drain();
            ++midSnapshots;
            midCompare(QStringLiteral("终态快照"));
            nodeValues += midNode;
            indexValues += midIdx;
            check(seqScan.scanFinished() && midBad == 0 && midUnfinished >= 1,
                  QStringLiteral("⑰ 扫描分批追加：%1 个快照（%2 个未收尾）逐值等价（比 %3+%4 处）")
                      .arg(midSnapshots).arg(midUnfinished).arg(midNode).arg(midIdx));
            // ★ **一出现就是最终值**：扫完那一刻记下的两个数，扫完之后**逐值不变**
            check(caughtClosed && seqScan.subtreeFirst(1) == closedFirst
                      && seqScan.subtreeCount(1) == closedCount,
                  QStringLiteral("⑰ d1 的记录从扫完那刻到扫描结束**逐值不变**（%1/%2 ⇒ %3/%4）")
                      .arg(closedFirst).arg(closedCount)
                      .arg(seqScan.subtreeFirst(1)).arg(seqScan.subtreeCount(1)));
            // ★ **空目录永远不出现**：走到终态也一样（节点还在树里，但没有任何枚举到它）
            check(seqScan.tree().size() == 7 && seqScan.subtreeFirst(6) == -1
                      && !seqScan.isElement(6),
                  QStringLiteral("⑰ 空目录（节点 6）：在树里（%1 个节点）但记录 −1 / 不是元素")
                      .arg(seqScan.tree().size()));
            {
                WindowBuilder wb;
                NavState nav;
                std::shared_ptr<const Window> w = wb.build(seqScan, nav);
                const fetch::BrowseLayer layer = browseLayerOf(seqScan, -1, 0);
                bool hasEmpty = false;
                for (const BrowseModel::Row &r : layer.rows)
                    if (r.nodeIndex == 6) hasEmpty = true;
                // ① **元素枚举**里没有它（口径不变：浏览行 / childrenOf 只含元素）；
                check(!hasEmpty && !w->childrenOf(0).contains(6),
                      QStringLiteral("⑰ 空目录（节点 6）不在**元素**枚举里（浏览行 / childrenOf）"));
                // ② **祖先链**（设计 12 §8.7.1）**不过"元素"判定条件** ⇒ 根 → 它自己都在链里。
                //    ★ T2d-1c-①：祖先链现在由 `browseLayerOf` 给（浏览「面包屑」用的就是它；
                //      有界窗口不再带浏览块）⇒ 这里按**新形态**验同一条口径（不是放宽：
                //      ① 仍是严格的元素枚举判定条件）。
                check(browseLayerOf(seqScan, 6, 0).chain == QVector<int>({ 0, 6 }),
                      QStringLiteral("⑰ 空目录在**祖先链**里（browseLayerOf(空目录).chain = {根 0, 6}）"));
            }

            // (h) **同一对象再收尾一次**（`sortNaturally()` ⇒ 重排条目 + 重新沿祖先链累加）
            seqScan.sortNaturally();
            check(seqScan.count() == seqScan.discoveredCount(),
                  QStringLiteral("⑰ 再收尾：count = discoveredCount = %1").arg(seqScan.count()));
            eqCheck(seqScan, QStringLiteral("同一对象再收尾一次（sortNaturally）"));

            // (i) 同一对象**换一批形状重建**：节点数不变、条目分布不同 ⇒ 索引必须跟着变
            Sequence seqRebuild;
            seqRebuild.buildFromRows(QStringLiteral("/synth"), n1, false, QStringLiteral("root"));
            const int aCount1 = seqRebuild.subtreeCount(1);          // 节点 1 = 目录 A
            QVector<Sequence::Row> n2 = n1;
            for (Sequence::Row &r : n2)
                if (r.kind == QLatin1String("image")) r.parentOrd = 0;   // 条目全挂到 A
            seqRebuild.buildFromRows(QStringLiteral("/synth"), n2, false, QStringLiteral("root"));
            const int aCount2 = seqRebuild.subtreeCount(1);
            eqCheck(seqRebuild, QStringLiteral("重建：同节点数 / 不同条目分布"));
            check(aCount1 != aCount2,
                  QStringLiteral("⑰ 重建后形状确实变了（目录 A 的 subtreeCount %1 ⇒ %2）")
                      .arg(aCount1).arg(aCount2));

            // (j) 拷贝构造 / 赋值：记录是**并入顺序的产物**（不是可重算的派生量）⇒ 必须**原样带走**
            Sequence seqAssign;
            seqAssign = seqA;
            eqCheck(seqAssign, QStringLiteral("赋值（operator=）后"));
            Sequence seqCopy(seqA);
            eqCheck(seqCopy, QStringLiteral("拷贝构造后"));
        }

        check(nodeValues > 0 && indexValues > 0,
              QStringLiteral("⑰ 等价比对合计：节点值 %1 处 / 下标值 %2 处"
                             "（各形状 × 含越界值 × 扫描中快照）")
                  .arg(nodeValues).arg(indexValues));
    }

    return check.finish();
}

int main(int argc, char *argv[])
{
    return testsupport::suiteMain(argc, argv, &runViewSeqSelfTest);
}

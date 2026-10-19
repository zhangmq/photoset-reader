#include "uimodel.h"

#include "appsettings.h"
#include "browsemodel.h"          // 行模型（`::BrowseModel`：换层/打开/选中的行语义）
#include "browsemodelview.h"      // ★ T2d-1b：浏览界面的模型（QML 的 `ui.browse`）
#include "librarycore.h"          // ★ T2c-2b-1：图库/序列抽象 + 重置队列（设计 14 §9.3）
#include "librarymodel.h"
#include "librarystore.h"
#include "gpuscale.h"
#include "seqwindow.h"            // 浏览的一层（`fetch::BrowseLayer` / `browseLayerOf`）
#include "sequence.h"
#include "viewermodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMetaObject>
#include <QStyleHints>
#include <QTimer>
#include <QVariantMap>

namespace app {

// 重置参数打包（**唯一实现**，`LibraryCore::ResetOptions`）：设计 14 §9.3 —— core 不读设置，
//   ⇒ 点按"重置"那一刻的参数快照由本类从持有的 `m_settings` 打包传进去。
static LibraryCore::ResetOptions resetOptionsOf(const AppSettings &s)
{
    LibraryCore::ResetOptions o;
    o.maxDepth = s.recurseDepth();
    o.hidden = s.scanHidden();
    o.innerArchiveMaxMb = qint64(s.innerArchiveMaxMb());
    return o;
}

UiModel::UiModel(library::Store *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    // ★★ 运行期设置的**唯一实例**在这里 load（= 旧 `main` / `iconpicker` / 自检里那次
    //   单例的 `load()`；时机等价：本类被建出来之后才有人读设置）。
    m_settings.load();

    // ★★ 设计 14 §9.3：**图库/序列抽象 = `LibraryCore`**（本类持有）。
    //   `Store` 仍由本类建（或由调用方注入）并交给它 —— core **不碰数据库生命周期**。
    m_core = std::make_unique<LibraryCore>(m_store, this);

    // ★★ **退出收尾（G3，用户 2026-10-15 批准）**：必须在 `aboutToQuit` 里 join 落库 worker。
    //   为什么不能只靠 `~LibraryCore`：`main.cpp` 把本类挂在 `QApplication` 上 ⇒
    //   `~LibraryCore` 发生在 `~QApplication` **删除子对象**时，那时
    //   `QCoreApplication::instance()` 已空 ⇒ worker 最后那次事务 / 连接析构用不了 QtSql
    //   （实测 `QSqlDatabase requires a QCoreApplication` + `Driver not loaded`，**落库丢失**）。
    //   `aboutToQuit` 时 app 实例仍在 ⇒ `LibraryCore::shutdown()` 能把那次原子替换跑完。
    //   ★ 已知点：`aboutToQuit` 之后事件循环就要停了 ⇒ worker 排队的 `commitFinished` 可能不再被处理 ——
    //     没关系（事务已提交）；`m_commitInFlight` 保持 true，锁由 `~ResetQueue` 释放。
    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this] {
            if (m_core) m_core->shutdown();
        });
    }

    // ★★ T2d-1b：**浏览界面的模型**（QML 的 `ui.browse`）—— 它只认序列，不认 viewer。
    //   动作在这里接线：换层 ⇒ 落到这一层的首条；选中 ⇒ viewer 跳过去；打开 ⇒ 切查看屏。
    m_browse = new BrowseModel(this);
    connect(m_browse, &BrowseModel::layerChanged, this, &UiModel::onBrowseLayerChanged);
    connect(m_browse, &BrowseModel::rowSelected, this, &UiModel::onBrowseRowSelected);
    connect(m_browse, &BrowseModel::openRequested, this, &UiModel::onBrowseOpenRequested);

    // 图集库列表：**永远来自 SQLite**（轮询 `data_version` 同步）—— 照旧 VM 的父子关系；
    //   ★ T2c-2b-1：卡片的状态/计数 role 改从 **core 的内存态**取（不再读 `scan_state` 列）
    m_library = new LibraryModel(m_store, m_core.get(), this);
    // 库写失败（加入图集等）走同一条瞬时提示
    //   （★ T2c-2b-2：旧的 `ScanController` 失败/提示通道随之删除；重置的失败/提示
    //     一律由 core 经 `noticeReady` 推过来，见下）
    connect(m_library, &LibraryModel::libraryError, this, [this](const QString &text) {
        notify(text);
    });

    // ── ★★ core 的接线（T2c-2b-1 交付物②③）────────────────────────────────
    //   · **生产者唤醒**：core 的发布钩子 ⇒ GUI 线程 ⇒ 合并到每轮事件循环最多一次
    //     `pump()`（core 自己已经做了两层合并）⇒ 结果经 `pumped` 送出来；
    //     本类**只接 `pumped`**、按"哪个槽真的变了"做事（**变化驱动，没变不刷**）。
    //   · **兜底**：生产者完全不推进（FIFO 卡死）⇒ 没有唤醒；core 的 1 s
    //     `data_version` 那一跳里顺带 pump（core 内，不新增定时器）。
    connect(m_core.get(), &LibraryCore::pumped, this, &UiModel::onCorePumped);
    // core 的提示（重置完成 / 未落库 / 别的实例持锁 / 卡住…）＝ 同一条瞬时提示通道。
    //   本类就是通知的**唯一消费者** ⇒ 顺手把 core 的 FIFO 取空（不留悬挂队列）。
    connect(m_core.get(), &LibraryCore::noticeReady, this, [this](const QString &text) {
        const QString taken = m_core->takeNotice();
        notify(taken.isEmpty() ? text : taken);
    });

    // 库打不开也用同一条瞬时提示（启动期"库不可用"不该只有日志）。
    //   ⚠️ `store == nullptr` = **调试视图刻意不建库**（只跑会话来源）⇒ 那不是失败，不提示。
    if (m_store && !m_store->isOpen()) {
        const QString why = m_store->lastError().isEmpty()
                                ? QStringLiteral("图集库不可用（只读/被占用？）")
                                : m_store->lastError();
        QTimer::singleShot(0, this, [this, why] { notify(why); });
    }
    // ★★ **"卡住"的发现与处置在消费侧**（`LibraryCore::pump` 的卡住分支 + `reapSlot()`）——
    //   **主线程不参与**：原来这里那个 1 s 轮询 + `QThread::terminate()` 的看门狗**已删除**。
    //   （旧的"陈旧自愈"机制随 `scan_state` 列在 T2c-2b-2 一并删除。）
    // ★ 会话来源（直接打开文件夹/压缩包/单图）：`Sequence::unsupportedReason()` 由
    //   **扫描线程**在开头判定（7z/RAR 的探针很贵，绝不能放 GUI 线程）⇒ 这里起一个
    //   便宜的探针定时器，扫完（或已经给出原因）就停。
    //   ★ v7：同一个定时器也读 `Sequence::noticeText()`（扫描线程产出的瞬时提示 ——
    //   例如"内层压缩包超过上限，未展开"）⇒ **一律走 `notify`，不是失败**。
    m_sessionProbe = new QTimer(this);
    m_sessionProbe->setInterval(300);
    connect(m_sessionProbe, &QTimer::timeout, this, [this] {
        Sequence *seq = m_src.seq;
        if (!seq || m_collectionId >= 0) { m_sessionProbe->stop(); return; }
        const QString why = seq->unsupportedReason();
        if (!why.isEmpty()) {
            qWarning().noquote() << QStringLiteral("[ui] 来源不支持：%1").arg(why);
            notify(why);
            m_sessionProbe->stop();
            return;
        }
        const QString notice = seq->noticeText();
        if (!notice.isEmpty() && notice != m_sessionNotice) {
            m_sessionNotice = notice;
            qWarning().noquote() << QStringLiteral("[ui] 来源提示：%1").arg(notice);
            notify(notice);
        }
        if (seq->scanFinished()) m_sessionProbe->stop();
    });

    // ★★ 加入图集后**自动重置**（定案，T2c-2b-2 改为走 core）：加入与重置是同一件事，
    //   调用方只调一次 `addCollection`，不必记得再调 `requestReset`。
    //   ⚠️ **功能性必需**：不重置就没有行 —— 新加入的图集永远是空的。
    connect(m_library, &LibraryModel::collectionAdded, this, [this](qint64 id) {
        if (!m_core) return;
        QString err;
        if (!m_core->requestReset(id, resetOptionsOf(m_settings), &err)) {
            qWarning().noquote() << QStringLiteral("[ui] 新图集 %1 自动重置被拒：%2").arg(id).arg(err);
            notify(err.isEmpty() ? QStringLiteral("重置请求失败") : err);
            return;
        }
        qInfo().noquote() << QStringLiteral("[ui] 新图集 %1 已加入 → 自动重置").arg(id);
    });

    // 阅读位置写入磁盘：游标变化 400ms 去抖 + 5s 定期兜底（AGENTS.md「resume 写入磁盘的时机」；
    //   ⚠️ 这个 Timer **不是**幻灯片推进器）
    m_resumeDebounce = new QTimer(this);
    m_resumeDebounce->setSingleShot(true);
    m_resumeDebounce->setInterval(400);
    connect(m_resumeDebounce, &QTimer::timeout, this, &UiModel::flushResume);
    m_resumeFallback = new QTimer(this);
    m_resumeFallback->setInterval(5000);
    connect(m_resumeFallback, &QTimer::timeout, this, &UiModel::flushResume);

    // ★ `ui.viewer` 在**没打开来源时就是 null**（用户明确：null 没问题，空 viewer 没用）。
    //   QML 侧（`src/ui/fullscreen/**`、`src/ui/browse/**`）对 `ui.viewer.*` 一律先判非空 ——
    //   启动瞬间不再刷 TypeError；打开任何来源时才整体装上一组（`installViewer`）。
    qInfo().noquote() << QStringLiteral("[ui] 初始化  屏=%1  库=%2")
                             .arg(int(m_screen))
                             .arg(m_store && m_store->isOpen() ? QStringLiteral("可用")
                                                               : QStringLiteral("不可用"));
}

QObject *UiModel::viewer() const { return m_src.vm; }

UiModel::~UiModel()
{
    releaseNow(m_src);
}

// ── 三屏状态与导航（语义照旧 `ViewModel`）────────────────────────────────
//   ⚠️ 一处**有意的语义更新**（`index` 的含义，见报告）：
//     旧 VM 的浏览屏是**平铺条目列表**（ItemListModel），行号 == 条目下标；
//     新壳的浏览屏是**文件树层级**（冻结的 `src/ui/browse/**`，模型 = `ui.browse.items`），
//     QML 给过来的是**当前层的行号**，它与序列下标在有子目录/压缩包时**不再相等**
//     （拿它当下标用就是"打开第 N 行显示错图"那一类静默 bug）。
//     ⇒ T2d-1b：这些**按浏览行号解释**的动作（`activate / open / isContainerRow`）与
//       查行一起归 `app::BrowseModel`（QML 的 `ui.browse`）—— 行语义只有一处实现。
//   ★ **离开查看屏 ⇒ 先停播**（用户 2026-10-19）：幻灯片是会话级状态，模型可能还活着
//     （退到浏览屏时它还要用同一条会话序列）⇒ 不显式停，它会在后台一直翻。
//     真正的"销毁整个 ViewerModel"发生在离开**这个来源**时（落到库屏 / 换来源 ⇒ `teardownViewer`）。
void UiModel::stopPlaybackLeavingViewer()
{
    if (m_screen == Screen::Viewer && m_src.vm) m_src.vm->setPlaying(false);
}

void UiModel::goTo(Screen s, bool push)
{
    if (s == m_screen) return;
    stopPlaybackLeavingViewer();
    if (push) m_screenStack << m_screen;
    m_screen = s;
    // ★ 循环模式**不持久化**（设计 01 §3.6）：**每次进入沉浸视图回到默认（不循环）**。
    //   （换来源自然是新的 viewer ⇒ 也是默认。）
    if (s == Screen::Viewer && m_src.vm) m_src.vm->setLoopMode(0);
    // ★ T2c-2b-1：**离开浏览 / 查看 ⇒ 释放显示槽**（设计 14 §9.3）——
    //   图集查看 = 从磁盘读出的只读快照（重开＝读一次行）；会话来源 = 停 + 销毁。
    if (s == Screen::Library) releaseDisplaySlot();
    emit screenChanged();
}

void UiModel::goLibrary()
{
    // [返回图集库] / [home]：**清屏栈**直接回（back() 才是一级一级退）
    m_screenStack.clear();
    if (m_screen == Screen::Library) return;
    stopPlaybackLeavingViewer();
    m_screen = Screen::Library;
    releaseDisplaySlot();                       // ★ 离开浏览/查看 ⇒ 释放显示槽
    emit screenChanged();
}

void UiModel::back()
{
    stopPlaybackLeavingViewer();                // ★ 离开查看屏 ⇒ 先停播
    if (m_screenStack.isEmpty()) {
        if (m_screen == Screen::Library) return;
        m_screen = Screen::Library;
        releaseDisplaySlot();                   // ★ 离开浏览/查看 ⇒ 释放显示槽
        emit screenChanged();
        return;
    }
    const Screen prev = m_screenStack.takeLast();
    if (prev == m_screen) return;
    m_screen = prev;
    if (prev == Screen::Library) releaseDisplaySlot();   // ★ 落到库屏才释放
    emit screenChanged();
}

// ── ★★ T2d-1b：浏览界面的动作接线 ────────────────────────────────────────
//   ⚠️ `openRow / enterRow / isContainerRow`（**按浏览行号解释**）已经**搬到新家**
//      `app::BrowseModel`（QML 的 `ui.browse`）—— 行语义只有一处实现。
//      本类只做"认识 viewer 与抢占屏"的那部分接线（下面四个槽）。
//   ⚠️ 浏览行由 `ui.browse` 自己从序列取那一层；这里只剩"进入容器 ⇒ 位置落到那一支的第一条"。
void UiModel::onBrowseLayerChanged(int reason)
{
    if (!m_browse || !m_src.seq || !m_src.vm) return;
    // **进入容器**时，当前条目不在这一层里 ⇒ 把游标落到它的第一条图（否则这一层没有高亮行）。
    //   判定条件直接用序列的子树记录（= 浏览行同一份口径；节点下标由浏览模型给）。
    if (reason != 1) return;
    const int node = m_browse->node();
    if (node < 0) return;
    const int first = m_src.seq->subtreeFirst(node);
    const int count = m_src.seq->subtreeCount(node);
    if (count <= 0 || first < 0) return;
    const int cur = m_src.vm->cursor();
    if (cur < first || cur >= first + count) m_src.vm->jump(first);
}

// 选中一条图 = **移动那个唯一的游标**（查看页也跟着走）
void UiModel::onBrowseRowSelected(int itemIndex)
{
    if (m_src.vm && itemIndex >= 0) m_src.vm->jump(itemIndex);
}

// 打开那条图 = 选中 + 切「图像查看」屏
void UiModel::onBrowseOpenRequested(int itemIndex)
{
    if (itemIndex < 0) return;
    onBrowseRowSelected(itemIndex);
    goTo(Screen::Viewer, true);
}

// ── ★★ 序列 / 浏览（**只在 GUI 线程**）───────────────────────────────────
//   位置 / 预取 / 上屏都归 `fetch::ViewerModel`；浏览那一层归 `app::BrowseModel`，
//   它自己从序列取。本类在"序列长了 / 收尾了 / 重扫了"时**各通知一声**：

// 会话序列：**本类是唯一消费者** —— 发布钩子唤醒（合并成一次）⇒ 一次 drain 到空 ⇒ 通知
void UiModel::pumpSession()
{
    Sequence *seq = m_src.seq;
    if (!seq || m_collectionId >= 0) return;        // 图集来源由 core 的 pump 消费（GUI 线程）
    if (seq->drain()) onSequenceChanged();
}

// 序列变了（放了新批 / 收尾 / 重扫）⇒ 通知 viewer（序列长进了）与浏览模型（重算这一层）
void UiModel::onSequenceChanged()
{
    if (m_src.vm) m_src.vm->sequenceChanged();
    if (m_browse) m_browse->refresh();
}

// ── 图集库动作 ───────────────────────────────────────────────────────────
qint64 UiModel::collectionIdAt(int row) const
{
    if (!m_library) return -1;
    return m_library->data(m_library->index(row, 0), LibraryModel::IdRole).toLongLong();
}

void UiModel::removeCollection(int row) { if (m_library) m_library->removeCollection(row); }

// ★ 瞬时提示（**不占位浮层**）：不写入磁盘、不进设置；`seq` 自增让同一条文案也能重新动画
void UiModel::notify(const QString &text)
{
    if (text.isEmpty()) return;
    m_notification = text;
    ++m_notificationSeq;
    qInfo().noquote() << QStringLiteral("[ui] 瞬时提示：%1").arg(text);
    emit notificationChanged();
}

// 文件对话框（**QFileDialog 只在这里**）；直接打开 = **会话来源**，不建库记录
void UiModel::chooseAddCollection()
{
    const QString dir = QFileDialog::getExistingDirectory(
        nullptr, QStringLiteral("添加图集"), QDir::homePath());
    if (dir.isEmpty()) return;
    // 加入即**自动重置**（构造函数里已接：collectionAdded → core->requestReset）
    if (m_library) m_library->addCollection(dir, Sequence::Kind::Folder);
}

void UiModel::chooseOpenFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        nullptr, QStringLiteral("打开文件夹"), QDir::homePath());
    if (dir.isEmpty()) return;
    openPath(dir);
}

void UiModel::chooseOpenArchive()
{
    const QString filter = QStringLiteral(
        "压缩包 (*.zip *.cbz *.rar *.cbr *.7z *.cb7 *.tar *.cbt *.gz *.bz2 *.xz *.zst);;所有文件 (*)");
    const QString path = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("打开压缩包"), QDir::homePath(), filter);
    if (path.isEmpty()) return;
    openPath(path);
}

void UiModel::chooseOpenImage()
{
    // 可解码格式的**唯一事实源**是 QImageReader（AGENTS.md「依赖策略」）
    QStringList pats;
    for (const QByteArray &f : QImageReader::supportedImageFormats())
        pats << QStringLiteral("*.") + QString::fromLatin1(f);
    const QString filter = QStringLiteral("图片 (%1);;所有文件 (*)").arg(pats.join(QLatin1Char(' ')));
    const QString path = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("打开图片"), QDir::homePath(), filter);
    if (path.isEmpty()) return;
    openPath(path);
}

// ── 打开图集（**T2c-2b-1 新路径**：设计 14 §9.1 / §9.3）─────────────────────
//   `core->openCollection(id)` = 把**已写入磁盘的行**在内存重建一条**静态只读**序列
//   （正在重置 ⇒ core 会别名到那条活序列，本批见下面的守卫）；viewer/browse 读的
//   就是 `core->viewSequence()`（**同一个对象，不做视图镜像**）。
//   位置 = `Sequence::indexOfChain(decodeChain(resumeKey), resumePage)`（★ v5：链 + 页号）。
bool UiModel::openCollection(qint64 collectionId)
{
    if (!m_core) {
        qWarning().noquote() << QStringLiteral("[ui] 没有 core，打不开集合 %1").arg(collectionId);
        return false;
    }
    const library::Collection c = collectionById(collectionId);
    if (c.id < 0) {
        qWarning().noquote() << QStringLiteral("[ui] 集合不存在: %1").arg(collectionId);
        return false;
    }
    // ★ v5：resume = **label 链（JSON 数组）+ 页号**（不是下标，也不是拼串 key）
    const QStringList chain = Sequence::decodeChain(c.resumeKey);

    // ★★ 建组走 `buildCollectionView`：它内部**先把旧组同步拆掉**（core 会丢掉旧序列），
    //    再把快照收尾（pump 一次）⇒ 之后 core 的 pump 对它是只读的。
    if (!buildCollectionView(collectionId, chain, c.resumePage)) return false;

    qInfo().noquote() << QStringLiteral("[resume] 集合 %1 读回位置：链 %2 页 %3（条目 %4 条）")
                             .arg(collectionId)
                             .arg(QString::fromUtf8(Sequence::encodeChain(chain).toUtf8()))
                             .arg(c.resumePage)
                             .arg(m_src.seq ? m_src.seq->count() : -1);
    goTo(Screen::Browse, true);                 // 照旧 VM：打开集合 ⇒ 文件浏览屏
    return true;
}

// ★★ 建"图集查看"这一组（core 快照 → 收尾 → 落到 chain/page）：
//   · `core->openCollection` 给的是**静态只读快照**；
//   · 立刻 `pump()` 一次把快照**收尾**（`restore` 后 `m_finished` 还是 false ⇒
//     `drain()` 会跑一次 `finishAppend()`）。此后 core 的每次 `pump()` 对它是**只读**的
//     （空队 + `m_finished` ⇒ 不写任何消费者状态）⇒ 只读。
bool UiModel::buildCollectionView(qint64 collectionId, const QStringList &chain, int page)
{
    if (!m_core) return false;
    // ★★ **先把旧组同步拆掉**（**所有**建图集视图的路径都经这里 —— 打开 / 重建 / 跟随刷新）：
    //    `core->openCollection` 会丢掉旧序列，而旧组的 viewer 可能还在解码最后几张
    //    ⇒ 必须在 core 动它之前先拆（解码线程只拿一份条目信息的副本，拆是立刻返回的）。
    if (m_src.vm || m_src.seq) {
        flushResume();                 // 离开前把去抖中的阅读位置落一次
        teardownViewer();
        m_collectionId = -1;
    }
    // ⚠️ **本批的守卫**（见报告）：正在重置的图集 ⇒ core 会把查看槽**别名**到那条活序列
    //    （还在长）⇒ 本批不许把 viewer 挂上去。
    const LibraryCore::ResetState rs = m_core->resetState(collectionId);
    if (rs == LibraryCore::ResetState::Queued || rs == LibraryCore::ResetState::Running) {
        notify(QStringLiteral("该图集正在重置，完成后再打开"));
        qWarning().noquote() << QStringLiteral("[ui] 集合 %1 正在重置（%2）⇒ 本批不挂 viewer")
                                    .arg(collectionId).arg(resetq::resetStateName(rs));
        return false;
    }
    QString err;
    if (!m_core->openCollection(collectionId, &err)) {
        notify(err.isEmpty() ? QStringLiteral("打开图集失败") : err);
        return false;
    }
    Sequence *seq = m_core->viewSequence();
    if (!seq) {
        notify(QStringLiteral("打开图集失败：没有视图序列"));
        return false;
    }
    m_core->pump();                             // ★ 快照收尾（此后只读，见上面的说明）

    int idx = chain.isEmpty() ? -1 : seq->indexOfChain(chain, page);
    if (idx < 0) idx = 0;

    m_collectionId = collectionId;
    installCoreViewer(seq);
    if (!m_src.vm) return false;
    m_src.vm->jump(qMax(0, idx));
    // 刚读回来的位置**不算要写入磁盘**（链 + 页号都按当前条目记，flushResume 只写"动了之后"的位置）
    m_lastResumeKey = Sequence::encodeChain(seq->chainAt(qMax(0, idx)));
    m_lastResumePage = seq->pageAt(qMax(0, idx));
    if (m_resumeFallback && !m_resumeFallback->isActive()) m_resumeFallback->start();
    qInfo().noquote() << QStringLiteral("[ui] 集合 %1 → core 快照 %2 条，落到下标 %3")
                             .arg(collectionId).arg(seq->count())
                             .arg(qMax(0, idx));
    return true;
}

// 读一行图集（resume 用）—— **只读**；core 不提供单行查询 ⇒ 走它的集合列表。
library::Collection UiModel::collectionById(qint64 id) const
{
    if (m_core) {
        for (const library::Collection &c : m_core->collections())
            if (c.id == id) return c;
    }
    if (m_store && m_store->isOpen()) return m_store->collection(id);
    return library::Collection();
}

// ── 阅读位置写入磁盘（**只在图集来源下**；走 core 的转发）────────────────────
void UiModel::flushResume()
{
    if (m_collectionId < 0 || !m_store || !m_src.vm || !m_src.seq) return;
    const int cursor = m_src.vm->cursor();
    // ★ v5：已写入磁盘的是 **label 链的结构化编码（JSON 数组）** + 页号 —— 不拼串。
    //   T2c-2b-1：链从**当前序列**派生（core 快照与会话来源同一套 API，不再经 ItemSource）。
    const QStringList chain = m_src.seq->chainAt(cursor);
    if (chain.isEmpty()) return;
    const int page = m_src.seq->pageAt(cursor);
    const QString key = Sequence::encodeChain(chain);
    if (key == m_lastResumeKey && page == m_lastResumePage) return;   // 没变就不写库
    const bool ok = m_core ? m_core->setResume(m_collectionId, key, page)
                           : m_store->setResume(m_collectionId, key, page);
    if (!ok) return;
    m_lastResumeKey = key;
    m_lastResumePage = page;
    qInfo().noquote() << QStringLiteral("[resume] 集合 %1 写入磁盘位置：链 %2 页 %3（下标 %4）")
                             .arg(m_collectionId).arg(key).arg(page).arg(cursor);
}

// ── 一组来源的创建 / 释放 ────────────────────────────────────────────────
void UiModel::releaseNow(Source &s)
{
    // 顺序：**先消费者**。`~ViewerModel` **不 join 工作线程**（只换个记号就走，见它自己的说明）
    //   ⇒ 这里立刻返回，之后才轮到序列。
    delete s.vm;    s.vm = nullptr;
    // ★ T2c-2b-1：**core 给的序列不许删**（生命周期 = core 的槽位，设计 14 §9.3）
    if (s.ownsSeq) delete s.seq;
    s.seq = nullptr;
}

// ★★ 同步拆掉当前 viewer 组（T2c-2b-1）：
//   · **core 接下来要销毁序列之前必须调**（换快照 / 释放槽位 / closeSession）；
//   · 顺序：`m_src` 先置空 → `emit viewerChanged()`（QML 先换绑到 null）→ `releaseNow`
//     （`~ViewerModel` 只换个记号就走，**不 join 工作线程** ⇒ 立刻返回）；
//   · 与 `releaseLater` 的区别：这里**不等事件循环**。
void UiModel::teardownViewer()
{
    if (m_src.vm) m_src.vm->setPlaying(false);   // ★ 先停止，再销毁整个 viewer
    if (!m_src.vm && !m_src.seq) return;
    Source old = m_src;
    m_src = Source{};
    if (m_browse) m_browse->clear();      // 浏览界面跟着换来源（不留上一条来源的层 / 序列）
    emit viewerChanged();
    releaseNow(old);
}

// 用 **core 给的序列**（非拥有）建一组 viewer（参数只有预取长度）
fetch::ViewerModel *UiModel::installCoreViewer(Sequence *seq)
{
    Source s;
    s.seq = seq;
    s.ownsSeq = false;                          // ★ 序列归 core，本组只读它
    s.vm = new fetch::ViewerModel(m_settings.prefetchWindow(), nullptr);
    installViewer(s);
    return m_src.vm;
}

void UiModel::releaseLater(const Source &s)
{
    if (!s.vm) {
        Source tmp = s;
        releaseNow(tmp);
        return;
    }
    // 序列随旧 viewer 的析构释放：`destroyed` 在 `~ViewerModel`（含 `~QObject`）之后发出。
    //   **不捕获 this**：即使 UiModel 先没了也要释放。
    Sequence *seq = s.seq;
    const bool ownsSeq = s.ownsSeq;            // ★ core 给的序列不许删（T2c-2b-1）
    QObject::connect(s.vm, &QObject::destroyed, [seq, ownsSeq] {
        if (ownsSeq) delete seq;
    });
    s.vm->deleteLater();      // ★ 不许同步 delete（QML 绑定刚换绑，事件循环还没转到现在）
}

void UiModel::openPath(const QString &path)
{
    if (path.isEmpty()) return;
    // ★★ T2c-2b-1：会话来源与"图集查看"是**同一个显示槽**（设计 14 §9.3 ⇒ 互斥）：
    //   先把旧组同步拆掉（core 的查看槽也要释放 —— 它可能正是旧 viewer 在读的快照）。
    //   ★ 会话序列归本组（`ownsSeq`）；viewer 的解码线程只拿一份条目信息的副本 ⇒ 不碰它。
    flushResume();
    teardownViewer();
    if (m_core) m_core->closeCollectionView();
    m_path = path;
    m_collectionId = -1;              // 直接打开：**不建集合记录**（设计 11 §四）
    m_lastResumeKey.clear();
    m_lastResumePage = 0;
    m_sessionNotice.clear();          // ★ v7：换来源 ⇒ 瞬时提示重新计
    m_resumeDebounce->stop();
    m_resumeFallback->stop();
    buildViewer();
    if (m_sessionProbe) m_sessionProbe->start();     // ★ 查一次"不支持原因"（扫完即停）
    goTo(Screen::Browse, true);       // 照旧 VM：直接打开 ⇒ 文件浏览屏
}

// 换 viewer（**接线只在这里**）：装新组 → emit viewerChanged → 旧组延迟释放
void UiModel::installViewer(Source s)
{
    // 换画面 ⇒ 复位手势态（`zoomLocked` 时保留）。复位动作归 ui ⇒ QML 里不再有那段逻辑
    connect(s.vm, &fetch::ViewerModel::imageAChanged, this, &UiModel::resetView);
    connect(s.vm, &fetch::ViewerModel::imageBChanged, this, &UiModel::resetView);
    // ★ 幻灯片：间隔由上层（设置）下发；到头等瞬时提示由上层转发到 `notify`
    s.vm->setIntervalMs(m_settings.intervalMs());
    connect(s.vm, &fetch::ViewerModel::notice, this, &UiModel::notify);
    // 阅读位置：游标变化 400ms 去抖（5s 兜底在 m_resumeFallback）；
    //   ★ 同一处把"当前条目"告诉浏览模型（高亮行 = **派生**自它，浏览不读 viewer）。
    connect(s.vm, &fetch::ViewerModel::currentChanged, this, [this] {
        if (m_collectionId >= 0 && m_resumeDebounce) m_resumeDebounce->start();
        if (m_browse && m_src.vm) m_browse->setCursor(m_src.vm->cursor());
    });
    // ★ 本类与 viewer 之间只剩"**给序列 + 序列变了通知**"：位置 / 预取 / 上屏 / loading
    //   全归 viewer 自己。
    const Source old = m_src;
    m_src = s;
    //   ★ 版面是**会话级**：viewer 随来源建了又毁 ⇒ 换组这一刻把上一组的版面**交接**过来
    //     （本类不存第二份；读的就是马上要释放的那一组）。
    if (m_src.vm && old.vm) m_src.vm->setDoublePage(old.vm->doublePage());
    emit viewerChanged();            // ★ 先让 QML 换绑到新 viewer
    if (m_src.vm) m_src.vm->setSequence(m_src.seq);      // ★ 立刻定 loading + 派活
    if (m_browse) {
        // 浏览只吃序列 + 当前条目（不经过 viewer）
        m_browse->setSequence(m_src.seq);
        m_browse->setCursor(m_src.vm ? m_src.vm->cursor() : 0);
    }
    if (old.vm) releaseLater(old);   // ★ 再让旧组延迟释放
}

void UiModel::buildViewer()
{
    if (m_path.isEmpty()) return;

    Source s;
    s.seq = new Sequence;
    // ★★ **序列只有一个消费者 = 本类（GUI 线程）**：唤醒 = 序列自带的**发布钩子**
    //   （生产者放完一批 / 写终态时调）⇒ **合并成一次** queued 投递 ⇒ 一次 drain 到空。
    //   ⚠️ 必须**在 `startScan()` 之前**设好（此后生产者只读 ⇒ 无锁读安全）。
    s.seq->setPublishHook([this] {
        if (m_wakeQueued.exchange(true)) return;        // 合并：只投递一次
        QMetaObject::invokeMethod(this, [this] {
            m_wakeQueued.store(false);
            pumpSession();
        }, Qt::QueuedConnection);
    });
    // ★ **建序列只有一条路**：`Sequence::startScan`（目录 / 压缩包 / 单图 / 多页 同一套；
    //   条目里的**磁盘路径 + 容器成员链 + 页号**由它派生 —— 那是解码
    //   （`Sequence::image` → `EntryReader`）唯一认识的形态，也是"嵌套压缩包能读"的原因）。
    s.seq->startScan(m_path, m_settings.recurseDepth(), m_settings.scanHidden(),
                     Sequence::ScanOptions{ qint64(m_settings.innerArchiveMaxMb()) });
    // 本类只给 viewer **预取长度**（它不读设置）
    s.vm = new fetch::ViewerModel(m_settings.prefetchWindow(), nullptr);
    installViewer(s);
    // 直接打开 ⇒ 看第 0 条（"进入查看 = jump"，设计 13 §十一）：阅读屏一进去就有画面
    //   （扫描还在跑时由 viewer 的 `loading` 表达；用户随后点哪一条会再 jump 一次）
    s.vm->jump(0);
}

// ⚠️ T2c-2b-1：旧的"图集来源：`LibraryItemSource` → 投影 → `Sequence::buildFromRows`"
//   （`buildLibraryViewer`）**已被 core 的快照路径取代**（`buildCollectionView`）——
//   序列不再由本类持有，位置派生也不再经 `ItemSource` 投影一份。**没有留旧实现**：
//   两条并存的建序列路径会分叉（AGENTS.md 单一事实源）。

void UiModel::rebuildViewer()
{
    if (!m_src.vm) return;              // 还没建过来源 ⇒ 没什么可重建
    // "重建 = 换实现，不是换来源" ⇒ 会话态带过去（否则设置一改画面就跳回第 1 条）
    const int cursor = m_src.vm->cursor();
    if (m_collectionId >= 0 && m_core) {
        // 图集查看：**重开 core 的快照**（扫描跟随 / 设置变化都走这条），
        //   按 **label 链 + 页号**找回位置（不存下标）
        const QStringList chain = m_src.seq ? m_src.seq->chainAt(cursor) : QStringList();
        const int page = m_src.seq ? m_src.seq->pageAt(cursor) : 0;
        if (buildCollectionView(m_collectionId, chain, page)) return;
        // 打不开（图集没了 / 正在重置）⇒ 回库屏，别留半死状态
        goLibrary();
        return;
    }
    buildViewer();
    if (!m_src.vm) return;
    m_src.vm->jump(qMax(0, cursor));
}

// ── ★★ T2c-2b-1：core 的唤醒 / 变化驱动（设计 14 §9.2 / §9.4）──────────────
//   core 的 `pumped(session, view, reset)` 只在**对应槽真的 drain 出了变化**时置位
//   （`Sequence::drain()` 的返回值）⇒ 这里按位做事，**没变什么都不做**（不重建、不发信号）。
//   · 会话槽：本批没有消费者（会话序列归本类，core 的会话槽只服务它自己的 openSession）；
//   · 查看槽：正在看 core 的序列时，core 说它变了（**活序列在长**）⇒ 立刻通知 viewer
//     （静态快照收尾之后 drain 恒为"无变化" ⇒ 不会白通知）；
//   · 重置槽：卡片的**状态/计数**由 `LibraryModel` 直接接 core 的
//     `resetStateChanged` / `resetProgress`（同样是变化驱动）⇒ 这里不用再刷。
void UiModel::onCorePumped(bool sessionChanged, bool viewChanged, bool resetChanged)
{
    if (sessionChanged) {
        qInfo().noquote() << QStringLiteral("[ui] core 会话槽有变化（无消费者：会话序列归本类）");
    }
    if (viewChanged && m_collectionId >= 0 && m_src.vm) {
        onSequenceChanged();          // 序列长了 ⇒ 通知 viewer + 浏览重算
    }
    Q_UNUSED(resetChanged);          // 卡片状态/计数在 LibraryModel 里接（不在这里刷）
}

// ── ★★ T2c-2b-1：重置 / 取消（走 core，设计 14 §9.1 / §9.3）────────────────
//   参数在**这里**从持有的 `m_settings` 打包（= 点按那一刻的快照，core 不读设置）；
//   失败原因走既有 notice 通道（core 也会 push 提示）。
bool UiModel::requestReset(qint64 collectionId)
{
    if (!m_core) return false;
    // core 起重置时会把"查看槽"从快照换成**活序列**（同一个图集 ⇒ 别名）⇒
    //   正看着这个图集就先把 viewer 组**同步拆掉**（旧组不能还在读 core 要动的序列）。
    const bool wasViewing = (m_collectionId == collectionId);
    if (wasViewing) {
        flushResume();
        teardownViewer();
        m_core->closeCollectionView();
        m_collectionId = -1;
    }
    QString err;
    if (!m_core->requestReset(collectionId, resetOptionsOf(m_settings), &err)) {
        notify(err.isEmpty() ? QStringLiteral("重置请求失败") : err);
        if (wasViewing) goLibrary();            // 别把用户留在没有 viewer 的浏览屏
        return false;
    }
    qInfo().noquote() << QStringLiteral("[ui] 集合 %1 重置已入队（core）").arg(collectionId);
    return true;
}

bool UiModel::cancelReset(qint64 collectionId)
{
    // 排队中 ⇒ 出队 + Stopped；running ⇒ 置停止位（生产者自己退，下一次 pump 收尾，**不落库**）
    return m_core ? m_core->cancelReset(collectionId) : false;
}

// 回到库屏 ⇒ 释放显示槽（设计 14 §9.3）：图集查看 = 从磁盘读出的只读快照；会话来源 = 停 + 销毁。
//   ⚠️ 顺序：先把阅读位置落一次（去抖可能还挂着）→ 同步拆 viewer
//      → 才让 core 释放它的槽位（core 会销毁序列）。
void UiModel::releaseDisplaySlot()
{
    if (m_collectionId >= 0) flushResume();
    teardownViewer();
    if (m_core) m_core->closeCollectionView();
    m_collectionId = -1;
    m_path.clear();
    if (m_resumeDebounce) m_resumeDebounce->stop();
    if (m_resumeFallback) m_resumeFallback->stop();
    if (m_sessionProbe) m_sessionProbe->stop();
}

// ── 幻灯片间隔（值全在 `m_settings`；本类只发通知）─────────────────────────
//   设置页读写 `ui.slideshowIntervalMs`。
int UiModel::slideshowIntervalMs() const { return m_settings.intervalMs(); }

void UiModel::setSlideshowIntervalMs(int ms)
{
    const int v = qMax(50, ms);                 // 下限 50ms（与 AppSettings 同一条）
    if (m_settings.intervalMs() != v) m_settings.setIntervalMs(v);
    //   ★ 立刻下发（幻灯片用新参数继续：取消旧闹钟、按当时请求时刻重算）
    if (m_src.vm) m_src.vm->setIntervalMs(v);
    emit slideshowIntervalMsChanged();
}

// ── 适配 / 手势态 ───────────────────────────────────────────────────────
int UiModel::fitMode() const { return m_settings.defaultFitMode(); }

void UiModel::setFitMode(int m)
{
    m = qBound(0, m, 3);
    if (m_settings.defaultFitMode() == m) return;
    m_settings.setDefaultFitMode(m);
    emit fitModeChanged();
}

void UiModel::setZoomLocked(bool on)
{
    if (m_zoomLocked == on) return;
    m_zoomLocked = on;
    emit zoomLockedChanged();
}

void UiModel::setZoom(qreal v)
{
    if (qFuzzyCompare(m_zoom, v)) return;
    m_zoom = v;
    emit zoomChanged();
}

void UiModel::setPanX(qreal v)
{
    if (qFuzzyCompare(m_panX, v)) return;
    m_panX = v;
    emit panXChanged();
}

void UiModel::setPanY(qreal v)
{
    if (qFuzzyCompare(m_panY, v)) return;
    m_panY = v;
    emit panYChanged();
}

// 换画面复位：**只有没锁**才复位（锁定 ⇒ 缩放与 offset 跨画面保留）
void UiModel::resetView()
{
    if (m_zoomLocked) return;
    setZoom(1.0);
    setPanX(0.0);
    setPanY(0.0);
}

// ── UI 形态（会话级）────────────────────────────────────────────────────
void UiModel::setOverlaysVisible(bool on)
{
    if (m_overlaysVisible == on) return;
    m_overlaysVisible = on;
    emit overlaysVisibleChanged();
}

void UiModel::setOsdVisible(bool on)
{
    if (m_osdVisible == on) return;
    m_osdVisible = on;
    emit osdVisibleChanged();
}

void UiModel::toggleOsd() { setOsdVisible(!m_osdVisible); }

void UiModel::setWindowFullscreen(bool on)
{
    if (m_windowFullscreen == on) return;
    m_windowFullscreen = on;
    emit windowFullscreenChanged();
}

void UiModel::setBrowseGrid(bool on)
{
    if (m_browseGrid == on) return;
    m_browseGrid = on;
    emit browseGridChanged();
}

void UiModel::setInputFromKeyboard(bool on)
{
    if (m_inputFromKeyboard == on) return;
    m_inputFromKeyboard = on;
    emit inputFromKeyboardChanged();
}

// ── 设置（**值都在本类持有的 `m_settings` 里**；本类只是接口 + 通知）────
int UiModel::themeMode() const { return m_settings.themeMode(); }

void UiModel::setThemeMode(int m)
{
    m = qBound(0, m, 2);
    if (m_settings.themeMode() == m) return;
    m_settings.setThemeMode(m);
    emit themeModeChanged();                 // 跟随系统 / 浅 / 深 —— Theme.qml 自己看
}

bool UiModel::systemDark() const
{
    // Qt 6.5+：跟随系统配色
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void UiModel::cycleThemeMode()
{
    setThemeMode((themeMode() + 1) % 3);     // 跟随系统 → 浅色 → 深色 → 跟随系统
}

bool UiModel::scanHidden() const { return m_settings.scanHidden(); }

void UiModel::setScanHidden(bool on)
{
    if (m_settings.scanHidden() == on) return;
    m_settings.setScanHidden(on);
    emit scanHiddenChanged();
    rebuildViewer();                         // 等价于重扫（语义不变）
}

int UiModel::recurseDepth() const { return m_settings.recurseDepth(); }

void UiModel::setRecurseDepth(int v)
{
    v = qBound(1, v, 64);
    if (m_settings.recurseDepth() == v) return;
    m_settings.setRecurseDepth(v);
    emit recurseDepthChanged();
    rebuildViewer();                         // 等价于重扫（语义不变）
}

int UiModel::prefetchWindow() const { return m_settings.prefetchWindow(); }

void UiModel::setPrefetchWindow(int v)
{
    v = qBound(3, v, 64);
    if (m_settings.prefetchWindow() == v) return;
    m_settings.setPrefetchWindow(v);
    emit prefetchWindowChanged();
    rebuildViewer();                         // 预取长度是 viewer 的**构造参数**
}

int UiModel::cacheMb() const { return m_settings.cacheMb(); }

void UiModel::setCacheMb(int v)
{
    v = qMax(64, v);
    if (m_settings.cacheMb() == v) return;
    m_settings.setCacheMb(v);
    emit cacheMbChanged();
}

int UiModel::innerArchiveMaxMb() const { return m_settings.innerArchiveMaxMb(); }

void UiModel::setInnerArchiveMaxMb(int v)
{
    v = qBound(64, v, 8192);
    if (m_settings.innerArchiveMaxMb() == v) return;
    m_settings.setInnerArchiveMaxMb(v);
    emit innerArchiveMaxMbChanged();
    // ★ v7：**不重建 viewer**（避免无谓的重新扫描）；上限在"下一次展开"生效。
}

// ── 图像（缩放）参数 ────────────────────────────────────────────────────
//   值域裁剪照旧 VM（`viewmodel.cpp`）的语义；写入磁盘走 `m_settings`（唯一事实源）。
QString UiModel::scalerQuality() const { return m_settings.scalerQuality(); }

void UiModel::setScalerQuality(const QString &v)
{
    const QString q = (v == QLatin1String("fast") || v == QLatin1String("best"))
                          ? v : QStringLiteral("balanced");
    if (m_settings.scalerQuality() == q) return;
    m_settings.setScalerQuality(q);
    emit scalerQualityChanged();
    emit scalerParamsChanged();
}

QString UiModel::scalerMode() const { return m_settings.scalerMode(); }

void UiModel::setScalerMode(const QString &v)
{
    const QString m = (v == QLatin1String("nearest") || v == QLatin1String("qt-smooth"))
                          ? v : QStringLiteral("linear-light");
    if (m_settings.scalerMode() == m) return;
    m_settings.setScalerMode(m);
    emit scalerModeChanged();
    emit scalerParamsChanged();
}

QString UiModel::scalerFilter() const { return m_settings.scalerFilter(); }

void UiModel::setScalerFilter(const QString &v)
{
    const QString f = (v == QLatin1String("box") || v == QLatin1String("lanczos")
                       || v == QLatin1String("mitchell")) ? v : QStringLiteral("auto");
    if (m_settings.scalerFilter() == f) return;
    m_settings.setScalerFilter(f);
    emit scalerFilterChanged();
    emit scalerParamsChanged();
}

int UiModel::scalerLobes() const { return m_settings.scalerLobes(); }

void UiModel::setScalerLobes(int v)
{
    v = qBound(2, v, 4);
    if (m_settings.scalerLobes() == v) return;
    m_settings.setScalerLobes(v);
    emit scalerLobesChanged();
    emit scalerParamsChanged();
}

double UiModel::scalerMitchellB() const { return m_settings.scalerMitchellB(); }

void UiModel::setScalerMitchellB(double v)
{
    if (qFuzzyCompare(m_settings.scalerMitchellB(), v)) return;
    m_settings.setScalerMitchellB(v);
    emit scalerMitchellBChanged();
    emit scalerParamsChanged();
}

double UiModel::scalerMitchellC() const { return m_settings.scalerMitchellC(); }

void UiModel::setScalerMitchellC(double v)
{
    if (qFuzzyCompare(m_settings.scalerMitchellC(), v)) return;
    m_settings.setScalerMitchellC(v);
    emit scalerMitchellCChanged();
    emit scalerParamsChanged();
}

double UiModel::scalerAntiring() const { return m_settings.scalerAntiring(); }

void UiModel::setScalerAntiring(double v)
{
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(m_settings.scalerAntiring(), v)) return;
    m_settings.setScalerAntiring(v);
    emit scalerAntiringChanged();
    emit scalerParamsChanged();
}

// 当前参数匹配哪个预设（空 = 自定义）—— 与旧 VM 同一口径（`gpuscale::presetFor`）
QString UiModel::scalerPreset() const
{
    const AppSettings &s = m_settings;
    const gpuscale::Params p = gpuscale::paramsFromStrings(s.scalerMode(), s.scalerQuality(),
                                                       s.scalerFilter(), s.scalerLobes(),
                                                       s.scalerMitchellB(), s.scalerMitchellC(),
                                                       s.scalerAntiring());
    const gpuscale::Preset *m = gpuscale::presetFor(p);
    return m ? QString::fromLatin1(m->key) : QString();
}

QVariantList UiModel::scalerPresetList() const
{
    const QString cur = scalerPreset();
    QVariantList out;
    for (const gpuscale::Preset &p : gpuscale::presets()) {
        QVariantMap m;
        m.insert(QStringLiteral("key"), QString::fromLatin1(p.key));
        m.insert(QStringLiteral("label"), QString::fromUtf8(p.label));
        m.insert(QStringLiteral("desc"), QString::fromUtf8(p.desc));
        m.insert(QStringLiteral("selected"), cur == QLatin1String(p.key));
        out << m;
    }
    return out;
}

void UiModel::applyScalerPreset(const QString &key)
{
    const gpuscale::Preset *pre = gpuscale::presetByKey(key);
    if (!pre) return;
    const gpuscale::Params &p = pre->params;
    m_settings.setScalerParams(QString::fromLatin1(gpuscale::modeName(p.mode)),
                               QString::fromLatin1(gpuscale::qualityName(p.quality)),
                               QString::fromLatin1(gpuscale::filterName(p.filter)),
                               p.lobes, p.mitchellB, p.mitchellC, p.antiring);
    emit scalerModeChanged();
    emit scalerQualityChanged();
    emit scalerFilterChanged();
    emit scalerLobesChanged();
    emit scalerMitchellBChanged();
    emit scalerMitchellCChanged();
    emit scalerAntiringChanged();
    emit scalerParamsChanged();
}

} // namespace app

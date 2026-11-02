#include "uimodel.h"

#include "appsettings.h"
#include "archive.h"                // ArchiveSource::isArchive（"是不是压缩包"的唯一实现）
#include "natural.h"                // imageSuffixes()：图片扩展名的唯一列表
#include "gpuscale.h"
#include "progressfile.h"
#include "progresslistmodel.h"
#include "sequence.h"
#include "viewermodel.h"

#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMetaObject>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>

namespace app {

//   扫描结束提示的最短时长：低于它不弹（小目录瞬间扫完，弹 toast 只是噪音）
//   验收可调小：`PHOTOSET_SCAN_TOAST_MS`（同自动保存那个 env 的口径）。
constexpr qint64 kScanToastMinMs = 10000;

UiModel::UiModel(QObject *parent)
    : QObject(parent)
{
    // ★★ 运行期设置的**唯一实例**在这里 load（时机等价：本类被建出来之后才有人读设置）。
    m_settings.load();

    // 来源失败 / 不支持 / 扫描期提示：`Sequence::unsupportedReason()` 与 `noticeText()`
    //   由**扫描线程**产出（7z / RAR 的探针很贵，绝不能放 GUI 线程）⇒ 这里起一个便宜的
    //   轮询定时器，扫完（或已经给出原因）就停。
    //   ★ 一律走 `notify`，**不是**失败状态：序列本身照常收尾（空序列 / 少几条）。
    m_sessionProbe = new QTimer(this);
    m_sessionProbe->setInterval(300);
    connect(m_sessionProbe, &QTimer::timeout, this, [this] {
        Sequence *seq = m_src.seq;
        if (!seq) { m_sessionProbe->stop(); return; }
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
        refreshScanning();
    });

    //   自动保存（用户 2026-11-01）：**只对"已关联进度文件"的来源**生效，固定间隔写一次；
    //   切来源 / 退出前再补一次（见 `dropSource` / `~UiModel`）
    m_autosave = new QTimer(this);
    //   自检用：`PHOTOSET_AUTOSAVE_MS` 可把间隔调小（不设 = 默认 30 s；正式行为不受影响）
    const int autosaveOverride = qEnvironmentVariableIntValue("PHOTOSET_AUTOSAVE_MS");
    m_autosave->setInterval(autosaveOverride > 0 ? autosaveOverride : kAutosaveMs);
    connect(m_autosave, &QTimer::timeout, this, &UiModel::autosaveProgress);

    //   收藏夹列表（QML 的 `ui.favorites`）：目录 = 收藏夹；只列文件名，内容按需读
    m_favorites = new ProgressListModel(this);
    m_favorites->setDirectory(favoritesDir());

    // ★ `ui.viewer` 在**没打开来源时就是 null**（QML 一律先判非空）。
    qInfo().noquote() << QStringLiteral("[ui] 初始化：只有一个查看界面；打开过的来源不写盘");
}

QObject *UiModel::viewer() const { return m_src.vm; }

UiModel::~UiModel()
{
    cancelRescan();              // 退出时后台重扫作废
    if (m_inhibitCookie && m_src.vm) { m_src.vm->setPlaying(false); updateScreenSaverInhibit(); }
    autosaveProgress();          // 退出前补一次（有变化且条件满足才写）
    releaseNow(m_src);
}

// ── 来源的创建 / 释放 ────────────────────────────────────────────────────
void UiModel::releaseNow(Source &s)
{
    // 顺序：**先消费者**。`~ViewerModel` **不 join 工作线程**（只换个记号就走，见它自己的说明）
    //   ⇒ 这里立刻返回，之后才轮到序列。
    delete s.vm;    s.vm = nullptr;
    delete s.seq;   s.seq = nullptr;
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
    QObject::connect(s.vm, &QObject::destroyed, [seq] { delete seq; });
    s.vm->deleteLater();      // ★ 不许同步 delete（QML 绑定刚换绑，事件循环还没转到现在）
}

// 换来源 / 退出：先把旧组**同步**拆掉（`~ViewerModel` 只换记号，立刻返回）
void UiModel::dropSource()
{
    //   ★ 运行时序列要销毁了（切来源 / 退出）：先把进度补写一次（用户 2026-11-01）
    cancelRescan();              // 后台重扫一并作废（不替换）
    autosaveProgress();
    stopTrackingProgress();      // 新来源会重新决定"关不关联"
    if (m_src.vm) m_src.vm->setPlaying(false);
    if (!m_src.vm && !m_src.seq) return;
    Source old = m_src;
    m_src = Source{};
    emit viewerChanged();
    releaseNow(old);
}

// ── 打开来源 ────────────────────────────────────────────────────────────
//   QML 的拖放入口：`drop.urls` 给的是 **file: URL**；只接本地路径（其它 scheme 一律忽略）
void UiModel::openPath(const QUrl &fileUrl)
{
    if (fileUrl.isLocalFile()) {
        openPath(fileUrl.toLocalFile(), -1);
        return;
    }
    if (fileUrl.scheme().isEmpty() && !fileUrl.toString().isEmpty())
        openPath(fileUrl.toString(), -1);      // 容错：调用方直接给了裸路径字符串
}

//   ★ 只有一种来源：**会话来源**（文件夹 / 压缩包 / 文件同一套：建序列 → 看）。
//     · 不写盘：进度与扫描结果都只在内存里（用户 2026-10-31）；
//     · 换来源 ⇒ 旧组同步拆掉（不等工作线程）；
//     · `index` 只给 CLI `--index N` 用（界面入口不传）。
void UiModel::openPath(const QString &path, int index)
{
    if (path.isEmpty()) return;
    dropSource();
    m_path = path;
    m_sessionNotice.clear();                  // 换来源 ⇒ 瞬时提示重新计
    buildViewer();
    refreshScanning();
    refreshFavorited();
    if (m_sessionProbe) m_sessionProbe->start();
    //   `--index N`：扫描是异步的 ⇒ 记下来，等条目够了再跳（见 onSequenceChanged）
    m_pendingJump = index;
    m_pendingChain.clear();
    if (m_pendingJump >= 0 && m_src.vm && m_src.seq && m_src.seq->count() > m_pendingJump) {
        m_src.vm->jump(m_pendingJump);
        m_pendingJump = -1;
    }
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
    //   条目里的**磁盘路径 + 容器成员链 + 页号**由它派生 —— 那是解码唯一认识的形态）。
    s.seq->startScan(m_path, m_settings.recurseDepth(), m_settings.scanHidden(),
                     Sequence::ScanOptions{ qint64(m_settings.innerArchiveMaxMb()) });
    // 本类只给 viewer **预取长度**（它不读设置）
    s.vm = new fetch::ViewerModel(m_settings.prefetchWindow(), nullptr);
    connect(s.vm, &fetch::ViewerModel::playingChanged, this, &UiModel::updateScreenSaverInhibit);
    installViewer(s);
    // 打开 ⇒ 看第 0 条（"进入查看 = jump"）：一进来就有画面
    //   （扫描还在跑时由 viewer 的 `loading` 表达）
    s.vm->jump(0);
}

// 换 viewer（**接线只在这里**）：装新组 → emit viewerChanged → 旧组延迟释放
void UiModel::installViewer(Source s)
{
    //   视图态复位由 viewer 自己在上屏时做（`zoomLocked` 时保留）——不再从上层挂连接。
    // ★ 幻灯片：间隔由上层（设置）下发；到头等瞬时提示由上层转发到 `notify`
    s.vm->setIntervalMs(m_settings.intervalMs());
    connect(s.vm, &fetch::ViewerModel::notice, this, &UiModel::notify);
    //   自动保存的"有变化"：位置或版面变了 ⇒ 下一个间隔点写一次
    connect(s.vm, &fetch::ViewerModel::currentChanged, this, [this] { markProgressDirty(); });
    connect(s.vm, &fetch::ViewerModel::doublePageChanged, this, [this] { markProgressDirty(); });
    // ★ 本类与 viewer 之间只剩"**给序列 + 序列变了通知**"：位置 / 预取 / 上屏 / loading
    //   全归 viewer 自己。
    const Source old = m_src;
    m_src = s;
    emit viewerChanged();            // ★ 先让 QML 换绑到新 viewer
    if (m_src.vm) m_src.vm->setSequence(m_src.seq);
    applyViewerDefaults();           // ★ 新建 viewer ⇒ 按全局默认起一份阅读态
    if (old.vm) releaseLater(old);   // ★ 再让旧组延迟释放
}

// 设置类变化 ⇒ 换一组（"重建 = 换实现，不是换来源" ⇒ 位置带过去）
void UiModel::rebuildViewer()
{
    if (!m_src.vm) return;              // 还没建过来源 ⇒ 没什么可重建
    //   ★ 重新扫描后条目集合可能变（文件增删）⇒ **不能按旧下标跳**，按 label 链 + 页号找回
    //     （新序列是异步长出来的：见 `onSequenceChanged` 里的 pending 处理）
    const int cursor = m_src.vm->cursor();
    m_pendingChain = m_src.seq ? m_src.seq->chainAt(cursor) : QStringList();
    m_pendingPage = m_src.seq ? m_src.seq->pageAt(cursor) : 0;
    m_pendingJump = -1;
    buildViewer();
    if (!m_src.vm) return;
    if (m_pendingChain.isEmpty())
        m_src.vm->jump(qMax(0, cursor));      // 没有链（空序列）⇒ 退回按下标
    markProgressDirty();                      // 磁盘内容可能变了 ⇒ 让自动保存重写一次
}

// ── 序列 / viewer 的通知（**只在 GUI 线程**）─────────────────────────────
// 会话序列：**本类是唯一消费者** —— 发布钩子唤醒（合并成一次）⇒ 一次 drain 到空 ⇒ 通知
void UiModel::pumpSession()
{
    Sequence *seq = m_src.seq;
    if (!seq) return;
    if (seq->drain()) onSequenceChanged();
}

// 序列变了（放了新批 / 收尾）⇒ 通知 viewer（序列长进了）
void UiModel::onSequenceChanged()
{
    if (m_src.vm) m_src.vm->sequenceChanged();
    //   CLI `--index N` 的延迟跳转：条目够了就跳一次；扫完了还没到 N 就放弃
    if (m_pendingJump >= 0 && m_src.vm && m_src.seq) {
        if (m_src.seq->count() > m_pendingJump) {
            m_src.vm->jump(m_pendingJump);
            m_pendingJump = -1;
        } else if (m_src.seq->scanFinished()) {
            m_pendingJump = -1;
        }
    }
    //   重新扫描后的"按链找回位置"：条目够了就跳一次；扫完还找不到就放弃
    if (!m_pendingChain.isEmpty() && m_src.vm && m_src.seq) {
        if (m_src.seq->count() > 0) {
            const int idx = m_src.seq->indexOfChain(m_pendingChain, m_pendingPage);
            if (idx >= 0) {
                m_src.vm->jump(idx);
                m_pendingChain.clear();
            } else if (m_src.seq->scanFinished()) {
                m_pendingChain.clear();
            }
        } else if (m_src.seq->scanFinished()) {
            m_pendingChain.clear();
        }
    }
    refreshScanning();               // 顶栏 spin / 已发现数
}

// ── 打开对话框（**QFileDialog 只在这里**；入口随右键菜单那一批接上）──────────
//   起始目录：**上次成功选过的目录** ⇒ 当前来源的父目录 ⇒ home。
//   （不要每次都从 home 起：home 里若有几十万个文件，KDE 的对话框会先把子目录条目数算一遍，
//     表现就是"打开半天没反应"。会话级记忆，不写设置文件。）
QString UiModel::dialogStartDir() const
{
    if (!m_lastDialogDir.isEmpty()) return m_lastDialogDir;
    if (!m_path.isEmpty()) {
        const QFileInfo fi(m_path);
        return fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    }
    return QDir::homePath();
}

void UiModel::chooseOpenFolder()
{
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    const QString start = dialogStartDir();
    const QString dir = QFileDialog::getExistingDirectory(
        nullptr, QStringLiteral("打开文件夹"), start);
    qInfo().noquote() << QStringLiteral("[dialog] 打开文件夹：%1ms（起始 %2）")
                             .arg(QDateTime::currentMSecsSinceEpoch() - t0).arg(start);
    if (dir.isEmpty()) return;
    m_lastDialogDir = QFileInfo(dir).absolutePath();
    openPath(dir);
}

void UiModel::chooseOpenArchive()
{
    const QString filter = QStringLiteral(
        "压缩包 (*.zip *.cbz *.rar *.cbr *.7z *.cb7 *.tar *.cbt *.gz *.bz2 *.xz *.zst);;所有文件 (*)");
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    const QString start = dialogStartDir();
    const QString path = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("打开压缩包"), start, filter);
    qInfo().noquote() << QStringLiteral("[dialog] 打开压缩包：%1ms（起始 %2）")
                             .arg(QDateTime::currentMSecsSinceEpoch() - t0).arg(start);
    if (path.isEmpty()) return;
    m_lastDialogDir = QFileInfo(path).absolutePath();
    openPath(path);
}

void UiModel::chooseOpenImage()
{
    //   过滤器 = **我们认得的扩展名**（`natural::imageSuffixes()`，唯一来源）∩ **系统真能解码的**
    //   （`QImageReader`）。别直接用 QImageReader 的全量列表 —— 本机它会给出 **101 项**，
    //   连 pdf / dds / psd / raw 这些我们根本不支持的也在里面（2026-11-01 实测）。
    //   HEIC/HEIF 按定案（2026-11-01「不考虑支持」）不列进过滤器。
    QSet<QByteArray> decodable;
    for (const QByteArray &f : QImageReader::supportedImageFormats()) decodable.insert(f);
    QStringList pats;
    for (const QString &s : imageSuffixes()) {
        if (s == QLatin1String("heic") || s == QLatin1String("heif")) continue;
        if (!decodable.contains(s.toLatin1())) continue;
        pats << QStringLiteral("*.") + s;
    }
    const QString filter = QStringLiteral("图片 (%1);;所有文件 (*)").arg(pats.join(QLatin1Char(' ')));
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    const QString start = dialogStartDir();
    const QString path = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("打开图片"), start, filter);
    qInfo().noquote() << QStringLiteral("[dialog] 打开图片：%1ms（起始 %2）")
                             .arg(QDateTime::currentMSecsSinceEpoch() - t0).arg(start);
    if (path.isEmpty()) return;
    m_lastDialogDir = QFileInfo(path).absolutePath();
    openPath(path);
}

// ★ 瞬时提示（**不占位浮层**）：不写入磁盘、不进设置；`seq` 自增让同一条文案也能重新动画
void UiModel::notify(const QString &text)
{
    if (text.isEmpty()) return;
    m_notification = text;
    ++m_notificationSeq;
    qInfo().noquote() << QStringLiteral("[ui] 瞬时提示：%1").arg(text);
    emit notificationChanged();
}

// ── 幻灯片间隔（值全在 `m_settings`；本类只发通知）─────────────────────────
int UiModel::slideshowIntervalMs() const { return m_settings.intervalMs(); }

void UiModel::setSlideshowIntervalMs(int ms)
{
    const int v = qMax(50, ms);                 // 下限 50ms（与 AppSettings 同一条）
    if (m_settings.intervalMs() != v) m_settings.setIntervalMs(v);
    // ★ 这是**全局默认**：只落盘，不动当前 viewer（当前间隔在 `ui.viewer.intervalMs`）
    emit slideshowIntervalMsChanged();
}

// ── 全局默认 ───────────────────────────────────────────────────────────
int UiModel::fitMode() const { return m_settings.defaultFitMode(); }

void UiModel::setFitMode(int m)
{
    m = qBound(0, m, 3);
    if (m_settings.defaultFitMode() == m) return;
    m_settings.setDefaultFitMode(m);
    emit fitModeChanged();
}

int UiModel::defaultLayout() const { return m_settings.defaultLayout(); }

void UiModel::setDefaultLayout(int v)
{
    v = qBound(0, v, 1);
    if (m_settings.defaultLayout() == v) return;
    m_settings.setDefaultLayout(v);
    emit defaultLayoutChanged();
}

int UiModel::defaultLoop() const { return m_settings.defaultLoop(); }

void UiModel::setDefaultLoop(int v)
{
    v = qBound(0, v, 2);
    if (m_settings.defaultLoop() == v) return;
    m_settings.setDefaultLoop(v);
    emit defaultLoopChanged();
}

// 新建 viewer ⇒ 把全局默认下发一次（之后 UI 怎么改都只改 viewer）
void UiModel::applyViewerDefaults()
{
    fetch::ViewerModel *vm = m_src.vm;
    if (!vm) return;
    vm->setFitMode(m_settings.defaultFitMode());          // 触发一次 applyFit（zoom 由 fit 算）
    vm->setIntervalMs(m_settings.intervalMs());
    vm->setDoublePage(m_settings.defaultLayout() == 1);
    vm->setLoopMode(m_settings.defaultLoop());
    vm->setScaleAlgorithm(m_settings.scaleAlgorithm());
    vm->setOsdVisible(false);                             // OSD 是偶发使用，每次进来默认关
}

// ── UI 形态（会话级）────────────────────────────────────────────────────
void UiModel::setWindowFullscreen(bool on)
{
    if (m_windowFullscreen == on) return;
    m_windowFullscreen = on;
    emit windowFullscreenChanged();
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

int UiModel::innerArchiveMaxMb() const { return m_settings.innerArchiveMaxMb(); }

void UiModel::setInnerArchiveMaxMb(int v)
{
    v = qBound(64, v, 8192);
    if (m_settings.innerArchiveMaxMb() == v) return;
    m_settings.setInnerArchiveMaxMb(v);
    emit innerArchiveMaxMbChanged();
    // ★ v7：**不重建 viewer**（避免无谓的重新扫描）；上限在"下一次展开"生效。
}

// ── 图像（缩放）：唯一选项 = 算法 ──────────────────────────────────────────
QString UiModel::scaleAlgorithm() const { return m_settings.scaleAlgorithm(); }

void UiModel::setScaleAlgorithm(const QString &v)
{
    // 不认识的值一律回默认（lanczos3）：宁可"没改成"，也不要留下一个渲染不出的键
    const gpuscale::Algorithm a = gpuscale::algorithmFromKey(v);
    const QString key = QLatin1String(gpuscale::algorithmKey(a));
    if (m_settings.scaleAlgorithm() == key) return;
    m_settings.setScaleAlgorithm(key);
    emit scaleAlgorithmChanged();
}

void UiModel::setSoftwareBackend(bool on)
{
    if (m_softwareBackend == on) return;
    m_softwareBackend = on;
    emit softwareBackendChanged();
}

// ══ 收藏夹 / 进度文件（设计 20）═════════════════════════════════════════════
QString UiModel::favoritesDir() const
{
    QString dir = m_settings.favoritesDir();
    if (dir.isEmpty())
        dir = QFileInfo(m_settings.filePath()).absolutePath() + QStringLiteral("/收藏");
    return QDir::cleanPath(dir);
}

void UiModel::setFavoritesDir(const QString &dir)
{
    const QString v = QDir::cleanPath(dir);
    if (favoritesDir() == v)
        return;
    m_settings.setFavoritesDir(v);
    emit favoritesDirChanged();
    if (m_favorites) m_favorites->setDirectory(favoritesDir());
    refreshFavorited();                 // 换了目录 ⇒ "已收藏"的判断跟着变
}

void UiModel::chooseFavoritesDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        nullptr, QStringLiteral("选择收藏夹目录"), favoritesDir());
    if (dir.isEmpty())
        return;
    setFavoritesDir(dir);
}

bool UiModel::scanning() const
{
    if (m_rescanSeq) return true;                       // 后台重扫也算"在扫"
    return m_src.seq && !m_src.seq->scanFinished();
}

int UiModel::discoveredCount() const
{
    if (m_rescanSeq) return m_rescanSeq->discoveredCount();
    return m_src.seq ? m_src.seq->discoveredCount() : 0;
}

//   幻灯片 = 长时间无人操作地看图 ⇒ 别让屏幕在播放中途休眠。
//   走桌面标准的 ScreenSaver 接口（KDE/GNOME 都实现）；没有会话总线就静默跳过。
void UiModel::updateScreenSaverInhibit()
{
    const bool want = m_src.vm && m_src.vm->playing();
    if (!QDBusConnection::sessionBus().isConnected())
        return;
    if (want && m_inhibitCookie == 0) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("/org/freedesktop/ScreenSaver"),
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("Inhibit"));
        msg << QStringLiteral("photoset-reader") << QStringLiteral("幻灯片播放中");
        const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 500);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            m_inhibitCookie = reply.arguments().first().toUInt();
    } else if (!want && m_inhibitCookie != 0) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("/org/freedesktop/ScreenSaver"),
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("UnInhibit"));
        msg << m_inhibitCookie;
        QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
        m_inhibitCookie = 0;
    }
}

void UiModel::refreshScanning()
{
    const bool now = scanning();
    const int  n = discoveredCount();
    //   扫描时长 ≥ 10 s 才弹完成提示（用户 2026-11-02）：小目录瞬间扫完，弹 toast 只是噪音。
    if (now && !m_scanning) {
        m_scanStartedMs = QDateTime::currentMSecsSinceEpoch();
    } else if (!now && m_scanning && m_scanStartedMs > 0) {
        const qint64 ms = QDateTime::currentMSecsSinceEpoch() - m_scanStartedMs;
        m_scanStartedMs = 0;
        const int override = qEnvironmentVariableIntValue("PHOTOSET_SCAN_TOAST_MS");
        const qint64 threshold = override > 0 ? override : kScanToastMinMs;
        if (ms >= threshold)
            notify(QStringLiteral("扫描完成：%1 个文件").arg(m_src.seq ? m_src.seq->count() : 0));
    }
    //   被用户停下的扫描 = **不完整**（ScanStoped；ScanStoped 也算 scanFinished）
    const bool incomplete = m_src.seq && m_src.seq->state() == Sequence::State::ScanStoped;
    const bool canRescan = canRescanNow();
    if (now != m_scanning || n != m_discovered || canRescan != m_canRescan) {
        m_scanning = now;
        m_discovered = n;
        m_canRescan = canRescan;
        emit scanningChanged();
    }
    if (incomplete != m_scanIncomplete) {
        m_scanIncomplete = incomplete;
        emit scanIncompleteChanged();
    }
}

bool UiModel::canRescan() const { return m_canRescan; }

//   内部用的实时判断（`m_canRescan` 只是缓存给 QML 的那份）
bool UiModel::canRescanNow() const
{
    if (!m_src.seq || !m_src.vm || m_path.isEmpty())
        return false;
    if (scanning())
        return false;
    const QFileInfo fi(m_path);
    return fi.isDir() || ArchiveSource::isArchive(m_path);
}

//   重新扫描当前来源（右键菜单）：**后台单开一条序列**，扫完再替换当前浏览的
void UiModel::rescanSource()
{
    startRescan();
}

void UiModel::startRescan()
{
    if (m_path.isEmpty() || m_rescanSeq || !m_src.vm || !m_src.seq)
        return;
    if (!m_src.seq->scanFinished()) {      // 当前这条还在扫 ⇒ 先别再来一条
        notify(QStringLiteral("正在扫描，等扫完再重新扫描"));
        return;
    }
    if (!canRescanNow())
        return;

    //   记下当前位置（**链 + 页号**，不是下标：扫完条目集合可能变）
    const int cursor = m_src.vm->cursor();
    m_rescanChain = m_src.seq->chainAt(cursor);
    m_rescanPage = m_src.seq->pageAt(cursor);

    m_rescanSeq = new Sequence;
    //   与会话序列同一套唤醒：生产者放完一批 / 写终态 ⇒ 合并成一次 queued 投递
    m_rescanSeq->setPublishHook([this] {
        if (m_rescanQueued.exchange(true)) return;
        QMetaObject::invokeMethod(this, [this] {
            m_rescanQueued.store(false);
            onRescanChanged();
        }, Qt::QueuedConnection);
    });
    m_rescanSeq->startScan(m_path, m_settings.recurseDepth(), m_settings.scanHidden(),
                           Sequence::ScanOptions{ qint64(m_settings.innerArchiveMaxMb()) });
    qInfo().noquote() << QStringLiteral("[seq] 后台重扫开始：%1").arg(m_path);
    refreshScanning();
}

//   后台重扫的消费点（GUI 线程；合并投递）：drain 到空，**扫完才替换**
void UiModel::onRescanChanged()
{
    if (!m_rescanSeq)
        return;
    m_rescanSeq->drain();
    if (!m_rescanSeq->scanFinished()) {
        refreshScanning();                 // spin 的"已发现 N"
        return;
    }
    applyRescan();
}

void UiModel::applyRescan()
{
    Sequence *fresh = m_rescanSeq;
    m_rescanSeq = nullptr;
    if (!fresh)
        return;
    if (!m_src.vm || !m_src.seq) {
        delete fresh;
        refreshScanning();
        return;
    }
    if (fresh->count() <= 0) {
        //   一条都没有（源不在了 / 打不开）⇒ **保留原来的**，不把画面变空
        const QString why = fresh->unsupportedReason();
        delete fresh;
        notify(why.isEmpty() ? QStringLiteral("重新扫描没有扫到内容，保留原来的")
                             : why);
        refreshScanning();
        return;
    }

    Sequence *old = m_src.seq;
    m_src.seq = fresh;
    m_src.vm->setSequence(fresh);          // 位置归零 + 清槽（版面/缩放/循环保持）
    const int idx = m_rescanChain.isEmpty()
                        ? -1
                        : fresh->indexOfChain(m_rescanChain, m_rescanPage);
    m_src.vm->jump(idx >= 0 ? idx : 0);
    delete old;                            // 旧序列已不被 viewer 引用
    qInfo().noquote() << QStringLiteral("[seq] 后台重扫完成，已替换当前序列（%1 条，落到 %2）")
                             .arg(fresh->count()).arg(qMax(0, idx));
    m_rescanChain.clear();
    markProgressDirty();                   // 内容可能变了 ⇒ 让自动保存按新 rows 更新
    refreshFavorited();
    refreshScanning();
}

//   作废：停止 + 析构，**不做替换**（取消 / 退出时走这里）
void UiModel::cancelRescan()
{
    if (!m_rescanSeq)
        return;
    Sequence *s = m_rescanSeq;
    m_rescanSeq = nullptr;
    s->requestStop();
    delete s;                              // 析构里 join 生产者（已置停止位，通常立刻退出）
    m_rescanChain.clear();
    qInfo().noquote() << QStringLiteral("[seq] 后台重扫已作废");
    refreshScanning();
}


// ══ 收藏夹 / 进度文件（设计 20）═════════════════════════════════════════════
//   当前来源在收藏夹里对应的文件（空 = 没来源 / 来源不是文件夹或压缩包）
QString UiModel::currentFavoritesFile() const
{
    if (m_path.isEmpty())
        return QString();
    const QFileInfo fi(m_path);
    if (!fi.isDir() && !ArchiveSource::isArchive(m_path))
        return QString();               // 单张图没有"进度"可说
    return favoritesDir() + QLatin1Char('/')
           + progressfile::fileNameFor(progressfile::titleOf(m_path), m_path);
}

void UiModel::refreshFavorited()
{
    const QString file = currentFavoritesFile();
    const bool now = !file.isEmpty() && QFileInfo::exists(file);
    if (now == m_sourceFavorited)
        return;
    m_sourceFavorited = now;
    emit sourceFavoritedChanged();
}

//   把当前来源写成 filePath。**元数据里所有"进度相关"的字段都按**当前浏览状态**取**
//   —— 位置（label 链 + 页号）、第 N 条 / 共 M 条、版面（单/双）；不用任何缓存值。
bool UiModel::writeProgress(const QString &filePath, bool quiet)
{
    if (!m_src.seq || !m_src.vm) {
        if (!quiet) notify(QStringLiteral("没有打开的来源"));
        return false;
    }
    if (!m_src.seq->scanFinished()) {
        //   只有**扫描完**的才能收藏（用户 2026-10-31）—— 点的时候会先弹框告知，这里是兜底
        if (!quiet) notify(QStringLiteral("扫描还没完，等扫完再保存"));
        return false;
    }
    const QFileInfo fi(m_path);
    const bool isDir = fi.isDir();
    if (!isDir && !ArchiveSource::isArchive(m_path)) {
        if (!quiet) notify(QStringLiteral("只有文件夹和压缩包能保存进度"));
        return false;
    }

    m_src.seq->drain();                 // 收尾那一批可能还没取（保证 rows 完整）

    const int cursor = m_src.vm->cursor();

    progressfile::Data d;
    d.meta.savedAt = QDateTime::currentSecsSinceEpoch();
    d.meta.title = progressfile::titleOf(m_path);
    d.meta.sourcePath = fi.absoluteFilePath();
    d.meta.sourceKind = isDir ? Sequence::Kind::Folder : Sequence::Kind::Archive;
    d.meta.sourceMtime = fi.lastModified().toSecsSinceEpoch();
    d.meta.hidden = m_settings.scanHidden();
    d.meta.depth = m_settings.recurseDepth();
    d.meta.innerArchiveMaxMb = m_settings.innerArchiveMaxMb();
    //   ★ 进度相关：全部取**当前浏览状态**
    d.meta.layout = m_src.vm->doublePage() ? 1 : 0;
    d.meta.chain = m_src.seq->chainAt(cursor);
    d.meta.page = m_src.seq->pageAt(cursor);
    d.meta.done = cursor + 1;
    d.meta.total = m_src.seq->count();
    d.rows = m_src.seq->rows();

    QString err;
    if (!progressfile::write(filePath, d, &err)) {
        if (!quiet) notify(err.isEmpty() ? QStringLiteral("保存进度失败") : err);
        else qWarning().noquote() << QStringLiteral("[progress] 自动保存失败：%1").arg(err);
        return false;
    }
    qInfo().noquote() << QStringLiteral("[progress] 已保存 %1（%2 条）")
                             .arg(filePath).arg(d.rows.size());
    snapshotSavedState(d.meta, d.rows.size());
    return true;
}

bool UiModel::saveProgressToFavorites()
{
    const QString file = currentFavoritesFile();
    if (file.isEmpty()) {
        notify(m_path.isEmpty() ? QStringLiteral("没有打开的来源")
                                : QStringLiteral("只有文件夹和压缩包能保存进度"));
        return false;
    }
    if (!writeProgress(file))
        return false;
    refreshFavorited();
    if (m_favorites) m_favorites->reload();      // 列表里能立刻看到
    startTrackingProgress(file);                 // 之后按间隔 / 切来源自动保存
    notify(QStringLiteral("已收藏"));
    return true;
}

bool UiModel::saveProgressToFile()
{
    if (!m_src.seq || !m_src.vm) {
        notify(QStringLiteral("没有打开的来源"));
        return false;
    }
    const QString start = m_lastProgressDir.isEmpty()
                              ? QDir::homePath()
                              : m_lastProgressDir + QLatin1Char('/')
                                    + progressfile::titleOf(m_path) + QLatin1String(".prsave");
    const QString picked = QFileDialog::getSaveFileName(
        nullptr, QStringLiteral("保存进度到文件"), start,
        QStringLiteral("进度文件 (*.prsave)"));
    if (picked.isEmpty())
        return false;
    QString file = picked;
    if (!file.endsWith(QLatin1String(".prsave"), Qt::CaseInsensitive))
        file += QLatin1String(".prsave");
    if (!writeProgress(file))
        return false;
    m_lastProgressDir = QFileInfo(file).absolutePath();
    startTrackingProgress(file);
    notify(QStringLiteral("进度已保存"));
    return true;
}

void UiModel::openProgressFile()
{
    const QString picked = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("打开进度文件"),
        m_lastProgressDir.isEmpty() ? QDir::homePath() : m_lastProgressDir,
        QStringLiteral("进度文件 (*.prsave)"));
    if (picked.isEmpty())
        return;
    m_lastProgressDir = QFileInfo(picked).absolutePath();
    openProgress(picked);
}

//   打开一份进度文件：**只按快照恢复，不扫描**（用户 2026-10-31）
bool UiModel::openProgress(const QString &filePath)
{
    progressfile::Data d;
    QString err;
    if (!progressfile::read(filePath, &d, &err)) {
        notify(err.isEmpty() ? QStringLiteral("打不开进度文件") : err);
        return false;
    }
    if (!QFileInfo::exists(d.meta.sourcePath)) {
        notify(QStringLiteral("源不在了：%1").arg(d.meta.sourcePath));
        return false;
    }

    dropSource();

    Source s;
    s.seq = new Sequence;
    //   ★ 恢复 = 行 → 序列（**同一份 `Sequence::Row`**，与扫描共用一套表示）
    s.seq->restore(d.meta.sourcePath, d.rows, d.meta.sourceKind == Sequence::Kind::Archive);
    s.vm = new fetch::ViewerModel(m_settings.prefetchWindow(), nullptr);
    m_path = d.meta.sourcePath;
    installViewer(s);
    if (m_src.vm) {
        m_src.vm->setDoublePage(d.meta.layout == 1);        // ★ 先还原版面，再按 chain/page 定位
        const int idx = m_src.seq->indexOfChain(d.meta.chain, d.meta.page);
        m_src.vm->jump(qMax(0, idx));
    }
    m_pendingJump = -1;
    m_pendingChain.clear();
    startTrackingProgress(filePath);                   // 之后按间隔 / 切来源自动保存
    snapshotSavedState(d.meta, d.rows.size());         // 文件里记着的就是这份
    if (m_sessionProbe) m_sessionProbe->stop();        // 恢复的序列已经收尾，不需要探测
    m_sessionNotice.clear();
    refreshScanning();
    refreshFavorited();
    qInfo().noquote() << QStringLiteral("[progress] 已恢复 %1（%2 条，落到 %3）")
                             .arg(filePath).arg(d.rows.size()).arg(d.meta.done);
    return true;
}

//   收藏夹列表里的"移除"：删文件（不动源）⇒ 刷新"已收藏" + 列表
bool UiModel::removeProgressFile(const QString &filePath)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath))
        return false;
    if (!QFile::remove(filePath)) {
        notify(QStringLiteral("删不掉：%1").arg(filePath));
        return false;
    }
    qInfo().noquote() << QStringLiteral("[progress] 已删除 %1").arg(filePath);
    if (QFileInfo(filePath).absoluteFilePath() == QFileInfo(m_progressFile).absoluteFilePath())
        stopTrackingProgress();                    // 别在下一个间隔点把它"复活"
    refreshFavorited();
    if (m_favorites) m_favorites->reload();
    return true;
}

bool UiModel::removeFavorite()
{
    const QString file = currentFavoritesFile();
    if (file.isEmpty() || !QFileInfo::exists(file))
        return false;
    if (!QFile::remove(file)) {
        notify(QStringLiteral("删不掉：%1").arg(file));
        return false;
    }
    refreshFavorited();
    if (m_favorites) m_favorites->reload();
    qInfo().noquote() << QStringLiteral("[progress] 已删除 %1").arg(file);
    stopTrackingProgress();
    return true;
}

// ══ 自动保存（用户 2026-11-01）═══════════════════════════════════════════════
void UiModel::autosaveProgress()
{
    if (m_progressFile.isEmpty() || !m_progressDirty)
        return;
    if (!m_src.seq || !m_src.vm)
        return;
    if (m_src.seq->state() != Sequence::State::Scaned)
        return;
    if (!QFileInfo::exists(m_progressFile)) {
        qInfo().noquote() << QStringLiteral("[progress] 关联的文件不在了，停止自动保存：%1")
                                 .arg(m_progressFile);
        stopTrackingProgress();
        return;
    }
    if (!progressChangedSinceSave()) {      // 只是被标记过，内容没变 ⇒ 不重写
        m_progressDirty = false;
        return;
    }
    if (!writeProgress(m_progressFile, /*quiet=*/true))
        return;
    m_progressDirty = false;
    qInfo().noquote() << QStringLiteral("[progress] 自动保存 %1").arg(m_progressFile);
}

//   文件里"已经记着"的状态 = **刚写进文件 / 刚从文件读出**的那一份（不是运行态：
//   恢复时 viewer 的位置是异步落定的，读运行态会把 0 误记成"已保存"）
void UiModel::snapshotSavedState(const progressfile::Meta &m, int rowCount)
{
    m_savedChain = m.chain;
    m_savedPage = m.page;
    m_savedLayout = m.layout;
    m_savedRows = rowCount;
}

bool UiModel::progressChangedSinceSave() const
{
    if (!m_src.seq || !m_src.vm)
        return false;
    const int cursor = m_src.vm->cursor();
    return m_src.seq->chainAt(cursor) != m_savedChain
           || m_src.seq->pageAt(cursor) != m_savedPage
           || (m_src.vm->doublePage() ? 1 : 0) != m_savedLayout
           || m_src.seq->rows().size() != m_savedRows;
}

void UiModel::startTrackingProgress(const QString &file)
{
    m_progressFile = file;
    m_progressDirty = false;              // 刚写过 / 刚读过 ⇒ 不算有变化
    if (m_autosave && !file.isEmpty())
        m_autosave->start();
    else if (m_autosave)
        m_autosave->stop();
}

void UiModel::stopTrackingProgress()
{
    m_progressFile.clear();
    m_progressDirty = false;
    if (m_autosave) m_autosave->stop();
}

void UiModel::cancelScan()
{
    if (m_rescanSeq) {                   // 后台重扫在跑 ⇒ 作废它（当前视图不受影响）
        cancelRescan();
        return;
    }
    if (!m_src.seq || m_src.seq->scanFinished())
        return;
    m_src.seq->requestStop();            // 只是请求：生产者自己退，已有内容照旧可看
    qInfo().noquote() << QStringLiteral("[seq] 用户请求停止扫描");
}

} // namespace app

#include "uimodel.h"

#include "appsettings.h"
#include "gpuscale.h"
#include "sequence.h"
#include "viewermodel.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMetaObject>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>

namespace app {

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
    });

    // ★ `ui.viewer` 在**没打开来源时就是 null**（QML 一律先判非空）。
    qInfo().noquote() << QStringLiteral("[ui] 初始化：只有一个查看界面；打开过的来源不写盘");
}

QObject *UiModel::viewer() const { return m_src.vm; }

UiModel::~UiModel()
{
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
    if (m_sessionProbe) m_sessionProbe->start();
    if (index > 0 && m_src.vm) m_src.vm->jump(index);
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
    const int cursor = m_src.vm->cursor();
    buildViewer();
    if (!m_src.vm) return;
    m_src.vm->jump(qMax(0, cursor));
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
}

// ── 打开对话框（**QFileDialog 只在这里**；入口随右键菜单那一批接上）──────────
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

} // namespace app

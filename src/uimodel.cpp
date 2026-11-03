#include "uimodel.h"
#include "logging.h"

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
#include <QLocale>

#include <clocale>
#include <QTranslator>
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
    //   ★ 翻译要在**任何 QML 加载之前**装好（本类在 `engine.load()` 之前构造）：
    //     否则首帧按源串（英文）渲染，切中文时整屏文字闪一下。
    applyLanguage();
    //   一次性迁移：默认收藏夹目录 2026-11-02 从 `<配置>/收藏` 改成 `<配置>/favorites`
    //   （用户要求默认路径不用中文）。**只搬一次**：旧目录在、新目录不在 ⇒ 整个改名，
    //   这样用户已有的进度文件不会"凭空消失"；搬不动就保持原样（下次启动还会再试）。
    if (m_settings.favoritesDir().isEmpty()) {
        const QString cfg = QFileInfo(m_settings.filePath()).absolutePath();
        const QString legacy = cfg + QStringLiteral("/收藏");
        const QString current = cfg + QStringLiteral("/favorites");
        if (QFileInfo::exists(legacy) && !QFileInfo::exists(current)) {
            if (QDir().rename(legacy, current))
                qCInfo(lcUi).noquote() << QStringLiteral("[ui] favorites dir migrated: %1 -> %2").arg(legacy, current);
            else
                qCWarning(lcUi).noquote() << QStringLiteral("[ui] favorites dir migration failed: %1").arg(legacy);
        }
    }

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
            qCWarning(lcUi).noquote()
                << QStringLiteral("[ui] source unsupported: %1")
                       .arg(seq->unsupportedLogReason().isEmpty() ? QStringLiteral("(see UI message)")
                                                                  : seq->unsupportedLogReason());
            notify(why);
            m_sessionProbe->stop();
            return;
        }
        const QString notice = seq->noticeText();
        if (!notice.isEmpty() && notice != m_sessionNotice) {
            m_sessionNotice = notice;
            //   通知**是界面文案**（可能是中文）⇒ 日志只记"有这条通知"，内容走界面
            qCInfo(lcUi).noquote() << QStringLiteral("[ui] source notice raised (see UI toast)");
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
}

QObject *UiModel::viewer() const { return m_src.vm; }

// ══ 界面语言（i18n）═════════════════════════════════════════════════════════
//   · 只有**一份**翻译：换语言 = 摘掉旧的 + 装新的（`QTranslator` 生命周期归本类）；
//   · 语言代码来自设置（空 = 跟随系统）；实际要装的 .qm 是 `:/i18n/photoset-reader_<代码>.qm`，
//     先试完整代码（`zh_CN`）再试两字母前缀（`zh`）—— 与 vsr-player 同一套回退；
//   · **没有 .qm 就不装**：源串是英文，英文界面本来就不需要翻译文件
//     ⇒ 新增一种语言 = 往 `i18n/` 加一份 .ts，代码**零改动**。
QString UiModel::language() const
{
    return m_settings.language();
}

QString UiModel::effectiveLanguage() const
{
    const QString code = m_settings.language();
    return code.isEmpty() ? QLocale::system().name() : code;
}

void UiModel::setLanguage(const QString &code)
{
    if (m_settings.language() == code) return;
    m_settings.setLanguage(code);
    applyLanguage();
    emit languageChanged();
}

//   ★ libexif 的**值**（`未压缩` / `未闪光` / `自动` …）不是我们翻译的，是它自带的
//     gettext 目录 —— 而 gettext 看的是**进程 locale**（`LC_MESSAGES`），不是 `QTranslator`。
//     Qt 启动时会按 `LANG` 设一次 locale，所以"启动时是什么语言，这些值就永远是什么语言"；
//     用户在设置里换语言时必须**同步改 `LC_MESSAGES`**，否则 OSD 会出现
//     "标签中文、值还是英文"（用户 2026-11-02 实测到）。
static void applyMessageLocale(const QString &code)
{
    if (code.isEmpty()) {          // 跟随系统 ⇒ 回到环境变量
        setlocale(LC_MESSAGES, "");
        return;
    }
    const QByteArray base = code.toUtf8();
    const QList<QByteArray> candidates = { base + ".UTF-8", base,
                                           base + ".utf8", code.toLower().toUtf8() + ".UTF-8" };
    for (const QByteArray &cand : candidates) {
        if (setlocale(LC_MESSAGES, cand.constData()))
            return;
    }
    setlocale(LC_MESSAGES, "C");   // 目标 locale 没装 ⇒ 值退回英文（标签仍按翻译走）
}

void UiModel::applyLanguage()
{
    if (m_translator) {
        QCoreApplication::removeTranslator(m_translator);
        delete m_translator;
        m_translator = nullptr;
    }
    QString code = effectiveLanguage();
    applyMessageLocale(m_settings.language().isEmpty() ? QString() : code);
    for (const QString &cand : {code, code.left(2)}) {
        if (cand.isEmpty()) continue;
        auto *t = new QTranslator(this);
        if (t->load(QStringLiteral(":/i18n/photoset-reader_") + cand)) {
            QCoreApplication::installTranslator(t);
            m_translator = t;
            qCInfo(lcUi).noquote() << QStringLiteral("[ui] language: %1 (translation installed)").arg(cand);
            return;
        }
        delete t;
    }
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] language: %1 (no translation, English source strings)").arg(code);
}

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
    m_path = QDir::cleanPath(path);      // ★ 统一形态（去掉尾随 `/`），见 Sequence::absClean
    m_sessionNotice.clear();                  // 换来源 ⇒ 瞬时提示重新计
    buildViewer();
    refreshScanning();
    refreshFavorited();
    if (m_sessionProbe) m_sessionProbe->start();
    //   `--index N`：扫描是异步的 ⇒ 记下来，等条目够了再跳（见 onSequenceChanged）
    m_pendingJump = index;
    if (m_pendingJump >= 0 && m_src.vm && m_src.seq && m_src.seq->count() > m_pendingJump) {
        m_src.vm->jump(m_pendingJump);
        m_pendingJump = -1;
    }
}

void UiModel::buildViewer()
{
    if (m_path.isEmpty()) return;

    Source s;
    s.seq = new Sequence;          // 新来源 ⇒ 空密码表（"密码随来源"）
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
    //   ★ 加密压缩包（设计 22 §5.3）：解码失败原因是"要密码" ⇒ 弹框（浏览进度文件那条路）
    connect(s.vm, &fetch::ViewerModel::passwordRequired, this,
            [this](int item, int level) { onPasswordRequired(item, level); });
    // ★ 本类与 viewer 之间只剩"**给序列 + 序列变了通知**"：位置 / 预取 / 上屏 / loading
    //   全归 viewer 自己。
    const Source old = m_src;
    m_src = s;
    emit viewerChanged();            // ★ 先让 QML 换绑到新 viewer
    if (m_src.vm) m_src.vm->setSequence(m_src.seq);
    applyViewerDefaults();           // ★ 新建 viewer ⇒ 按全局默认起一份阅读态
    if (old.vm) releaseLater(old);   // ★ 再让旧组延迟释放
}

// ── 序列 / viewer 的通知（**只在 GUI 线程**）─────────────────────────────
// 会话序列：**本类是唯一消费者** —— 发布钩子唤醒（合并成一次）⇒ 一次 drain 到空 ⇒ 通知
void UiModel::pumpSession()
{
    Sequence *seq = m_src.seq;
    if (!seq) return;
    if (seq->drain()) onSequenceChanged();
    checkPasswordNeeded();
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
        nullptr, tr("Open folder"), start);
    qCDebug(lcUi).noquote() << QStringLiteral("[ui] dialog open folder: %1 ms (start %2)")
                             .arg(QDateTime::currentMSecsSinceEpoch() - t0).arg(start);
    if (dir.isEmpty()) return;
    m_lastDialogDir = QFileInfo(dir).absolutePath();
    openPath(dir);
}

void UiModel::chooseOpenArchive()
{
    const QString filter = tr("Archives (*.zip *.cbz *.rar *.cbr *.7z *.cb7 *.tar *.cbt *.gz *.bz2 *.xz *.zst);;All files (*)");
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    const QString start = dialogStartDir();
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Open archive"), start, filter);
    qCDebug(lcUi).noquote() << QStringLiteral("[ui] dialog open archive: %1 ms (start %2)")
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
    const QString filter = tr("Images (%1);;All files (*)").arg(pats.join(QLatin1Char(' ')));
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    const QString start = dialogStartDir();
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Open image"), start, filter);
    qCDebug(lcUi).noquote() << QStringLiteral("[ui] dialog open image: %1 ms (start %2)")
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
    qCDebug(lcUi).noquote() << QStringLiteral("[ui] toast: %1").arg(text);
    emit notificationChanged();
}

// ── 幻灯片间隔（值全在 `m_settings`；本类只发通知）─────────────────────────
int UiModel::slideshowIntervalMs() const { return m_settings.intervalMs(); }

void UiModel::setSlideshowIntervalMs(int ms)
{
    const int v = qMax(app::kMinSlideshowIntervalMs, ms);   // 下限的**唯一来源**（appsettings.h）
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
    //   ★ 扫描参数**只在建序列时用一次** ⇒ 下一次打开来源 / 手动「重新扫描」时生效，
    //     改它不重建（用户 2026-11-03：设置项不该触发 rebuildViewer）。
}

int UiModel::recurseDepth() const { return m_settings.recurseDepth(); }

void UiModel::setRecurseDepth(int v)
{
    v = qBound(1, v, 64);
    if (m_settings.recurseDepth() == v) return;
    m_settings.setRecurseDepth(v);
    emit recurseDepthChanged();
    //   ★ 同上：递归深度只在建序列时用一次 ⇒ 下一次打开来源 / 手动「重新扫描」时生效。
}

int UiModel::prefetchWindow() const { return m_settings.prefetchWindow(); }

void UiModel::setPrefetchWindow(int v)
{
    v = qBound(3, v, 64);
    if (m_settings.prefetchWindow() == v) return;
    m_settings.setPrefetchWindow(v);
    emit prefetchWindowChanged();
    //   ★ 设置值是全局的、由本类持有；viewer 只**用即时值** ⇒ 直接交给它，
    //     不重建（重建 = 重扫来源，既慢又会把密码表带丢，用户 2026-11-03）。
    if (m_src.vm) m_src.vm->setPrefetchWindow(v);
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

// ══ 加密压缩包（2026-11-03）：密码表住在 `Sequence` 里（**密码随来源**）══════════
//   · key = 容器**绝对路径**（外层 = 源路径；嵌套 = `源!/成员链`）；
//   · 本类只留**弹框瞬态**：正在问哪个 key / 错误文案 / 可见性；
//   · "本次会话新输入、尚未保存"由序列的 `m_enteredUnsaved` 管，保存进度时问一次。
bool UiModel::hasUnsavedPasswords() const
{
    return m_src.seq && m_src.seq->hasUnsavedPasswords();
}

//   弹框里显示"哪个文件要密码"：**相对来源的路径**（用户 2026-11-03，便于定位具体文件）。
//     · 文件夹来源 ⇒ 相对路径（`vol12/a.zip`）；
//     · 压缩包来源 ⇒ 归档内的成员链（`inner.zip` / `a.zip!/b.zip`）；
//     · 要密码的就是来源本身 ⇒ 只给文件名。
//   界面按宽度**中间省略**（头尾信息量最大）。
QString UiModel::passwordPromptLabel(const QString &key) const
{
    const QString root = QDir::cleanPath(QFileInfo(m_path).absoluteFilePath());
    if (root.isEmpty() || key.isEmpty()) return key;
    if (key == root) return QFileInfo(m_path).fileName();
    if (key.startsWith(root + QLatin1String("!/")))
        return key.mid(root.size() + 2);
    if (key.startsWith(root + QLatin1Char('/')))
        return key.mid(root.size() + 1);
    return key;
}

//   这次扫描卡在密码上 ⇒ 让 QML 弹框（同一个包已经在问的话不重复发通知）
void UiModel::checkPasswordNeeded()
{
    //   ★ 谁卡住就问谁：会话序列（打开来源 / 解码）或后台「重新扫描」那条。
    Sequence *owner = nullptr;
    QString key;
    if (m_src.seq) {
        key = m_src.seq->passwordNeeded();
        if (!key.isEmpty()) owner = m_src.seq;
    }
    if (!owner && m_rescanSeq) {
        key = m_rescanSeq->passwordNeeded();
        if (!key.isEmpty()) owner = m_rescanSeq;
    }
    if (!owner || key == m_pendingPasswordKey) return;

    m_pendingPasswordItem = -1;          // 扫描期
    m_pendingPasswordKey = key;
    m_passwordOwner = owner;
    m_passwordPromptCanSave = false;      // 扫描期还没有关联进度文件 ⇒ 没地方存
    m_passwordPromptSaveDefault = false;
    const QString shown = passwordPromptLabel(key);
    m_passwordPromptPath = shown;
    //   试过才说"密码不正确"（第一次问的时候不能吓人）
    //   ★ 判据只有一条：**这个容器表里已经有密码、却还被追问** ⇒ 那个密码是错的。
    //     第一次问（表里没密码 / 只剩 sticky 候选）不吓人。
    m_passwordPromptError = owner->passwordToTry(key).isEmpty() ? QString() : tr("Wrong password");
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] password required: %1").arg(shown);
    emit passwordPromptChanged();
}

void UiModel::submitPassword(const QString &pw, bool reuseForSubsequent, bool saveToFile)
{
    const QString key = m_pendingPasswordKey;
    if (key.isEmpty() || pw.isEmpty()) return;
    Sequence *owner = m_passwordOwner;
    m_passwordOwner = nullptr;
    //   ★ 后台「重新扫描」那条序列问的 ⇒ 写回它、重启它（不碰当前会话序列）
    if (m_rescanSeq && owner != m_rescanSeq && m_rescanSeq->passwordNeeded() == key) {
        //   取消的是"会话那条"问的框，但后台重扫也在等同一个包 ⇒ 一并放弃重扫
        delete m_rescanSeq;
        m_rescanSeq = nullptr;
        refreshScanning();
    }
    if (owner && owner == m_rescanSeq) {
        owner->notePasswordEntered(key, pw);
        if (reuseForSubsequent) owner->setPasswordDefault(pw);
        m_pendingPasswordItem = -1;
        m_pendingPasswordKey.clear();
        m_passwordPromptPath.clear();
        m_passwordPromptError.clear();
        emit passwordPromptChanged();
        owner->startScan(m_path, m_settings.recurseDepth(), m_settings.scanHidden(),
                         Sequence::ScanOptions{ qint64(m_settings.innerArchiveMaxMb()) });
        qCInfo(lcUi).noquote() << QStringLiteral("[ui] password submitted (rescan restarted)");
        return;
    }
    if (m_src.seq) {
        //   ★ 密码表住在序列里（随来源）；解码期不重扫，`syncItemPasswords()` 从这里回填。
        m_src.seq->notePasswordEntered(key, pw);
        if (reuseForSubsequent) m_src.seq->setPasswordDefault(pw);
    }
    //   ★ 同一个来源的两条序列（会话 + 后台重扫）同时等同一个容器的密码 ⇒ 共享这份输入，
    //     否则重扫会一直停在"等密码"。来源没变 ⇒ 密码随来源走，只此一处共享。
    if (m_rescanSeq && m_rescanSeq->passwordNeeded() == key) {
        m_rescanSeq->notePasswordEntered(key, pw);
        if (reuseForSubsequent) m_rescanSeq->setPasswordDefault(pw);
        m_rescanSeq->startScan(m_path, m_settings.recurseDepth(), m_settings.scanHidden(),
                               Sequence::ScanOptions{ qint64(m_settings.innerArchiveMaxMb()) });
    }
    const int item = m_pendingPasswordItem;
    m_pendingPasswordItem = -1;
    m_pendingPasswordKey.clear();
    m_passwordPromptPath.clear();
    m_passwordPromptError.clear();
    m_passwordPromptCanSave = false;
    m_passwordPromptSaveDefault = false;
    emit passwordPromptChanged();
    if (item >= 0 && m_src.seq && m_src.vm) {
        //   ★ 解码期（浏览进度文件）：只重填密码 + **重取这一条**（不重扫、不关来源）
        m_src.seq->syncItemPasswords();
        //   ★ 同一个包可能有多条都在等密码 ⇒ 全部重取（只重取一条会留下空白屏）
        m_src.vm->retryNeedingPassword();
        //   ★ 勾了"保存密码" ⇒ **立刻写进关联的进度文件**（这条路没有别的保存入口）
        if (saveToFile && !m_progressFile.isEmpty()) {
            const bool ok = writeProgress(m_progressFile, /*quiet=*/true, /*savePasswords=*/true);
            qCInfo(lcUi).noquote() << QStringLiteral("[ui] password saved to progress file: %1")
                                      .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"));
        }
        qCInfo(lcUi).noquote() << QStringLiteral("[ui] password submitted (retrying one item)");
        return;
    }
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] password submitted (retrying scan)");
    //   ★ **同一个序列对象**再扫一次（`startScan` 自己先 join + 重置行）；
    //     不新建序列 ⇒ 密码表（在序列里）自然带着，不必拷贝。
    restartScan();
}

//   同一个来源重扫（提交密码后）：复用当前 `Sequence`，只把 viewer 的槽位/坏因复位
void UiModel::restartScan()
{
    if (!m_src.seq) return;
    m_src.seq->startScan(m_path, m_settings.recurseDepth(), m_settings.scanHidden(),
                         Sequence::ScanOptions{ qint64(m_settings.innerArchiveMaxMb()) });
    if (m_src.vm) m_src.vm->setSequence(m_src.seq);      // 槽位 / 坏因 / 位置复位
    m_pendingJump = -1;
    if (m_sessionProbe) m_sessionProbe->start();
    refreshScanning();
}

void UiModel::cancelPassword()
{
    if (m_pendingPasswordKey.isEmpty()) return;
    const QString key = m_pendingPasswordKey;
    const bool decodeTime = m_pendingPasswordItem >= 0;
    Sequence *owner = m_passwordOwner;
    m_passwordOwner = nullptr;
    m_pendingPasswordItem = -1;
    m_pendingPasswordKey.clear();
    m_passwordPromptPath.clear();
    m_passwordPromptError.clear();
    m_passwordPromptCanSave = false;
    m_passwordPromptSaveDefault = false;
    emit passwordPromptChanged();
    if (owner && owner == m_rescanSeq) {
        //   ★ 后台「重新扫描」问的不想答 ⇒ 放弃这次重扫，**当前画面不动**
        owner->declinePassword(key);
        delete m_rescanSeq;
        m_rescanSeq = nullptr;
        refreshScanning();
        qCInfo(lcUi).noquote() << QStringLiteral("[ui] password cancelled; rescan abandoned");
        return;
    }
    if (decodeTime) {
        //   ★ 浏览进度文件时取消 ⇒ 只记"别再问"，**不关来源**（用户还在看图）
        if (m_src.seq) m_src.seq->declinePassword(key);
        qCInfo(lcUi).noquote() << QStringLiteral("[ui] password cancelled (item stays unreadable)");
        return;
    }
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] password cancelled; source closed");
    dropSource();                       // 扫描期取消 ⇒ 中止本次打开（不留残缺序列）
}

//   ★ 解码期检测（设计 22 §5.3）：浏览进度文件时，光标进入某个需要密码的容器 ⇒ 弹框。
void UiModel::onPasswordRequired(int item, int level)
{
    if (item < 0 || !m_src.seq) return;
    //   ★ 延迟一拍：打开进度文件时窗口会先落 0、再跳到链位置；瞬态那屏也可能是"空着等密码"，
    //     立刻弹就会连着弹好几个卷（用户 2026-11-03 实测）。到点复核"还空着"再弹。
    const int token = ++m_passwordAskToken;
    m_passwordAskItem = item;
    m_passwordAskLevel = level;
    QTimer::singleShot(300, this, [this, token] {
        if (token != m_passwordAskToken) return;
        if (!m_src.vm || !m_src.seq) return;
        if (!m_src.vm->imageA().isNull()) return;          // 已经看到图了 ⇒ 不需要密码
        if (!m_src.vm->screenNeedsPassword()) return;
        askPasswordNow(m_passwordAskItem, m_passwordAskLevel);
    });
}

//   真正弹框（延迟复核通过之后）
void UiModel::askPasswordNow(int item, int level)
{
    if (item < 0 || !m_src.seq) return;
    if (item >= m_src.seq->count()) return;
    const Sequence::Item &it = m_src.seq->at(item);
    //   key = 容器绝对路径；`level` = 0 是外层盘上容器，i = 第 i 层嵌套
    const QString key = Sequence::containerKey(it.loc.diskPath,
                                               it.loc.members.mid(0, level));
    if (m_src.seq->isPasswordDeclined(key)) return;  // 本次会话取消过 ⇒ 不再问
    if (key == m_pendingPasswordKey) return;         // 已经在问同一个包

    m_pendingPasswordItem = item;
    m_pendingPasswordKey = key;
    //   ★ 打开进度文件 / 收藏这条路**没有"保存进度"入口**（用户 2026-11-03）⇒ 在密码框里给一个
    //     "保存密码"勾选：能写文件（有关联的进度文件）才给。
    //   ★ 默认值（用户 2026-11-03 改定）：
    //     · 文件里**没存过** ⇒ **不勾**（不替用户做主，他想存自己勾）；
    //     · 文件里**已经存过**（密码错了才又弹）⇒ **勾上**（沿用他原先"要保存"的选择）。
    m_passwordPromptCanSave = !m_progressFile.isEmpty();
    m_passwordPromptSaveDefault = m_passwordPromptCanSave && m_src.seq->isPasswordSaved(key);
    const QString shown = passwordPromptLabel(key);
    m_passwordPromptPath = shown;
    //   ★ 判据只有一条：**这个容器表里已经有密码、却还被追问** ⇒ 那个密码是错的。
    //     第一次问（表里没密码 / 只剩 sticky 候选）不吓人。
    m_passwordPromptError = m_src.seq->passwordToTry(key).isEmpty() ? QString()
                                                                    : tr("Wrong password");
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] password required (while browsing): %1").arg(shown);
    emit passwordPromptChanged();
}

// ══ 收藏夹 / 进度文件（设计 20）═════════════════════════════════════════════
QString UiModel::favoritesDir() const
{
    QString dir = m_settings.favoritesDir();
    if (dir.isEmpty())
        dir = QFileInfo(m_settings.filePath()).absolutePath() + QStringLiteral("/favorites");
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
        nullptr, tr("Choose favorites folder"), favoritesDir());
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
        msg << QStringLiteral("photoset-reader") << tr("Slideshow playing");
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
            notify(tr("Scan finished: %1 files").arg(m_src.seq ? m_src.seq->count() : 0));
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
        notify(tr("Scanning; try again when it finishes"));
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
    //   ★ 重扫 = 同一个来源的另一次扫描 ⇒ 来源已知的密码跟着表走（不再重复问）
    m_rescanSeq->copyPasswordStateFrom(*m_src.seq);
    m_rescanSeq->setPublishHook([this] {
        if (m_rescanQueued.exchange(true)) return;
        QMetaObject::invokeMethod(this, [this] {
            m_rescanQueued.store(false);
            onRescanChanged();
        }, Qt::QueuedConnection);
    });
    m_rescanSeq->startScan(m_path, m_settings.recurseDepth(), m_settings.scanHidden(),
                           Sequence::ScanOptions{ qint64(m_settings.innerArchiveMaxMb()) });
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] rescan started: %1").arg(m_path);
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
    if (!m_rescanSeq->passwordNeeded().isEmpty()) {
        //   ★ 重扫卡在密码上 ⇒ **先别替换**（否则等于把这次重扫当"没扫到"），
        //     保留这条序列，等用户输密码后重启它（设计 22 §5.1）。
        checkPasswordNeeded();
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
        notify(why.isEmpty() ? tr("Rescan found nothing; keeping the previous result")
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
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] rescan finished, sequence replaced: %1 items, cursor %2")
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
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] rescan cancelled");
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
bool UiModel::writeProgress(const QString &filePath, bool quiet, bool savePasswords)
{
    if (!m_src.seq || !m_src.vm) {
        if (!quiet) notify(tr("No source is open"));
        return false;
    }
    if (!m_src.seq->scanFinished()) {
        //   只有**扫描完**的才能收藏（用户 2026-10-31）—— 点的时候会先弹框告知，这里是兜底
        if (!quiet) notify(tr("Scan not finished; wait for it before saving"));
        return false;
    }
    const QFileInfo fi(m_path);
    const bool isDir = fi.isDir();
    if (!isDir && !ArchiveSource::isArchive(m_path)) {
        if (!quiet) notify(tr("Only folders and archives can save progress"));
        return false;
    }

    m_src.seq->drain();                 // 收尾那一批可能还没取（保证 rows 完整）

    const int cursor = m_src.vm->cursor();

    progressfile::Data d;
    d.meta.savedAt = QDateTime::currentSecsSinceEpoch();
    d.meta.title = progressfile::titleOf(m_path);
    d.meta.sourcePath = QDir::cleanPath(fi.absoluteFilePath());   // ★ 统一形态（见 Sequence::absClean）
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
    //   ★ 加密压缩包（2026-11-03）：密码**按容器条目**写进行里（明文，用户已确认）。
    //     · 已经保存过的（`Sequence::isPasswordSaved`）⇒ 每次都写（自动保存不能把密码弄丢）；
    //     · 本次会话新输入的 ⇒ **只有用户同意**（`savePasswords`）才写。
    //   ★ 源本身是加密容器（根压缩包没有行）⇒ 密码写在元数据段
    //   ★ 密码只有**两个载体**（用户 2026-11-03 定的更简形态）：
    //     ① **来源默认密码**（= 弹框里「后续压缩包使用相同密码」）⇒ 元数据段一行，管所有容器；
    //     ② **按容器的例外**（这个包用了别的密码）⇒ 那一行自己的属性。
    //     ⇒ 不需要"把同一个密码复制到每个容器行"：那样会越存越长，还会给没加密的包塞密码。
    {
        const QString rootKey = m_src.seq->rootIsArchive()
                                    ? QDir::cleanPath(QFileInfo(m_path).absoluteFilePath()) : QString();
        const bool rootKnown = !rootKey.isEmpty() && m_src.seq->hasStoredPassword(rootKey);
        const QString def = m_src.seq->passwordDefault();
        if (rootKnown && (m_src.seq->isPasswordSaved(rootKey) || savePasswords))
            d.meta.sourcePassword = m_src.seq->passwordToTry(rootKey);   // 根容器自己的密码优先
        else if (!def.isEmpty() && (savePasswords || m_src.seq->isPasswordDefaultSaved()))
            d.meta.sourcePassword = def;                                  // 来源默认密码
    }
    for (const auto &row : m_src.seq->containerRows()) {   // 只写"例外"
        const QString &key = row.second;
        if (!m_src.seq->hasStoredPassword(key)) continue;
        if (!m_src.seq->isPasswordSaved(key) && !savePasswords) continue;
        //   和来源默认密码相同的**不写**（默认已经管它了）⇒ 文件里只留真正的例外
        if (m_src.seq->passwordToTry(key) == m_src.seq->passwordDefault()) continue;
        if (row.first >= 0 && row.first < d.rows.size())
            d.rows[row.first].password = m_src.seq->passwordToTry(key);
    }

    QString err;
    if (!progressfile::write(filePath, d, &err)) {
        if (!quiet) notify(err.isEmpty() ? tr("Failed to save progress") : err);
        else qCWarning(lcUi).noquote()
                 << QStringLiteral("[ui] autosave failed: %1 (details in the UI message)").arg(filePath);
        return false;
    }
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] progress saved: %1 (%2 rows)")
                             .arg(filePath).arg(d.rows.size());
    snapshotSavedState(d.meta, d.rows.size());
    if (savePasswords && m_src.seq)            // 用户同意保存 ⇒ 之后不再问
        m_src.seq->markPasswordsSaved();
    return true;
}

bool UiModel::saveProgressToFavorites(bool savePasswords)
{
    const QString file = currentFavoritesFile();
    if (file.isEmpty()) {
        notify(m_path.isEmpty() ? tr("No source is open")
                                : tr("Only folders and archives can save progress"));
        return false;
    }
    if (!writeProgress(file, false, savePasswords))
        return false;
    refreshFavorited();
    if (m_favorites) m_favorites->reload();      // 列表里能立刻看到
    startTrackingProgress(file);                 // 之后按间隔 / 切来源自动保存
    notify(tr("Added to favorites"));
    return true;
}

bool UiModel::saveProgressToFile(bool savePasswords)
{
    if (!m_src.seq || !m_src.vm) {
        notify(tr("No source is open"));
        return false;
    }
    const QString start = m_lastProgressDir.isEmpty()
                              ? QDir::homePath()
                              : m_lastProgressDir + QLatin1Char('/')
                                    + progressfile::titleOf(m_path) + QLatin1String(".prsave");
    const QString picked = QFileDialog::getSaveFileName(
        nullptr, tr("Save progress to file"), start,
        tr("Progress files (*.prsave)"));
    if (picked.isEmpty())
        return false;
    QString file = picked;
    if (!file.endsWith(QLatin1String(".prsave"), Qt::CaseInsensitive))
        file += QLatin1String(".prsave");
    if (!writeProgress(file, false, savePasswords))
        return false;
    m_lastProgressDir = QFileInfo(file).absolutePath();
    startTrackingProgress(file);
    notify(tr("Progress saved"));
    return true;
}

void UiModel::openProgressFile()
{
    const QString picked = QFileDialog::getOpenFileName(
        nullptr, tr("Open progress file"),
        m_lastProgressDir.isEmpty() ? QDir::homePath() : m_lastProgressDir,
        tr("Progress files (*.prsave)"));
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
        notify(err.isEmpty() ? tr("Cannot open progress file") : err);
        return false;
    }
    if (!QFileInfo::exists(d.meta.sourcePath)) {
        notify(tr("Source is gone: %1").arg(d.meta.sourcePath));
        return false;
    }

    dropSource();

    Source s;
    s.seq = new Sequence;
    //   ★ 恢复 = 行 → 序列（**同一份 `Sequence::Row`**，与扫描共用一套表示）
    s.seq->restore(d.meta.sourcePath, d.rows, d.meta.sourceKind == Sequence::Kind::Archive);
    //   ★ 加密压缩包（2026-11-03）：进度文件里存了密码的容器行 ⇒ 灌进序列（条目已建好，
    //     `applyRowPasswords()` 会把每个条目的 `loc.passwords` 一并补齐）⇒ 浏览时不再弹框。
    const QString srcKey = QDir::cleanPath(QFileInfo(d.meta.sourcePath).absoluteFilePath());
    if (!d.meta.sourcePassword.isEmpty()) {
        //   ★ 文件里那个密码 = **这个来源的默认密码**（所有容器先拿它试），不是"只给根容器"。
        //     根容器把它同时记到自己名下（根没有行，靠元数据段，写回时原样带着）。
        s.seq->setPasswordDefault(d.meta.sourcePassword);
        s.seq->setSavedPasswordDefault(d.meta.sourcePassword);
        if (d.meta.sourceKind == Sequence::Kind::Archive)
            s.seq->setPassword(srcKey, d.meta.sourcePassword);
    }
    s.seq->applyRowPasswords();
    s.seq->markPasswordsSaved();      // 文件里已有 ⇒ 不再问"是否保存"
    s.seq->syncItemPasswords();
    s.vm = new fetch::ViewerModel(m_settings.prefetchWindow(), nullptr);
    m_path = QDir::cleanPath(d.meta.sourcePath);   // ★ 统一形态
    installViewer(s);
    if (m_src.vm) {
        m_src.vm->setDoublePage(d.meta.layout == 1);        // ★ 先还原版面，再按 chain/page 定位
        const int idx = m_src.seq->indexOfChain(d.meta.chain, d.meta.page);
        m_src.vm->jump(qMax(0, idx));
    }
    m_pendingJump = -1;
    startTrackingProgress(filePath);                   // 之后按间隔 / 切来源自动保存
    snapshotSavedState(d.meta, d.rows.size());         // 文件里记着的就是这份
    if (m_sessionProbe) m_sessionProbe->stop();        // 恢复的序列已经收尾，不需要探测
    m_sessionNotice.clear();
    refreshScanning();
    refreshFavorited();
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] progress restored: %1 (%2 rows, entry %3)")
                             .arg(filePath).arg(d.rows.size()).arg(d.meta.done);
    return true;
}

//   收藏夹列表里的"移除"：删文件（不动源）⇒ 刷新"已收藏" + 列表
bool UiModel::removeProgressFile(const QString &filePath)
{
    if (filePath.isEmpty() || !QFileInfo::exists(filePath))
        return false;
    if (!QFile::remove(filePath)) {
        notify(tr("Cannot delete: %1").arg(filePath));
        return false;
    }
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] progress file removed: %1").arg(filePath);
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
        notify(tr("Cannot delete: %1").arg(file));
        return false;
    }
    refreshFavorited();
    if (m_favorites) m_favorites->reload();
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] favorite removed, progress file deleted: %1").arg(file);
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
        qCWarning(lcUi).noquote() << QStringLiteral("[ui] autosave stopped, progress file is gone: %1")
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
    qCDebug(lcUi).noquote() << QStringLiteral("[ui] autosave: %1").arg(m_progressFile);
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
    qCInfo(lcUi).noquote() << QStringLiteral("[ui] scan stop requested");
}

} // namespace app

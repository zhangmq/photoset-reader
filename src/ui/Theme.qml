pragma Singleton
import QtQuick

// ══ 设计令牌（单例）══════════════════════════════════════════════
// 结构照 DeepSeek Harness 的设计系统：**两层令牌**
//   ① static  —— 原始色阶（neutral-bluish / deepseek / amber / green / red）
//   ② alias   —— 语义别名（bg / border / text / brand / …），**有明暗两套**
// 界面只允许用 alias；换主题 = 换 mode，不改任何界面代码。
QtObject {
    id: t

    // ── 主题模式（三态，由 ViewModel 决定）─────────────────
    //   ui.themeMode: 0=跟随系统 1=浅色 2=深色
    readonly property bool isDark: ui.themeMode === 2
                                 || (ui.themeMode === 0 && ui.systemDark)


    // ── ① static 原始色阶（DSH 实测值）─────────────────────
    readonly property QtObject _s: QtObject {
        // neutral-bluish（主力中性色阶）
        readonly property color nb00:   "#ffffff"
        readonly property color nb50:   "#f9fafb"
        readonly property color nb60:   "#f5f6f7"
        readonly property color nb75:   "#f1f3f5"
        readonly property color nb100:  "#ebeef2"
        readonly property color nb150:  "#e9ecf2"
        readonly property color nb200:  "#e1e5ee"
        readonly property color nb300:  "#cfd3d6"
        readonly property color nb400:  "#adb2b8"
        readonly property color nb500:  "#979da6"
        readonly property color nb600:  "#81858c"
        readonly property color nb700:  "#61666b"
        readonly property color nb750:  "#43454a"
        readonly property color nb800:  "#353638"
        readonly property color nb850:  "#2c2c2e"
        readonly property color nb875:  "#232324"
        readonly property color nb900:  "#1b1b1c"
        readonly property color nb950:  "#151517"
        readonly property color nb1000: "#0f1115"
        // 品牌
        readonly property color ds400:  "#7aaaff"
        readonly property color ds450:  "#5686fe"
        readonly property color ds500:  "#4176e6"
        readonly property color ds800:  "#34415b"
        // 语义色
        readonly property color amber500: "#f59e0b"
        readonly property color amber600: "#dd8629"
        readonly property color amber900: "#27241f"
        readonly property color green400: "#4ed17e"
        readonly property color green500: "#22c55e"
        readonly property color green900: "#233c2c"
        readonly property color red400:   "#f25a5a"
        readonly property color red600:   "#ec1313"
    }

    // ── ② alias 语义别名 ────────────────────────────────────
    //   **逐条对照 DSH**（dsh-client-ui-theme/lib/client.js 的 design-platform.css）
    //   左列 = 本项目的 alias，右列 = DSH 的 token
    //
    //   ★ 注意 DSH 的两套概念：
    //     · `bg-layer-1/2/3`  —— 给**层叠浮层**用（浅色下三层全是纯白）
    //     · surface 类（bg-module-platform / specific-*） —— 给**面板/卡片**用
    //   卡片属于 surface 类，所以取 `bg-module-platform`，不是 layer。

    // 页面底            ← DSH: --dsw-alias-bg-base
    readonly property color bgBase:    isDark ? _s.nb950 : _s.nb00

    // 卡片 / 面板        ← DSH: --dsw-alias-bg-module-platform (== specific-selector == bg-multi-select)
    readonly property color bgCard:    isDark ? _s.nb800 : _s.nb60

    // 卡片悬停          ← DSH: --dsw-alias-interactive-bg-hover-solid

    // 激活 / 强调底      ← DSH: --dsw-alias-button-ghost-active-fill
    readonly property color bgActive:  isDark ? _s.nb750 : _s.nb100

    // 侧栏              ← DSH: --dsw-specific-sidebar-fill

    // 侧栏项悬停 / 激活  ← DSH: --dsw-specific-sidebar-nav-item-hover / -active

    // 徽标（普通）      ← DSH: .zGbnIq_rowTag
    //   底 = markdown-tag；边框 = border-l3；文字 = label-secondary
    readonly property color bgTag:     isDark ? _s.nb850 : _s.nb75
    // 徽标（错误/缺失）  ← DSH: .rtSEdW_brokenBadge
    //   底/边框/文字都取 state-error-primary（DSH 是实底无边框，这里按用户要求保留同色边框）
    readonly property color tagErrBg:   isDark ? "#1ff25a5a" : "#1fec1313"
    readonly property color tagErrLine: isDark ? _s.red400   : _s.red600
    readonly property color tagErrText: isDark ? _s.red400   : _s.red600

    // 输入框            ← DSH: --dsw-specific-input-major
    //   ★ 浅色下 DSH 的输入框坐在**卡片**(nb-60)上，用 nb-00 能区分；
    //     但本项目的输入框坐在**纯白页面**上，nb-00 会完全看不见 →
    //     浅色改用 nb-60（与卡片同色，靠 borderL2 描边区分）

    // 浮层 / 提示        ← DSH: --dsw-alias-bg-overlay / --dsw-alias-tooltip-bg

    // 提示气泡底 / 文字   ← DSH: --dsw-alias-tooltip-bg / --dsw-static-neutral-bluish-00
    //   ★ 深浅两色都是"深底白字"（浅色主题的底反而更深：nb850）
    readonly property color tooltipBg:   isDark ? _s.nb750 : _s.nb850
    readonly property color tooltipText: _s.nb00

    // ── 全屏浮层：**跟随主题，配色一律用 DSH alias 令牌**（用户 2026-10-11 定案 (A)）──
    //   画布/栏底 = `bgBase`；文字 = `textPrimary`；图标 = `textSecondary`；hover 底 = `hoverFill`；
    //   禁用 = `dimOpacity`。**只保留三个 alias 层没有对应物的**：
    //     · `overlayTrack` / `overlayCursor`：时间轴轨道与游标（照抄 vsr-player `ProgressSlider`）
    //     · `overlayTextOutline`：OSD 无背景文字的外描边
    readonly property color overlayTrack:       isDark ? "#44ffffff" : "#33000000"
    readonly property color overlayCursor:      isDark ? "#e0e0e0" : "#404040"
    readonly property color overlayTextOutline: isDark ? "#cc000000" : "#ccffffff"

    // 图片/骨架占位底     ← DSH: --dsw-alias-bg-skeleton
    readonly property color bgSkeleton: isDark ? "#14ffffff" : "#0a000000"

    // 层叠表面 ← DSH: --dsw-alias-bg-layer-1 / -2
    readonly property color bgLayer1: isDark ? _s.nb875 : _s.nb00   // 抬起面（分段控件指示块）
    readonly property color bgLayer2: isDark ? _s.nb850 : _s.nb00   // 对话框表面
    // 遮罩            ← DSH: --dsw-alias-bg-mask-1
    readonly property color mask:     isDark ? "#80000000" : "#3d000000"   // QML 是 AARRGGBB

    // 卡片底 / hover   ← DSH 产出物·文件变更卡 `ChangedFiles.module.css`
    //   `.header{background:var(--changes-fill)}`，深色主题重绑：
    //     浅 --dsw-static-neutral-50 / -100；深 -850 / -800
    readonly property color cardFill:      isDark ? "#212123" : "#fafafa"
    readonly property color cardFillHover: isDark ? "#292929" : "#f5f5f5"

    // 按钮底（DSH「新会话」按钮）← DSH: --dsw-alias-button-elevated-fill
    readonly property color btnFill:      isDark ? _s.nb750 : _s.nb00
    // 按钮 hover 底            ← DSH: --dsw-alias-button-floating-hover
    readonly property color btnFillHover: isDark ? _s.nb800 : _s.nb75

    // 菜单 / 浮层表面    ← DSH: --dsw-specific-menu == --dsw-menu-surface-fill
    //   ★ DSH 原值：浅 #f8f9fa94（58%）/ 深 #43454a73（45%），再叠
    //     backdrop-filter: blur(40px) saturate(150%) —— 真实观感是**实心毛玻璃**。
    //     QML 没有背景模糊 API；只留 alpha 会"透出后面的内容"，与 DSH 不符。
    //     故把 alpha 与页面底色（浅 nb00 / 深 nb950）预先合成，取 DSH 的**等效不透明色**：
    //       浅 0.58×#f8f9fa + 0.42×#ffffff = #fbfcfc
    //       深 0.45×#43454a + 0.55×#151517 = #2a2b2e
    readonly property color menuFill:  isDark ? "#2a2b2e" : "#fbfcfc"
    // 行/图标按钮 hover 底 ← DSH: --dsw-alias-interactive-bg-hover
    //   （半透明：叠在菜单表面、或叠在页面底上，都是它；与实底的 bgHover 是两个 token）
    readonly property color hoverFill: isDark ? "#14ffffff" : "#0f263148"
    // 选中底（比 hover 强一档）← DSH: --dsw-alias-interactive-bg-active
    readonly property color activeFill: isDark ? "#24ffffff" : "#1a263148"
    // 菜单图标           ← DSH: --dsw-alias-menu-icon
    readonly property color menuIcon:  isDark ? _s.nb100 : _s.nb800
    // 菜单分组标题底     ← DSH: --dsw-alias-menu-group-header-fill

    // 描边              ← DSH: --dsw-alias-border-l1 / -l2 / -l3
    readonly property color borderL1:  isDark ? "#0fffffff" : "#0a000000"
    readonly property color borderL2:  isDark ? "#1fffffff" : "#1a000000"
    readonly property color borderL3:  isDark ? "#29ffffff" : "#1f000000"
    // 浮层描边（elevation stroke 用色）← DSH: --dsw-alias-border-l4
    readonly property color borderL4:  isDark ? "#33ffffff" : "#29000000"

    // 文字              ← DSH: --dsw-alias-label-primary / -secondary / -tertiary / -caption
    // ★ 主文本色（text-primary）—— **不是**"主色"。主色是下面的 `brand`，两者颜色不同
    readonly property color textPrimary:   isDark ? _s.nb50  : _s.nb1000
    readonly property color textSecondary: isDark ? _s.nb300 : _s.nb700
    readonly property color textTertiary:  isDark ? _s.nb400 : _s.nb600
    readonly property color textCaption:   isDark ? _s.nb600 : _s.nb400

    // 链接 / 品牌        ← DSH: --dsw-alias-link (dark: deepseek-400 / light: deepseek-500)
    // ★ 主色（primary）≡ brand —— 用户明确：说 "primary" 就是它（DSH `--dsw-alias-link` 同值）
    readonly property color brand:      isDark ? _s.ds400 : _s.ds500

    // 语义              ← DSH: --dsw-alias-state-error/warn/success-primary
    readonly property color danger:     isDark ? _s.red400   : _s.red600
    // 开关滑块 ← DSH: --dsw-alias-switch-thumb
    readonly property color switchThumb: isDark ? _s.nb400 : _s.nb00
    // 品牌底上的前景（开关 on 的滑块等）← DSH: --dsw-alias-label-primary-foreground
    readonly property color brandFg:     isDark ? _s.nb1000 : _s.nb00
    readonly property color dangerSoft: isDark ? "#1ff25a5a" : "#1fec1313"
    readonly property color warn:       isDark ? _s.amber500 : _s.amber500

    // ── ③ 尺寸 / 间距 / 字体（语义化 token）──────────────────
    //   规则：界面里**不允许出现魔法数字**，一律引用这里的 token。

    // 间距（spacing）
    readonly property int space2xs:  2
    readonly property int spaceXs:   4
    readonly property int spaceSm:   8
    readonly property int spaceMd:   12
    readonly property int spaceLg:   20
    readonly property int spaceXl:   32

    // 圆角（radius）—— **对齐 DSH 的 --dsw-radius-* 刻度**
    readonly property int radiusXs:    4      // DSH: --dsw-radius-xs
    readonly property int radiusSm:    8      // DSH: --dsw-radius-sm
    readonly property int radiusMd:   12      // DSH: --dsw-radius-md
    readonly property int radiusLg:   16      // DSH: --dsw-radius-lg
    readonly property int radiusPanel:28      // DSH: --dsw-radius-panel
    readonly property int radiusPill: 999     // 胶囊（DSH 的 brokenBadge 用 999px）

    // 控件高度（control）
    readonly property int controlSm: 28        // 输入框 / 小按钮
    readonly property int controlMd: 36        // 图标按钮 / 标准按钮 / 菜单行

    // 图标尺寸（图标是图形元素，允许独立尺寸）
    // 焦点环宽度 ← DSH: --dsw-focus-ring-width（2px）
    //   颜色用 brand：DSH 的 --dsw-focus-ring-color 缺省回退到 --dsw-alias-state-business-primary
    //   （浅 deepseek-500 / 深 deepseek-400），与本项目 brand 完全同值
    readonly property int focusRingW: 2

    readonly property int iconSm:    14
    readonly property int iconMd:    18
    readonly property int iconLg:    40

    // 字号（相对系统默认；全局 +1pt 由 C++ 设一次）
    //   fontBase = 系统默认（不设，继承全局）
    //   fontSm   = 小一号（徽标等次要信息）
    readonly property int fontSm: Qt.application.font.pointSize - 1

    // 布局宽度
    readonly property int cardMaxW:     980    // 卡片最大宽度

    // 组件尺寸
    readonly property int coverSize:   64      // 卡片封面（正方形）
    readonly property int thumbCellW: 168      // 缩略图格子宽
    readonly property int thumbCellH: 190      // 缩略图格子高
    // 横向操作区统一高度（动作条 / 列表标题行）—— 试验中：若决定不统一，再拆回两个 token
    readonly property int barH:        48
    readonly property int menuMinW:   144      // 菜单最小宽度
    readonly property int dialogW:    380      // 对话框宽度 ← DSH Modal.module.css .dialog{width:min(380px,100%)}
    readonly property int progressH:    4      // 进度条高度
    readonly property int badgePadH:   Theme.spaceMd   // 徽标左右内边距（合计）← DSH rowTag: padding 1px 6px
    readonly property int hairline:     1      // 分割线
    readonly property int scrollBarW:   6      // 滚动条宽度（hover 才显，拖动时保持）
    readonly property int borderW:      1      // 描边宽
    readonly property int borderSelW:   2      // 选中态描边宽（卡片；**不是**焦点环）
    readonly property int cardPadH:    Theme.spaceMd   // 卡片左右内边距
    readonly property int cardPadV:    Theme.spaceMd   // 卡片上下内边距
    readonly property int filterW:    240      // 过滤框宽度
    readonly property int thumbSm:     28      // 列表行缩略图
    // 列表视图（表格）列几何 —— **唯一定义处**，表头与行都读这里（§1.2 算法单一事实源）
    readonly property int colListPad:   Theme.spaceMd   // 列表左右外边距
    readonly property int colRowPadL:   Theme.spaceSm   // 行内左边距
    readonly property int colRowPadR:   Theme.spaceMd   // 行内右边距
    readonly property int colNameOffset: colRowPadL + Theme.thumbSm + Theme.spaceSm   // 名称列（行内偏移）
    readonly property int colSizeW:     90      // 「文件大小」列宽
    readonly property int colResW:     110      // 「分辨率」列宽
    readonly property int colGap:       Theme.spaceLg   // 两个数值列之间
    readonly property int markSm:      18      // 小角标
    // 全屏浮层（照抄 vsr-player 的 OSD 几何）
    readonly property int osdHotZoneH: 40      // 顶/底浮层热区高
    readonly property int osdTrackH:   14      // 时间轴行高
    readonly property int osdCursorW:   2      // 时间轴游标宽
    // 开关几何 ← DSH Switch.module.css：36×20 轨道 + 16 圆形滑块 + 2 padding
    readonly property int switchW:      36
    readonly property int switchH:      20
    readonly property int switchThumbD: 16
    // 设置对话框
    readonly property int settingsCtlW: 280      // 设置项右侧控件列预留宽（断开"标签宽↔控件宽"循环绑定）
    readonly property int settingsKeyW: 200      // 快捷键表键列宽
    readonly property int settingsScopeW: 110    // 快捷键表"适用范围"列宽
    readonly property int numberW:      120      // 数字输入框宽
    readonly property int settingsW:    800      // 设置框宽 ← DSH settings-general `.panel{width:800px}`
    readonly property int settingsH:    800      // 设置框高上限 ← 同处 `height:min(800px, 100vh - 48px)`
    readonly property int settingsNavW: 188      // 设置框左导航列宽 ← DSH `.nav{width:188px}`
    readonly property int osdKeyW:      64      // OSD 面板键列宽（最长键 = 4 字：色彩空间/修改时间）
    readonly property int osdPathMaxW: 480      // 顶栏相对路径最大宽（超出中间省略）
    readonly property int osdValueMaxW: 420     // OSD 值列最大宽（EXIF 长值省略）
    readonly property int osdKeyMaxW:   180     // OSD 键列上限（未映射的 EXIF 名可能很长）
    readonly property int fastFwdDelayMs:  400   // 长按多久开始连续快进（设计 §3.5）
    readonly property int fastFwdFastMs:    80   // 连续快进的重叠间隔
    readonly property int osdTrackLineH: 4     // 时间轴轨道高（常态）
    readonly property int osdTrackLineHoverH: 6  // 时间轴轨道高（hover/拖动，vsr-player 6→8 同理）
    readonly property int crumbItemH:  24      // 面包屑段高
    readonly property int crumbMaxW:  360      // 面包屑段最大宽度（超出中间省略 + tooltip 全名）
    readonly property int crumbRootMaxW: 240   // 锁死的**根**段上限（它是整条路径，通常最长，单独压一档）
    readonly property int titleGap:     3      // 卡片标题行内间距
    readonly property int badgeGap:     6      // 标题与徽标间距
    readonly property int emptyGap:    10      // 空状态间距
    readonly property int rowGap:       4      // 列表行间距

    // 弱化透明度（缺失 / 不可用态）← DSH: .item:disabled / .button:disabled { opacity: 0.4 }
    readonly property real dimOpacity: 0.4

    // 动效
    readonly property int durFast:    150
    readonly property int durMed:     300      // 浮层淡入淡出（vsr-player 同值）

    // ── 图标码点（MDI；字体族取 context property `iconFont`）──
    // ★ 必须 String.fromCodePoint：MDI 码点在 U+F0000 以上（5 位），JS 的 \uXXXX 只吃 4 位
    readonly property string icFolder:  String.fromCodePoint(0xF0770)
    readonly property string icZip:     String.fromCodePoint(0xF05C4)
    readonly property string icImage:   String.fromCodePoint(0xF0222)
    readonly property string icGrid:    String.fromCodePoint(0xF0570)
    readonly property string icCollections: String.fromCodePoint(0xF0254)  // folder-multiple-image（空态：还没有图集）
    readonly property string icAlbum:   String.fromCodePoint(0xF02EA)   // image-album（卡片封面占位 / 缺失）
    readonly property string icPlus:    String.fromCodePoint(0xF0415)
    readonly property string icAlert:   String.fromCodePoint(0xF0026)
    readonly property string icTrash:   String.fromCodePoint(0xF0A7A)
    readonly property string icScan:    String.fromCodePoint(0xF0969)   // folder-search-outline（扫描 / 重新扫描图集）
    readonly property string icSort:    String.fromCodePoint(0xF04BA)
    readonly property string icSearch:  String.fromCodePoint(0xF0349)   // magnify
    readonly property string icClear:   String.fromCodePoint(0xF0159)   // close-circle
    // 主题三态图标（**定死，不再变**）
    readonly property string icThemeSystem: String.fromCodePoint(0xF0379)   // monitor（跟随系统）
    readonly property string icThemeLight:  String.fromCodePoint(0xF0599)   // weather-sunny
    readonly property string icThemeDark:   String.fromCodePoint(0xF0F67)   // moon-waxing-crescent
    // 设置（候选见 docs/design/06-设置图标候选.md）
    readonly property string icSettings:    String.fromCodePoint(0xF08BB)   // cog-outline
    // 导航 / 全屏（**全部查表得来，不内联猜**）
    readonly property string icBack:      String.fromCodePoint(0xF004D)   // arrow-left
    readonly property string icUp:        String.fromCodePoint(0xF005D)   // arrow-up
    readonly property string icList:      String.fromCodePoint(0xF0572)   // view-list
    readonly property string icFirst:     String.fromCodePoint(0xF0600)   // page-first
    readonly property string icLast:      String.fromCodePoint(0xF0601)   // page-last
    readonly property string icPrevVol:   String.fromCodePoint(0xF013D)   // chevron-double-left
    readonly property string icNextVol:   String.fromCodePoint(0xF013E)   // chevron-double-right
    readonly property string icPrev:      String.fromCodePoint(0xF0141)   // chevron-left
    readonly property string icNext:      String.fromCodePoint(0xF0142)   // chevron-right
    readonly property string icChevronR:  String.fromCodePoint(0xF0142)   // chevron-right（面包屑）
    readonly property string icChevronD:  String.fromCodePoint(0xF0140)   // chevron-down（下拉）
    readonly property string icPlay:      String.fromCodePoint(0xF040A)   // play
    readonly property string icPause:     String.fromCodePoint(0xF03E4)   // pause
    readonly property string icInfo:      String.fromCodePoint(0xF02FD)   // information-outline
    readonly property string icHistory:   String.fromCodePoint(0xF02DA)   // history
    readonly property string icClose:     String.fromCodePoint(0xF0156)   // close
    readonly property string icLock:      String.fromCodePoint(0xF0341)   // lock-outline
    readonly property string icLockOpen:  String.fromCodePoint(0xF0340)   // lock-open-outline（缩放未锁定）
    readonly property string icStar:      String.fromCodePoint(0xF04CE)   // star
    readonly property string icStarO:     String.fromCodePoint(0xF04D2)   // star-outline

    // 排序方向
    readonly property string icAsc:     String.fromCodePoint(0xF005D)   // arrow-up
    readonly property string icDesc:    String.fromCodePoint(0xF0045)   // arrow-down
    readonly property string icCheck:   String.fromCodePoint(0xF012C)   // check
    // ── 全屏浮层用（照搬 vsr-player 的 loop / 文件夹箭头语义）──
    readonly property string icLoop:    String.fromCodePoint(0xF0456)   // repeat（全部循环）
    readonly property string icLoopOff: String.fromCodePoint(0xF0457)   // repeat-off（不循环）
    readonly property string icLoopFolder: String.fromCodePoint(0xF0D0B) // folder-sync（文件夹循环）
    readonly property string icInterval: String.fromCodePoint(0xF051B)  // timer-outline（切换速度）
    readonly property string icFullscreen: String.fromCodePoint(0xF0293) // fullscreen（切全屏窗口）
    readonly property string icFullscreenExit: String.fromCodePoint(0xF0294) // fullscreen-exit
    readonly property string icMinus:   String.fromCodePoint(0xF0374)   // minus（减速）
    readonly property string icFolderPrev: String.fromCodePoint(0xF19EA)  // folder-arrow-left
    readonly property string icFolderNext: String.fromCodePoint(0xF19EE)  // folder-arrow-right
    readonly property string icHome:   String.fromCodePoint(0xF02DC)   // home（回图集库）
    // ── 全屏浮层：画面缩放 / 阅读版面 ──
    // 缩放算法（对图"施加魔法"：图 + 自动调整）—— 用户 2026-10-20 选定
    readonly property string icAlgorithm:  String.fromCodePoint(0xF0FC1)  // image-auto-adjust
    readonly property string icFitActual:  String.fromCodePoint(0xF0C8D)  // image-size-select-actual（原始大小）
    readonly property string icFitScreen:  String.fromCodePoint(0xF18F4)  // fit-to-screen（适应屏幕）
    readonly property string icFitWidth:   String.fromCodePoint(0xF084E)  // arrow-expand-horizontal（适应宽度）
    readonly property string icFitHeight:  String.fromCodePoint(0xF084F)  // arrow-expand-vertical（适应高度）
    readonly property string icSinglePage: String.fromCodePoint(0xF0EB0)  // file-image-outline（单幅，用户选定）
    readonly property string icDoublePage: String.fromCodePoint(0xF14F7)  // book-open-variant（双幅）
}

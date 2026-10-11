import QtQuick

// 占位数据 —— 后续替换为 C++ 的序列模型（roles 保持一致）
//   roles: name / sizeText(文件大小) / resText(分辨率) / kind(image|archive|collection) / missing / thumb
//   文件名由真实目录生成，避免手写出错
ListModel {
    id: model
    Component.onCompleted: {
        const base = "file:///home/zmq/projects/image-browser/.testdata/vol01/"
        const rows = [
            // ── 非图片条目：用于验证**类型 tag（图标+文本）**与卡片类型图标 ──
            { name: "第01卷（全）", sizeText: "—", resText: "—", kind: "collection", missing: false, thumb: base + "[七龙珠完全版]第01卷_010.jpg" },
            { name: "[七龙珠完全版][DRAGON BALL][鸟山明][文传][01卷].rar", sizeText: "412 MB", resText: "—", kind: "archive", missing: false, thumb: base + "[七龙珠完全版]第01卷.jpg" },
            { name: "[七龙珠完全版]第01卷_000.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷.jpg" },
            { name: "[七龙珠完全版]第01卷_000.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_000.jpg" },
            { name: "[七龙珠完全版]第01卷_001.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_001.jpg" },
            { name: "[七龙珠完全版]第01卷_002.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_002.jpg" },
            { name: "[七龙珠完全版]第01卷_003.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_003.jpg" },
            { name: "[七龙珠完全版]第01卷_004.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_004.jpg" },
            { name: "[七龙珠完全版]第01卷_005.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_005.jpg" },
            { name: "[七龙珠完全版]第01卷_006.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_006.jpg" },
            { name: "[七龙珠完全版]第01卷_007.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_007.jpg" },
            { name: "[七龙珠完全版]第01卷_008.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_008.jpg" },
            { name: "[七龙珠完全版]第01卷_009.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_009.jpg" },
            { name: "[七龙珠完全版]第01卷_010.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_010.jpg" },
            { name: "[七龙珠完全版]第01卷_011.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_011.jpg" },
            { name: "[七龙珠完全版]第01卷_012.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_012.jpg" },
            { name: "[七龙珠完全版]第01卷_013.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_013.jpg" },
            { name: "[七龙珠完全版]第01卷_014.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_014.jpg" },
            { name: "[七龙珠完全版]第01卷_015.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_015.jpg" },
            { name: "[七龙珠完全版]第01卷_016.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_016.jpg" },
            { name: "[七龙珠完全版]第01卷_017.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_017.jpg" },
            { name: "[七龙珠完全版]第01卷_018.jpg", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_018.jpg" },
            { name: "[七龙珠完全版]第01卷_019.png", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_019.png" },
            { name: "[七龙珠完全版]第01卷_020.png", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_020.png" },
            { name: "[七龙珠完全版]第01卷_021.png", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_021.png" },
            { name: "[七龙珠完全版]第01卷_022.png", sizeText: "1.2 MB", resText: "3288×2200", kind: "image", missing: false, thumb: base + "[七龙珠完全版]第01卷_022.png" }
        ]
        for (let i = 0; i < rows.length; ++i) append(rows[i])
    }
}

import QtQuick

// 占位数据模型 —— 后续替换为 C++ 图集模型（roles 保持一致）
//   roles: name / path / addedAt / lastViewedAt / progressValue / progressText / missing / cover
ListModel {
    id: model

    ListElement {
        name: "龙珠 第01卷"
        path: "/run/media/zmq/Home/Dragon Ball/[七龙珠完全版][DRAGON BALL][鸟山明][文传][01卷].rar"
        addedAt: "2026-10-01"
        lastViewedAt: "2026-10-10"
        progressValue: 0.52
        progressText: "61 / 117"
        missing: false
        cover: "file:///home/zmq/projects/image-browser/.testdata/vol01/[七龙珠完全版]第01卷_001.jpg"
    }
    ListElement {
        name: "龙珠 第01卷"
        path: "/data/backup/01卷.rar"
        addedAt: "2026-09-20"
        lastViewedAt: "2026-09-01"
        progressValue: 0.10
        progressText: "12 / 117"
        missing: false
        cover: "file:///home/zmq/projects/image-browser/.testdata/vol01/[七龙珠完全版]第01卷_010.jpg"
    }
    ListElement {
        name: "2024-01-01"
        path: "/home/zmq/Pictures/2024-01-01"
        addedAt: "2026-10-09"
        lastViewedAt: "2026-10-09"
        progressValue: 0.04
        progressText: "200 / 5000"
        missing: false
        cover: "file:///home/zmq/projects/image-browser/.testdata/vol01/[七龙珠完全版]第01卷_020.png"
    }
    ListElement {
        name: "某扫描集"
        path: "/mnt/old/scans"
        addedAt: "2026-08-11"
        lastViewedAt: "2026-08-11"
        progressValue: 0.0
        progressText: "（缺失）"
        missing: true
        cover: ""
    }
}

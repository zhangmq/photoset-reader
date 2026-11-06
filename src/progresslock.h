#pragma once

#include <QLockFile>
#include <QString>

#include <memory>


// ══ 一份 `.prsave` 的独占锁 ═══════════════════════════════════════════
//   多实例下两个窗口写同一份进度会互相覆盖（自动保存 30 s 一次），所以要独占。
//
//   ★ 锁 **sidecar**（`<x>.prsave.lock`），不锁 `.prsave` 本身：进度是 `QSaveFile`
//     写的（临时文件 → rename），inode 每次都换，锁会跟着旧文件走。
//   ★ `setStaleLockTime(0)`：默认 30 s 按 mtime 判陈旧，会把**还活着**的长锁抢走
//     （静默双写，比锁死更糟）。设 0 = 只看"进程还在 + bootid 变没变"。
//   ★ 崩溃不锁死：锁里记 pid / appname / bootid，进程没了、重启过、PID 被复用都会
//     判陈旧，下一个进程直接拿到（`ib-test-progresslock` 用 kill -9 真进程钉住）。
namespace progresslock {

//   `.prsave` ⇒ sidecar 路径（先规范化，否则 `a/x.prsave` 与 `a/../a/x.prsave` 绕过锁）
QString lockPathFor(const QString &prsavePath);

class Guard
{
public:
    Guard() = default;
    ~Guard();
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

    //   成功 true；失败时 `reasonKey` 取 "in-use" / "readonly-dir" / "lock-error"，
    //   `holder` 是占用者描述（pid@host，只进日志）。同锁同路径重复 acquire ⇒ true。
    bool acquire(const QString &prsavePath, QString *holder = nullptr, QString *reasonKey = nullptr);
    void release();
    bool isHeld() const { return m_lock != nullptr; }
    QString path() const { return m_prsave; }

private:
    std::unique_ptr<QLockFile> m_lock;
    QString m_prsave;
};

}   // namespace progresslock

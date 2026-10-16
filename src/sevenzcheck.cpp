#include "sevenzcheck.h"

#include <QFile>

#include <lzma.h>

// 7z 头解析（只做"判 solid"这一件事）—— 依据 7z 格式规范 / p7zip `7zIn.cpp` 的读法。
//
//   签名头 32 字节：magic(6) + ver(2) + StartHeaderCRC(4) + NextHeaderOffset(8) +
//                   NextHeaderSize(8) + NextHeaderCRC(4)     （数值一律小端）
//   头本体在 `32 + NextHeaderOffset`，长度 NextHeaderSize；首字节是 NID：
//     · kHeader        ⇒ 头是明文，直接走
//     · kEncodedHeader ⇒ 头被压缩（7-Zip 默认）⇒ 用 liblzma 解出来再走
//   判定：走 NID 到 `kUnpackInfo/kFolder`（数出块数）与 `kSubStreamsInfo/kNumUnpackStream`
//        （每个块里几个文件）⇒ **任何块 > 1 个文件 ⇒ solid** ✗

namespace {

using uchar = unsigned char;

constexpr uchar kEnd = 0x00, kHeader = 0x01, kMainStreamsInfo = 0x04, kFilesInfo = 0x05,
                kPackInfo = 0x06, kUnpackInfo = 0x07, kSubStreamsInfo = 0x08, kSize = 0x09,
                kCRC = 0x0A, kFolder = 0x0B, kCodersUnpackSize = 0x0C,
                kNumUnpackStream = 0x0D, kEncodedHeader = 0x17;

// 7z 的变长整数（p7zip ReadNumber）
struct Reader {
    const uchar *p = nullptr;
    const uchar *end = nullptr;
    bool ok = true;

    bool byte(uchar *v)
    {
        if (p >= end) { ok = false; return false; }
        *v = *p++;
        return true;
    }
    bool num(quint64 *v)
    {
        if (p >= end) { ok = false; return false; }
        const uchar first = *p++;
        uchar mask = 0x80;
        quint64 value = 0;
        for (int i = 0; i < 8; ++i) {
            if ((first & mask) == 0) {
                value |= quint64(first & (mask - 1)) << (8 * i);
                *v = value;
                return true;
            }
            if (p >= end) { ok = false; return false; }
            value |= quint64(*p++) << (8 * i);
            mask >>= 1;
        }
        *v = value;
        return true;
    }
    bool skip(qint64 n)
    {
        if (n < 0 || end - p < n) { ok = false; return false; }
        p += n;
        return true;
    }
    quint64 left() const { return quint64(end - p); }
};

// CRC 段：AllAreDefined(byte)；为 0 时跟一个 bit 向量；随后**已定义项各 4 字节**。
//   我们只在"全定义"时跳过（不解析 bit 向量 ⇒ 未全定义就放弃，不猜）
bool skipCrc(Reader &r, quint64 numItems)
{
    uchar allDefined = 0;
    if (!r.byte(&allDefined)) return false;
    if (!allDefined) return false;
    return r.skip(qint64(numItems) * 4);
}

quint32 le32(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
quint64 le64(const uchar *p)
{
    quint64 v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// 7z 的 LZMA2 props（1 字节）⇒ 字典大小
quint32 lzma2DictSize(uchar props)
{
    if (props > 40) return 0xFFFFFFFFu;
    if (props == 40) return 0xFFFFFFFFu;
    return (2u | (props & 1u)) << (props / 2 + 11);
}

// 用 liblzma 解一段裸流（头本体）；不支持/失败返回空
QByteArray decodeLzma(const QByteArray &in, quint64 unpackSize, const QByteArray &props,
                      quint64 coderId)
{
    if (in.isEmpty() || unpackSize == 0 || unpackSize > (64u << 20)) return {};
    QByteArray out(int(unpackSize), Qt::Uninitialized);

    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, 6)) return {};

    lzma_filter filters[2];
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = nullptr;

    if (coderId == 0x21) {                                  // LZMA2
        if (props.size() != 1) return {};
        opt.dict_size = lzma2DictSize(uchar(props.at(0)));
        filters[0].id = LZMA_FILTER_LZMA2;
        filters[0].options = &opt;
    } else if (coderId == 0x030101) {                       // LZMA1
        if (props.size() != 5) return {};
        const uchar d = uchar(props.at(0));
        if (d >= 9 * 5 * 5) return {};
        opt.lc = d % 9;
        opt.lp = (d / 9) % 5;
        opt.pb = d / 45;
        opt.dict_size = le32(reinterpret_cast<const uchar *>(props.constData()) + 1);
        filters[0].id = LZMA_FILTER_LZMA1;
        filters[0].options = &opt;
    } else {
        return {};                                          // 别的编码器（含 BCJ 组合）⇒ 不猜
    }

    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_raw_decoder(&strm, filters) != LZMA_OK) return {};
    strm.next_in = reinterpret_cast<const uint8_t *>(in.constData());
    strm.avail_in = size_t(in.size());
    strm.next_out = reinterpret_cast<uint8_t *>(out.data());
    strm.avail_out = size_t(out.size());
    const lzma_ret r = lzma_code(&strm, LZMA_FINISH);
    const bool done = (r == LZMA_STREAM_END) || (strm.avail_out == 0);
    lzma_end(&strm);
    return done ? out : QByteArray();
}

// 走一个 StreamsInfo（PackInfo / UnpackInfo / SubStreamsInfo），只要块数与"每块文件数"
struct Streams {
    quint64 numFolders = 0;
    quint64 packPos = 0;                 // 相对"签名头之后"
    QVector<quint64> packSizes;
    QVector<quint64> unpackSizes;        // 每个块的解压后大小（只取第一个 coder 的）
    QVector<quint64> numUnpackStreams;   // 每块里的文件数（缺省 1）
    QByteArray firstCoderProps;          // 第一个块的第一个 coder 的 props
    quint64 firstCoderId = 0;
};

// 读 UnpackInfo（块 + coder + 解压后大小）。`onlyFirstCoder`：头流只需要第一个块的编码器
bool readUnpackInfo(Reader &r, Streams *st, bool *external)
{
    uchar id = 0;
    if (!r.byte(&id) || id != kFolder) return false;
    quint64 n = 0;
    if (!r.num(&n)) return false;
    st->numFolders = n;
    uchar ext = 0;
    if (!r.byte(&ext)) return false;
    *external = (ext != 0);
    if (ext) return true;                                   // 外部定义：我们不处理

    for (quint64 i = 0; i < n; ++i) {
        quint64 numCoders = 0;
        if (!r.num(&numCoders)) return false;
        for (quint64 c = 0; c < numCoders; ++c) {
            uchar flags = 0;
            if (!r.byte(&flags)) return false;
            // 7z 规范：bit0-3 = coderId 字节数；**bit4 = isComplex**；**bit5 = hasAttributes**
            //   （我第一版把 4/5 写反了 ⇒ 把普通 LZMA 当"复杂编码器"放弃 ✗）
            const quint64 idSize = flags & 0x0F;
            const bool complexCoder = (flags & 0x10) != 0;  // 多输入/输出流 ⇒ 我们不处理
            const bool hasAttrs = (flags & 0x20) != 0;
            if (complexCoder) return false;
            quint64 coderId = 0;
            for (quint64 k = 0; k < idSize; ++k) {
                uchar b = 0;
                if (!r.byte(&b)) return false;
                coderId = (coderId << 8) | quint64(b);      // ★ 大端：03 01 01 ⇒ 0x030101
            }
            if (hasAttrs) {
                quint64 propSize = 0;
                if (!r.num(&propSize)) return false;
                QByteArray props(int(propSize), Qt::Uninitialized);
                if (propSize) {
                    if (r.left() < propSize) return false;
                    memcpy(props.data(), r.p, size_t(propSize));
                    if (!r.skip(qint64(propSize))) return false;
                }
                if (i == 0 && c == 0) { st->firstCoderId = coderId; st->firstCoderProps = props; }
            } else if (i == 0 && c == 0) {
                st->firstCoderId = coderId;
                st->firstCoderProps.clear();
            }
        }
    }
    for (;;) {
        if (!r.byte(&id)) return false;
        if (id == kCodersUnpackSize) {
            for (quint64 i = 0; i < n; ++i) {
                quint64 sz = 0;
                if (!r.num(&sz)) return false;
                if (i == 0) st->unpackSizes << sz;
            }
            continue;
        }
        if (id == kCRC) {
            if (!skipCrc(r, n)) return false;               // 每个块一个 CRC
            continue;
        }
        if (id == kEnd) break;
        return false;                                       // 没见过的 NID ⇒ 不猜
    }
    return true;
}

// 读 SubStreamsInfo：只要"每块几个文件"
bool readSubStreams(Reader &r, Streams *st)
{
    st->numUnpackStreams.fill(1, int(st->numFolders));
    uchar id = 0;
    for (;;) {
        if (!r.byte(&id)) return false;
        if (id == kNumUnpackStream) {
            for (quint64 i = 0; i < st->numFolders; ++i) {
                quint64 v = 0;
                if (!r.num(&v)) return false;
                if (i < quint64(st->numUnpackStreams.size())) st->numUnpackStreams[int(i)] = v;
            }
            continue;
        }
        if (id == kSize) {
            // 每个块里"除最后一个子流以外"的 size ⇒ 共 cnt-1 个；
            //   ⚠️ **所有块都要数**（我第一版漏了最后一个块 ⇒ 少读一批数字 ⇒ 后面全错位 ✗）
            for (quint64 i = 0; i < st->numFolders; ++i) {
                const quint64 cnt = st->numUnpackStreams.value(int(i), 1);
                for (quint64 k = 1; k < cnt; ++k) {
                    quint64 v = 0;
                    if (!r.num(&v)) return false;
                }
            }
            continue;
        }
        if (id == kCRC) {
            quint64 total = 0;
            for (quint64 i = 0; i < st->numFolders; ++i)
                total += st->numUnpackStreams.value(int(i), 1);
            if (!skipCrc(r, total)) return false;           // 每个子流一个 CRC
            continue;
        }
        if (id == kEnd) break;
        return false;
    }
    return true;
}

// 走 StreamsInfo（PackInfo → UnpackInfo → SubStreamsInfo）
bool readStreamsInfo(Reader &r, Streams *st, bool *external)
{
    uchar id = 0;
    for (;;) {
        if (!r.byte(&id)) return false;
        if (id == kPackInfo) {
            if (!r.num(&st->packPos)) return false;
            quint64 numPack = 0;
            if (!r.num(&numPack)) return false;
            for (;;) {
                if (!r.byte(&id)) return false;
                if (id == kSize) {
                    for (quint64 i = 0; i < numPack; ++i) {
                        quint64 v = 0;
                        if (!r.num(&v)) return false;
                        st->packSizes << v;
                    }
                    continue;
                }
                if (id == kCRC) {
                    if (!skipCrc(r, numPack)) return false;   // 每个 pack 流一个 CRC
                    continue;
                }
                if (id == kEnd) break;
                return false;
            }
            continue;
        }
        if (id == kUnpackInfo) {
            if (!readUnpackInfo(r, st, external)) return false;
            continue;
        }
        if (id == kSubStreamsInfo) {
            if (!readSubStreams(r, st)) return false;
            continue;
        }
        if (id == kEnd) break;
        return false;
    }
    return true;
}

// 解出"头本体"（明文头 / 被压缩的头）
bool loadHeaderBody(QFile &f, const uchar *sig, QByteArray *body, QString *why)
{
    const quint64 nextOff = le64(sig + 12);
    const quint64 nextSize = le64(sig + 20);
    if (nextSize == 0 || nextSize > (64u << 20)) {
        if (why) *why = QStringLiteral("7z 头大小异常");
        return false;
    }
    const qint64 at = 32 + qint64(nextOff);
    if (at < 0 || quint64(at) + nextSize > quint64(f.size())) {
        if (why) *why = QStringLiteral("7z 头越界");
        return false;
    }
    // ★ 只读"头那一段"（几十~几百 KB），**不要** readAll 整包 ✗
    //   （实测：141MB 的 7z 因为整包读，启动多等 1.7s）
    if (!f.seek(at)) {
        if (why) *why = QStringLiteral("读不到 7z 头");
        return false;
    }
    const QByteArray raw = f.read(qint64(nextSize));
    const uchar first = uchar(raw.at(0));
    if (first == kHeader) {
        // ★ **不要把 kHeader 这个 NID 剥掉**：编码头解出来的内容是**带 NID 的**整段 Header
        //   （下面走 NID 的那段代码两条路共用）—— 我第一版这里剥了一层 ⇒ 明文头的小包全被误拒 ✗
        *body = raw;
        return true;
    }
    if (first != kEncodedHeader) {
        if (why) *why = QStringLiteral("7z 头结构不认识");
        return false;
    }
    // 被压缩的头：先读它自己的 StreamsInfo，再用 liblzma 解
    Reader r { reinterpret_cast<const uchar *>(raw.constData()) + 1,
               reinterpret_cast<const uchar *>(raw.constData()) + raw.size() };
    Streams st;
    bool external = false;
    if (!readStreamsInfo(r, &st, &external) || external || st.packSizes.isEmpty()
        || st.unpackSizes.isEmpty()) {
        if (why) *why = QStringLiteral("7z 压缩头结构不认识");
        return false;
    }
    const qint64 packAt = 32 + qint64(st.packPos);
    const quint64 packSize = quint64(st.packSizes.first());
    if (packAt < 0 || quint64(packAt) + packSize > quint64(f.size())) {
        if (why) *why = QStringLiteral("7z 头数据越界");
        return false;
    }
    if (!f.seek(packAt)) {
        if (why) *why = QStringLiteral("读不到 7z 压缩头");
        return false;
    }
    const QByteArray packed = f.read(qint64(packSize));
    const QByteArray decoded = decodeLzma(packed, st.unpackSizes.first(), st.firstCoderProps,
                                          st.firstCoderId);
    if (decoded.isEmpty()) {
        if (why) *why = QStringLiteral("7z 压缩头解不开（编码器不支持）");
        return false;
    }
    *body = decoded;
    return true;
}

} // namespace

bool sevenZipIsRandomAccessible(const QString &path, QString *why)
{
    if (why) why->clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (why) *why = QStringLiteral("打不开文件");
        return false;
    }
    const QByteArray head = f.read(32);
    if (head.size() != 32) {
        if (why) *why = QStringLiteral("不是 7z（文件太短）");
        return false;
    }
    const uchar *sig = reinterpret_cast<const uchar *>(head.constData());
    static const uchar magic[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
    if (memcmp(sig, magic, 6) != 0) {
        if (why) *why = QStringLiteral("不是 7z（签名不对）");
        return false;
    }
    QByteArray body;
    if (!loadHeaderBody(f, sig, &body, why)) return false;

    // 走 Header → MainStreamsInfo（FilesInfo 里还有文件总数，用来补"最后一块"）
    Reader r { reinterpret_cast<const uchar *>(body.constData()),
               reinterpret_cast<const uchar *>(body.constData()) + body.size() };
    uchar id = 0;
    if (!r.byte(&id) || id != kHeader) {
        if (why) *why = QStringLiteral("7z 头不是 kHeader");
        return false;
    }
    Streams st;
    bool external = false;
    quint64 numFiles = 0;
    for (;;) {
        if (!r.byte(&id)) break;
        if (id == kMainStreamsInfo) {
            if (!readStreamsInfo(r, &st, &external)) {
                if (why) *why = QStringLiteral("7z 主流信息不认识");
                return false;
            }
            continue;
        }
        if (id == kFilesInfo) {
            if (!r.num(&numFiles)) break;
            break;                                          // 后面的属性不用看
        }
        if (id == kEnd) break;
        return false;                                       // 没见过的 NID ⇒ 不猜
    }

    if (st.numFolders == 0) {
        if (why) *why = QStringLiteral("7z 里没有块信息");
        return false;
    }
    // 缺省：每块 1 个文件（规范：没有 kNumUnpackStream 就是每块 1 个 ⇒ 非 solid）；
    //   给了就按它 ⇒ **任何块 > 1 个文件就是 solid** ✓
    //   ⚠️ **不要**再用"总文件数 − 前面之和"去补最后一块 ✗ ——
    //     7z 的"文件数"把**目录条目**（空流）也算进去 ⇒ 会误判 solid ✗（实测踩到）
    (void)numFiles;
    for (quint64 i = 0; i < st.numFolders; ++i) {
        if (st.numUnpackStreams.value(int(i), 1) > 1) {
            if (why) *why = QStringLiteral("7z 是 solid（一个块装了 %1 个文件）")
                                .arg(st.numUnpackStreams.value(int(i), 1));
            return false;
        }
    }
    return true;                                            // 每块一个文件 ⇒ 能随机读 ✓
}

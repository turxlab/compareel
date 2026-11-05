#include "ImageReaderInternal.h"
#include <QColorSpace>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QXmlStreamReader>
#if __has_include(<QtZlib/zlib.h>)
#include <QtZlib/zlib.h>
#else
#include <zlib.h>
#endif
#include <cmath>

namespace StillImages::Internal {

QImage decodeSvgSafe(const QByteArray& inputBytes, bool isSvgz, std::atomic<bool>& cancel, HRESULT& error, ImageExtendedInfo* ext) {
    QByteArray bytes=inputBytes;
    if(bytes.startsWith(QByteArray::fromHex("1f8b"))) {
        z_stream stream{};
        if(inflateInit2(&stream,15+16)!=Z_OK) return {};
        struct End { z_stream* stream; ~End() { inflateEnd(stream); } } end{&stream};
        stream.next_in=reinterpret_cast<Bytef*>(bytes.data()); stream.avail_in=uInt(bytes.size());
        QByteArray unpacked; int status=Z_OK;
        while(status==Z_OK && !cancel.load()) {
            char block[32768]; stream.next_out=reinterpret_cast<Bytef*>(block); stream.avail_out=sizeof(block);
            status=inflate(&stream,Z_NO_FLUSH); const auto count=sizeof(block)-stream.avail_out;
            if(unpacked.size()+qsizetype(count)>16*1024*1024) { error=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); return {}; }
            unpacked.append(block,qsizetype(count));
        }
        if(cancel.load()) { error=HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
        if(status!=Z_STREAM_END || stream.avail_in) return {};
        bytes=std::move(unpacked);
    }
    QXmlStreamReader xml(bytes); bool root=false;
    auto safeCss=[](QString value) {
        if(value.contains('@') || value.contains('\\')) return false;
        static const QRegularExpression urls("url\\s*\\(([^)]*)\\)",QRegularExpression::CaseInsensitiveOption);
        auto matches=urls.globalMatch(value);
        while(matches.hasNext()) { auto target=matches.next().captured(1).trimmed(); target.remove('"'); target.remove('\''); if(!target.startsWith('#')) return false; }
        return true;
    };
    // Nested <use> references multiply rendered elements (a "billion laughs" in a few KB), so bound the expanded tree.
    struct SvgNode { std::vector<int> children; QString use; };
    std::vector<SvgNode> nodes; std::vector<int> open; QHash<QString,int> ids; int uses=0;
    constexpr int maxDepth=256, maxUses=4096; constexpr qint64 maxRendered=200000;
    while(!xml.atEnd()) {
        xml.readNext();
        if(xml.tokenType()==QXmlStreamReader::DTD || xml.tokenType()==QXmlStreamReader::EntityReference) return {};
        if(xml.isStartElement()) {
            const auto name=xml.name().toString().toLower();
            if(!root) { if(name!="svg") return {}; root=true; }
            if(name=="script" || name=="foreignobject") return {};
            const int node=int(nodes.size()); nodes.push_back({});
            if(!open.empty()) nodes[open.back()].children.push_back(node);
            open.push_back(node); if(open.size()>maxDepth) return {};
            for(const auto& attribute:xml.attributes()) {
                auto value=attribute.value().toString().trimmed();
                if(attribute.name().compare(QStringView(u"href"),Qt::CaseInsensitive)==0 && !value.startsWith('#') &&
                   !value.startsWith("data:image/png;base64,",Qt::CaseInsensitive) && !value.startsWith("data:image/jpeg;base64,",Qt::CaseInsensitive) &&
                   !value.startsWith("data:image/gif;base64,",Qt::CaseInsensitive) && !value.startsWith("data:image/webp;base64,",Qt::CaseInsensitive)) return {};
                if(!safeCss(value)) return {};
                if(attribute.name()==QStringView(u"id")) ids.insert(value,node);
                else if(name=="use" && attribute.name().compare(QStringView(u"href"),Qt::CaseInsensitive)==0 && value.startsWith('#')) {
                    nodes[node].use=value.mid(1); if(++uses>maxUses) return {};
                }
            }
        } else if(xml.isEndElement()) { if(!open.empty()) open.pop_back(); }
        else if(xml.isCharacters() && !safeCss(xml.text().toString())) return {};
    }
    if(xml.hasError() || !root) return {};
    if(uses) {
        std::vector<qint64> rendered(nodes.size(),-1); std::vector<char> visiting(nodes.size(),0);
        std::function<qint64(int,int)> count=[&](int node,int depth)->qint64 {
            if(rendered[node]>=0) return rendered[node];
            if(visiting[node] || depth>maxDepth*4) return maxRendered+1; // reference cycle or runaway chain
            visiting[node]=1; qint64 total=1;
            for(int child:nodes[node].children) { if(total>maxRendered) break; total+=count(child,depth+1); }
            if(total<=maxRendered && !nodes[node].use.isEmpty()) if(auto target=ids.constFind(nodes[node].use); target!=ids.cend()) total+=count(*target,depth+1);
            visiting[node]=0; return rendered[node]=std::min(total,maxRendered+1);
        };
        if(count(0,0)>maxRendered) { error=HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return {}; }
    }
    if (cancel.load()) { error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return {}; }
    if (bytes.size() < 4 || bytes.size() > 16 * 1024 * 1024) {
        error = (bytes.size() > 16 * 1024 * 1024) ? HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return {};
    }
    Q_UNUSED(isSvgz);
    QSvgRenderer renderer(bytes);
    if (!renderer.isValid()) {
        error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return {};
    }
    QSize targetSize = renderer.defaultSize();
    if (!targetSize.isValid() || targetSize.width() <= 0 || targetSize.height() <= 0) {
        const QRectF vb = renderer.viewBoxF();
        if (vb.width() > 0.0 && vb.height() > 0.0) {
            targetSize = QSize(int(std::round(vb.width())), int(std::round(vb.height())));
        } else {
            targetSize = QSize(512, 512);
        }
    }
    double w = std::max(1, targetSize.width());
    double h = std::max(1, targetSize.height());
    const double maxDim = std::max(w, h);
    if (maxDim < 256.0) {
        const double scale = 512.0 / maxDim;
        w = std::round(w * scale);
        h = std::round(h * scale);
    } else if (maxDim > 2048.0) {
        const double scale = 2048.0 / maxDim;
        w = std::round(w * scale);
        h = std::round(h * scale);
    }
    const int rw = std::clamp(int(w), 1, 4096);
    const int rh = std::clamp(int(h), 1, 4096);

    QImage image(rw, rh, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) { error = E_OUTOFMEMORY; return {}; }
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter, QRectF(0, 0, rw, rh));
    }
    image.setColorSpace(QColorSpace::SRgb);
    image = toWhiteSRgb(std::move(image));
    if (image.isNull()) { error = E_OUTOFMEMORY; return {}; }
    if (ext) {
        ext->kind = ImageEvidenceKind::VectorRasterized;
        ext->frameOrPageCount = 1;
    }
    error = S_OK;
    return image;
}

} // namespace StillImages::Internal

#include "ScanManifest.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <stdexcept>

static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir root; require(root.isValid(),"private root");
        ScanCriteria criteria; criteria.comparisonRoots={root.path().toStdWString()}; criteria.protectedFolders=criteria.comparisonRoots;
        std::vector<VideoFileInfo> files;
        for(int i=0;i<2;++i) {
            VideoFileInfo file; file.fullPath=(root.path()+"/"+QString::number(i)+".mp4").toStdWString();
            file.identity.valid=true; file.identity.volume=1; file.identity.index=i+1; file.identity.size=file.fileSize=i+1; file.identity.modified=1;
            files.push_back(std::move(file));
        }
        const auto path=root.filePath("list.json"); require(ScanManifest::save(path,criteria,files),"save fixture");
        std::vector<VideoFileInfo> restored;
        require(ScanManifest::load(path,criteria,restored) && restored.size()==2 && restored[0].protectedFile && restored[0].protectedByFolder,"round trip and authoritative folder protection");
        QFile input(path); require(input.open(QIODevice::ReadOnly),"fixture read"); const auto original=input.readAll(); input.close();
        auto reject=[&](const std::function<void(QJsonObject&)>& mutate) {
            auto object=QJsonDocument::fromJson(original).object(); mutate(object);
            QFile output(path); require(output.open(QIODevice::WriteOnly|QIODevice::Truncate),"invalid fixture open");
            const auto bytes=QJsonDocument(object).toJson(); require(output.write(bytes)==bytes.size(),"invalid fixture write"); output.close();
            require(!ScanManifest::load(path,criteria,restored) && restored.size()==2 && restored[0].protectedFile,"invalid manifest changed destination");
        };
        reject([](auto& o) { o["version"]=2; });
        reject([](auto& o) { o["context"]="wrong"; });
        reject([](auto& o) { o["files"]="not an array"; });
        reject([](auto& o) { auto a=o["files"].toArray(); a.append(a[0]); o["files"]=a; });
        reject([](auto& o) { auto a=o["files"].toArray(); auto r=a[1].toArray(); r[2]="1"; a[1]=r; o["files"]=a; });
        reject([](auto& o) { auto a=o["files"].toArray(); auto r=a[0].toArray(); r[0]="relative.mp4"; a[0]=r; o["files"]=a; });
        reject([](auto& o) { auto a=o["files"].toArray(); auto r=a[0].toArray(); r[3]="18446744073709551616"; a[0]=r; o["files"]=a; });
        reject([](auto& o) { auto a=o["files"].toArray(); auto r=a[0].toArray(); r[5]="false"; a[0]=r; o["files"]=a; });
        { QFile output(path); require(output.open(QIODevice::WriteOnly) && output.resize(33*1024*1024),"oversized input fixture"); }
        require(!ScanManifest::load(path,criteria,restored) && restored.size()==2,"oversized input accepted");
        const auto prototype=files.front(); files.reserve(100001);
        for(int i=2;i<100000;++i) { auto file=prototype; file.fullPath=(root.path()+"/"+QString::number(i)+".mp4").toStdWString(); file.identity.index=i+1; files.push_back(std::move(file)); }
        QElapsedTimer timer; timer.start();
        require(ScanManifest::save(path,criteria,files) && ScanManifest::load(path,criteria,restored) && restored.size()==100000 && restored.back().identity.index==100000,"100000-member round trip");
        const auto ms=timer.elapsed(); const auto bytes=QFileInfo(path).size();
        QFile saved(path); require(saved.open(QIODevice::ReadOnly),"saved list read"); const auto committed=saved.readAll(); saved.close();
        auto unchanged=[&] { QFile check(path); return check.open(QIODevice::ReadOnly) && check.readAll()==committed; };
        files.push_back(prototype); require(!ScanManifest::save(path,criteria,files) && unchanged(),"file limit overwrote checkpoint");
        files.resize(2000); restored.clear();
        const auto longName=std::wstring(20000,L'x'); for(auto& file:files) file.fullPath+=longName;
        require(!ScanManifest::save(path,criteria,files) && unchanged(),"byte limit overwrote checkpoint");
        printf("PASS scan manifest: 8 malformed cases, oversized input, protection, 100000-member round trip bytes=%lld elapsed_ms=%lld; count/byte overflow preserves committed list; no media files created\n",static_cast<long long>(bytes),static_cast<long long>(ms));
        return 0;
    } catch(const std::exception& error) { fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}

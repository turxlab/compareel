#include "ShellIntegration.h"
#include "platform/WinApi.h"
#include <wrl/client.h>
#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>
#include <stdexcept>
static void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        require(SUCCEEDED(com) && argc==2,"usage ShellChecks Compareel.exe");
        QTemporaryDir root; require(root.isValid(),"private fixture root");
        const auto link=root.filePath(QString::fromUtf8("Compareel 資料.lnk")),exe=QFileInfo(app.arguments()[1]).absoluteFilePath();
        require(ShellIntegration::install(link,exe) && ShellIntegration::owned(link),"create owned shortcut");
        Microsoft::WRL::ComPtr<IShellLinkW> loaded; Microsoft::WRL::ComPtr<IPersistFile> persisted;
        require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&loaded))) && SUCCEEDED(loaded.As(&persisted)) &&
                SUCCEEDED(persisted->Load(link.toStdWString().c_str(),STGM_READ)),"load real Shell shortcut");
        wchar_t args[128]{}; require(SUCCEEDED(loaded->GetArguments(args,128)) && std::wstring(args)==L"--","SendTo argument separator");
        // Keep the production shortcut's file argument flow, adding only the isolated self-check flag.
        require(SUCCEEDED(loaded->SetArguments(L"--open-pair-check --")) && SUCCEEDED(persisted->Save(nullptr,TRUE)),"self-check shortcut");
        const auto a=root.filePath(QString::fromUtf8("Örnek 資料 left.png")),b=root.filePath(QString::fromUtf8("Sağ image.png"));
        QImage image(32,32,QImage::Format_RGB32); image.fill(Qt::red); require(image.save(a),"red fixture"); image.fill(Qt::blue); require(image.save(b),"blue fixture");
        const auto parameters=(QString('"')+a+"\" \""+b+'"').toStdWString(); const auto native=QDir::toNativeSeparators(link).toStdWString();
        SHELLEXECUTEINFOW info{}; info.cbSize=sizeof(info); info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
        info.lpFile=native.c_str(); info.lpParameters=parameters.c_str(); info.nShow=SW_HIDE;
        require(ShellExecuteExW(&info) && info.hProcess,"launch through real .lnk");
        const auto waited=WaitForSingleObject(info.hProcess,30000); DWORD code=1;
        if(waited!=WAIT_OBJECT_0) TerminateProcess(info.hProcess,77);
        GetExitCodeProcess(info.hProcess,&code); CloseHandle(info.hProcess);
        if(waited!=WAIT_OBJECT_0 || code) fprintf(stderr,"Shell child wait=%lu exit=%lu\n",waited,code);
        require(waited==WAIT_OBJECT_0 && code==0,"shortcut/Unicode two-file startup/A-B paint integration");
        require(ShellIntegration::install(link,exe),"refresh owned shortcut");
        require(ShellIntegration::uninstall(link) && !QFileInfo::exists(link),"remove owned shortcut");
        { QFile foreign(link); require(foreign.open(QIODevice::WriteOnly) && foreign.write("unrelated")==9,"foreign shortcut fixture"); }
        require(!ShellIntegration::install(link,exe) && !ShellIntegration::uninstall(link),"foreign shortcut overwrite/removal refused");
        puts("PASS Shell + A/B: real .lnk launch, Unicode/spaces, separator, temporary user state, canvas swap/release/focus/privacy, owned refresh/remove and foreign guard");
    } catch(const std::exception& e) { fprintf(stderr,"FAIL Shell: %s\n",e.what()); if(SUCCEEDED(com)) CoUninitialize(); return 1; }
    CoUninitialize();
}

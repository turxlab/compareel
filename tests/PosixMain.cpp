// Entry point for check programs written against wmain (Windows wide arguments): converts UTF-8 arguments.
#include "platform/PosixSupport.h"

int CompareelCheckMain(int argc, wchar_t** argv);

int main(int argc, char** argv) {
    std::vector<std::wstring> storage;
    std::vector<wchar_t*> arguments;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) storage.push_back(Platform::Posix::FromUtf8(argv[i]));
    for (auto& argument : storage) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    return CompareelCheckMain(argc, arguments.data());
}

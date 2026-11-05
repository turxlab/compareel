#pragma once
// Result codes and a few handle types shared by every platform.
// Error values are Windows HRESULTs everywhere: they are written to the scan cache and to saved sessions,
// so a file produced on one platform must read the same on another. Only Windows headers define them natively.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdint>

using HRESULT = int32_t;
using DWORD = uint32_t;
using HANDLE = void*;
using HWND = void*;

constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001u);
constexpr HRESULT E_POINTER = static_cast<HRESULT>(0x80004003u);
constexpr HRESULT E_ABORT = static_cast<HRESULT>(0x80004004u);
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u);
constexpr HRESULT E_PENDING = static_cast<HRESULT>(0x8000000Au);
constexpr HRESULT E_UNEXPECTED = static_cast<HRESULT>(0x8000FFFFu);
constexpr HRESULT E_ACCESSDENIED = static_cast<HRESULT>(0x80070005u);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000Eu);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057u);

constexpr bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
constexpr bool FAILED(HRESULT hr) { return hr < 0; }
constexpr HRESULT HRESULT_FROM_WIN32(uint32_t error) {
    return static_cast<int32_t>(error) <= 0 ? static_cast<HRESULT>(error) : static_cast<HRESULT>((error & 0xFFFFu) | (7u << 16) | 0x80000000u);
}
constexpr HRESULT HRESULT_FROM_NT(uint32_t status) { return static_cast<HRESULT>(status | 0x10000000u); }

// Win32 error numbers used by Compareel (same values as winerror.h).
constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_FILE_NOT_FOUND = 2;
constexpr uint32_t ERROR_PATH_NOT_FOUND = 3;
constexpr uint32_t ERROR_ACCESS_DENIED = 5;
constexpr uint32_t ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr uint32_t ERROR_BAD_FORMAT = 11;
constexpr uint32_t ERROR_INVALID_DATA = 13;
constexpr uint32_t ERROR_OUTOFMEMORY = 14;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_WRITE_PROTECT = 19;
constexpr uint32_t ERROR_NOT_READY = 21;
constexpr uint32_t ERROR_CRC = 23;
constexpr uint32_t ERROR_GEN_FAILURE = 31;
constexpr uint32_t ERROR_SHARING_VIOLATION = 32;
constexpr uint32_t ERROR_HANDLE_EOF = 38;
constexpr uint32_t ERROR_NOT_SUPPORTED = 50;
constexpr uint32_t ERROR_BAD_NETPATH = 53;
constexpr uint32_t ERROR_DEV_NOT_EXIST = 55;
constexpr uint32_t ERROR_UNEXP_NET_ERR = 59;
constexpr uint32_t ERROR_NETNAME_DELETED = 64;
constexpr uint32_t ERROR_FILE_EXISTS = 80;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_DISK_FULL = 112;
constexpr uint32_t ERROR_SEM_TIMEOUT = 121;
constexpr uint32_t ERROR_INVALID_NAME = 123;
constexpr uint32_t ERROR_DIR_NOT_EMPTY = 145;
constexpr uint32_t ERROR_ALREADY_EXISTS = 183;
constexpr uint32_t ERROR_FILE_TOO_LARGE = 223;
constexpr uint32_t ERROR_DIRECTORY = 267;
constexpr uint32_t ERROR_OPERATION_ABORTED = 995;
constexpr uint32_t ERROR_FILE_INVALID = 1006;
constexpr uint32_t ERROR_NOT_FOUND = 1168;
constexpr uint32_t ERROR_CANCELLED = 1223;
constexpr uint32_t ERROR_TIMEOUT = 1460;

constexpr uint32_t MAX_PATH = 260;
inline const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
#endif

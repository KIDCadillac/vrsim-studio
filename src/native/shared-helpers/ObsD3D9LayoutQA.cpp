// ObsD3D9LayoutQA.cpp -- Gate0 read-only dynamic-layout diagnostic for a
// self-owned Win32/D3D9 fixture. x64-only console. No rendering, no Present,
// no Reset, no flag writes, no byte patches, no shared textures, no threads,
// no SIM/RPC, no OBS runtime dependency.
//
// Signature/mask data and the scan idea are derived from the official OBS
// Studio project file plugins/win-capture/get-graphics-offsets/d3d9-offsets.cpp
// (https://github.com/obsproject/obs-studio/blob/30b63c68490be1b0e0cb250a61c9eca1b01c9076),
// GPLv2+, see reference/COPYING in this directory. Only the layout-recognition
// signatures for x64 are reproduced here; none of the OBS capture-side
// implementation is copied into this tool.
//
// Build (reviewed/compiled/run by Codex/root, x64):
//   cl /std:c++17 /EHsc ObsD3D9LayoutQA.cpp
// Links user32.lib and version.lib (kernel32 by default). d3d9.dll is loaded
// at runtime from the system directory only; do NOT link d3d9.lib.
//
// Usage: ObsD3D9LayoutQA.exe <new-json-result-path>   (path must not exist)

#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "dbghelp.lib")

namespace {

// ---------------------------------------------------------------------------
// x64 masked signatures, verbatim values from the official OBS reference
// ---------------------------------------------------------------------------
constexpr size_t kMaxCmpSize = 22;
constexpr size_t kMaxFuncScanBytes = 200;

const uint8_t kMask[4][kMaxCmpSize] = {
    {
        0xF8, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0x00,
        0xF8, 0xF8, 0x00, 0x00, 0x00, 0x00
    },
    {
        0xF8, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0x00,
        0xF8, 0xF8, 0x00, 0x00, 0x00, 0x00
    },
    {
        0xF8, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
    },
    {
        0xF8, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x00
    },
};

const uint8_t kMaskCmp[4][kMaxCmpSize] = {
    {
        0x48, 0x8B, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x44, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x75, 0x00,
        0x40, 0xB8, 0x00, 0x00, 0x00, 0x00
    },
    {
        0x48, 0x8B, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x39, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x75, 0x00,
        0x40, 0xB8, 0x00, 0x00, 0x00, 0x00
    },
    {
        0x48, 0x8B, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x39, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x75, 0x00,
        0x48, 0x8D, 0x00, 0x00, 0x00, 0x00
    },
    {
        0x48, 0x8B, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x83, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x75, 0x00,
        0x48, 0x8D, 0x00, 0x00, 0x00
    },
};

const uint32_t kCodeOffsets[4][2] = {
    { 3, 10 },
    { 3, 9 },
    { 3, 9 },
    { 3, 9 },
};

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
std::string JsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                sprintf_s(buf, "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string s((size_t)n, '\0');
    n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    s.resize((size_t)n);
    return s;
}

std::string HexPtr(uintptr_t v)
{
    char buf[32];
    sprintf_s(buf, "0x%016llx", (unsigned long long)v);
    return std::string(buf);
}

std::string HexHresult(HRESULT hr)
{
    char buf[16];
    sprintf_s(buf, "0x%08lx", (unsigned long)hr);
    return std::string(buf);
}

std::string HexBytes(const uint8_t* p, size_t n)
{
    std::string s;
    s.reserve(n * 2);
    char buf[4];
    for (size_t i = 0; i < n; ++i) {
        sprintf_s(buf, "%02x", p[i]);
        s += buf;
    }
    return s;
}

// Mutable JSON object body builder. Values are pre-rendered strings.
struct JsonObj {
    std::string body;
    bool first = true;

    void add(const std::string& key, const std::string& renderedValue)
    {
        if (!first)
            body += ",";
        first = false;
        body += "\"" + JsonEscape(key) + "\":" + renderedValue;
    }
    void str(const std::string& key, const std::string& v)
    {
        add(key, "\"" + JsonEscape(v) + "\"");
    }
    void num(const std::string& key, long long v)
    {
        add(key, std::to_string(v));
    }
    void boolean(const std::string& key, bool v)
    {
        add(key, v ? "true" : "false");
    }
    void nullableNum(const std::string& key, bool has, long long v)
    {
        add(key, has ? std::to_string(v) : "null");
    }
    void nullableBool(const std::string& key, bool has, bool v)
    {
        add(key, has ? (v ? "true" : "false") : "null");
    }
    void nullableStr(const std::string& key, bool has, const std::string& v)
    {
        add(key, has ? "\"" + JsonEscape(v) + "\"" : "null");
    }
    static std::string arrayOfStrings(const std::vector<std::string>& items)
    {
        std::string s = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i)
                s += ",";
            s += "\"" + JsonEscape(items[i]) + "\"";
        }
        s += "]";
        return s;
    }
};

// ---------------------------------------------------------------------------
// Read-only memory guards (SEH helpers contain no objects with destructors to
// stay clear of MSVC C2712)
// ---------------------------------------------------------------------------
bool RangeReadable(const void* addr, SIZE_T size)
{
    const uint8_t* start = (const uint8_t*)addr;
    SIZE_T remaining = size;
    while (remaining > 0) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(start, &mbi, sizeof(mbi)) == 0)
            return false;
        if (mbi.BaseAddress > (void*)start)
            return false; // gap: unqueried/never-committed territory
        if (mbi.State != MEM_COMMIT)
            return false;
        DWORD basic = mbi.Protect & 0xFF;
        if (basic == PAGE_NOACCESS)
            return false;
        if (mbi.Protect & PAGE_GUARD)
            return false;
        const bool readable = basic == PAGE_READONLY || basic == PAGE_READWRITE ||
                              basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE ||
                              basic == PAGE_EXECUTE_READ || basic == PAGE_EXECUTE_READWRITE ||
                              basic == PAGE_EXECUTE_WRITECOPY;
        if (!readable)
            return false;
        SIZE_T avail = (SIZE_T)((uint8_t*)mbi.BaseAddress + mbi.RegionSize) - (size_t)start;
        if (avail == 0)
            avail = mbi.RegionSize;
        SIZE_T step = avail < remaining ? avail : remaining;
        start += step;
        remaining -= step;
    }
    return true;
}

bool ReadBytesSafe(const void* src, void* dst, size_t n)
{
    if (!RangeReadable(src, n))
        return false;
    __try {
        const volatile uint8_t* s = (const volatile uint8_t*)src;
        uint8_t* d = (uint8_t*)dst;
        for (size_t i = 0; i < n; ++i)
            d[i] = s[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool ReadU64LE(const void* addr, uint64_t* out)
{
    uint8_t bytes[8];
    if (!ReadBytesSafe(addr, bytes, 8))
        return false;
    *out = (uint64_t)bytes[0] | ((uint64_t)bytes[1] << 8) | ((uint64_t)bytes[2] << 16) |
           ((uint64_t)bytes[3] << 24) | ((uint64_t)bytes[4] << 32) | ((uint64_t)bytes[5] << 40) |
           ((uint64_t)bytes[6] << 48) | ((uint64_t)bytes[7] << 56);
    return true;
}

bool ReadU32LE(const void* addr, uint32_t* out)
{
    uint8_t bytes[4];
    if (!ReadBytesSafe(addr, bytes, 4))
        return false;
    *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
    return true;
}

bool AddressInExecutableImageSection(HMODULE module, uintptr_t addr, SIZE_T imageSize, SIZE_T span)
{
    if (span == 0 || imageSize == 0 || imageSize > 0xFFFFFFFFull)
        return false;
    const uintptr_t base = (uintptr_t)module;
    if (addr < base)
        return false;
    const SIZE_T offset = (SIZE_T)(addr - base);
    if (offset >= imageSize || span > imageSize - offset)
        return false; // [addr, addr+span) must lie entirely inside the loaded image
    const DWORD rva = (DWORD)offset;
    const DWORD rvaEnd = rva + (DWORD)span; // no overflow: rva+span <= imageSize <= 0xFFFFFFFF
    PIMAGE_NT_HEADERS nt = ImageNtHeader(module);
    if (!nt)
        return false;
    const DWORD numberOfSections = nt->FileHeader.NumberOfSections;
    if (numberOfSections == 0 || numberOfSections > 96)
        return false;
    const PIMAGE_SECTION_HEADER sec =
        (PIMAGE_SECTION_HEADER)((PUCHAR)nt + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
                               nt->FileHeader.SizeOfOptionalHeader);
    for (DWORD i = 0; i < numberOfSections; ++i) {
        const DWORD va = sec[i].VirtualAddress;
        const DWORD vs = sec[i].Misc.VirtualSize;
        const DWORD raw = sec[i].SizeOfRawData;
        const DWORD region = vs ? vs : raw;
        if (region == 0 || va > 0xFFFFFFFFu - region)
            continue; // section-end overflow: unusable
        // entire span inside this one section, not merely its first byte
        if (rva >= va && rvaEnd <= va + region)
            return (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
    return false;
}

bool MaskedSignatureMatch(const uint8_t* window, size_t pattern, uint32_t* offA, uint32_t* offB)
{
    for (size_t i = 0; i < kMaxCmpSize; ++i) {
        if ((window[i] & kMask[pattern][i]) != kMaskCmp[pattern][i])
            return false;
    }
    const uint32_t codeOffsetA = kCodeOffsets[pattern][0];
    const uint32_t codeOffsetB = kCodeOffsets[pattern][1];
    *offA = (uint32_t)window[codeOffsetA] | ((uint32_t)window[codeOffsetA + 1] << 8) |
            ((uint32_t)window[codeOffsetA + 2] << 16) | ((uint32_t)window[codeOffsetA + 3] << 24);
    *offB = (uint32_t)window[codeOffsetB] | ((uint32_t)window[codeOffsetB + 1] << 8) |
            ((uint32_t)window[codeOffsetB + 2] << 16) | ((uint32_t)window[codeOffsetB + 3] << 24);
    return true;
}

// ---------------------------------------------------------------------------
// Optional value holder
// ---------------------------------------------------------------------------
template <typename T>
struct Opt {
    bool present = false;
    T value{};
    void set(const T& v)
    {
        value = v;
        present = true;
    }
};

} // namespace

typedef HRESULT(WINAPI* PFN_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex**);
typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9)(UINT SDKVersion);

#if !defined(VRSIM_PRODUCTION_SHARED_HELPERS)
int wmain(int argc, wchar_t** argv)
{
    // -----------------------------------------------------------------------
    // Invocation gate: exactly one argument, a path that must not exist.
    // The tool runs once; no retry logic exists anywhere in this program.
    // -----------------------------------------------------------------------
    if (argc != 2 || argv[1] == nullptr || argv[1][0] == L'\0') {
        fwprintf(stderr,
                 L"usage: ObsD3D9LayoutQA.exe <new-json-result-path>\n"
                 L"exactly one argument required; no device/SDK/PID/port/handle input is accepted.\n");
        return 2;
    }
    const std::wstring outPath = argv[1];
    if (GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"refusing to overwrite existing result file.\n");
        return 2;
    }

    std::vector<std::string> errors;
    std::string cleanupStatus = "not-started";

    // Owned resources (only this tool holds and releases these).
    HWND hwnd = nullptr;
    ATOM classAtom = 0;
    const wchar_t* kClassName = L"ObsD3D9LayoutQAGate0Hidden64";
    HMODULE d3d9Module = nullptr;
    IDirect3D9Ex* d3d9exParent = nullptr;
    IDirect3DDevice9Ex* exDevice = nullptr;
    IDirect3D9* d3d9BaseParent = nullptr;
    IDirect3DDevice9* baseDevice = nullptr;

    // Module facts.
    std::wstring modulePathW;
    std::string versionStr;
    SIZE_T imageSize = 0;

    // Gate results.
    Opt<uint32_t> off1, off2;
    Opt<bool> signatureMatched;
    Opt<size_t> matchIndex;
    Opt<size_t> matchCodeOffset;
    std::string runtimeProofMatchedWindow, runtimeProofFuncStart;
    Opt<bool> exParentExact;
    Opt<uint32_t> exBoolActual;
    Opt<std::string> exKnownParentHex, exInternalParentHex;
    Opt<bool> baseParentCanonicalIdentityMatch;
    Opt<uint32_t> baseBoolActual;
    Opt<std::string> baseKnownParentHex, baseInternalParentHex, basePublicParentHex;
    HRESULT exCreateDeviceHr = E_UNEXPECTED;
    HRESULT baseCreateDeviceHr = E_UNEXPECTED;
    HRESULT exParentCreateHr = E_UNEXPECTED;
    bool exCreateDeviceHrAttempted = false;
    bool baseCreateDeviceHrAttempted = false;
    bool exParentCreateHrAttempted = false;
    bool d3d9exParentCreated = false;
    bool d3d9BaseParentCreated = false;
    bool baseGetDirect3dHrOk = false;

    // -----------------------------------------------------------------------
    // Step 1: x64-only gate.
    // -----------------------------------------------------------------------
#ifdef _M_X64
    const bool isX64 = true;
#else
    const bool isX64 = false;
#endif
    if (!isX64 || sizeof(void*) != 8)
        errors.push_back("x64 gate failed: this diagnostic must be compiled and run as x64");

    // -----------------------------------------------------------------------
    // Step 1b: own hidden Win32 window, 64x64, never shown, no message pump.
    // -----------------------------------------------------------------------
    if (isX64 && sizeof(void*) == 8) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = 0;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        classAtom = RegisterClassExW(&wc);
        if (classAtom == 0) {
            char buf[64];
            sprintf_s(buf, "RegisterClassExW failed (win=%lu)", (unsigned long)GetLastError());
            errors.push_back(buf);
        } else {
            hwnd = CreateWindowExW(0, kClassName, L"ObsD3D9LayoutQA gate0", WS_OVERLAPPEDWINDOW, 0, 0,
                                   64, 64, nullptr, nullptr, wc.hInstance, nullptr);
            if (!hwnd) {
                char buf[64];
                sprintf_s(buf, "CreateWindowExW failed (win=%lu)", (unsigned long)GetLastError());
                errors.push_back(buf);
            }
            // Deliberately no ShowWindow, no message loop.
        }
    }

    // -----------------------------------------------------------------------
    // Step 2: load d3d9.dll from the system directory ONLY, absolute path,
    // LOAD_LIBRARY_SEARCH_SYSTEM32, verify resolved module path, read version
    // and image size. No installer/OBS-directory DLLs are ever accepted.
    // -----------------------------------------------------------------------
    PFN_Direct3DCreate9Ex pCreate9Ex = nullptr;
    PFN_Direct3DCreate9 pCreate9 = nullptr;
    if (hwnd) {
        wchar_t sysDir[MAX_PATH] = {};
        const UINT dirLen = GetSystemDirectoryW(sysDir, MAX_PATH);
        if (dirLen == 0 || dirLen >= MAX_PATH) {
            errors.push_back("GetSystemDirectoryW failed");
        } else {
            std::wstring expected = sysDir;
            expected += L"\\d3d9.dll";
            d3d9Module = LoadLibraryExW(expected.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!d3d9Module) {
                char buf[80];
                sprintf_s(buf, "LoadLibraryExW(system d3d9.dll) failed (win=%lu)", (unsigned long)GetLastError());
                errors.push_back(buf);
            } else {
                wchar_t actualPath[MAX_PATH] = {};
                const DWORD pathLen = GetModuleFileNameW(d3d9Module, actualPath, MAX_PATH);
                if (pathLen == 0 || pathLen >= MAX_PATH) {
                    errors.push_back("GetModuleFileNameW on loaded d3d9.dll failed");
                } else {
                    modulePathW = actualPath;
                    if (_wcsicmp(modulePathW.c_str(), expected.c_str()) != 0) {
                        errors.push_back("loaded d3d9.dll path does not equal the system-directory path; refusing");
                        FreeLibrary(d3d9Module);
                        d3d9Module = nullptr;
                    }
                }
            }

            if (d3d9Module) {
                MODULEINFO mi = {};
                if (GetModuleInformation(GetCurrentProcess(), d3d9Module, &mi, sizeof(mi))) {
                    imageSize = mi.SizeOfImage;
                } else {
                    errors.push_back("GetModuleInformation failed for d3d9.dll");
                }

                DWORD handle = 0;
                const DWORD vsize = GetFileVersionInfoSizeW(modulePathW.c_str(), &handle);
                if (vsize == 0) {
                    errors.push_back("GetFileVersionInfoSizeW failed for d3d9.dll");
                } else {
                    std::vector<uint8_t> vbuf(vsize);
                    if (GetFileVersionInfoW(modulePathW.c_str(), handle, vsize, vbuf.data())) {
                        VS_FIXEDFILEINFO* ffi = nullptr;
                        UINT ffiLen = 0;
                        if (VerQueryValueW(vbuf.data(), L"\\", (LPVOID*)&ffi, &ffiLen) &&
                            ffi && ffiLen >= sizeof(VS_FIXEDFILEINFO)) {
                            char vb[64];
                            sprintf_s(vb, "%u.%u.%u.%u",
                                      (unsigned)HIWORD(ffi->dwFileVersionMS),
                                      (unsigned)LOWORD(ffi->dwFileVersionMS),
                                      (unsigned)HIWORD(ffi->dwFileVersionLS),
                                      (unsigned)LOWORD(ffi->dwFileVersionLS));
                            versionStr = vb;
                        } else {
                            errors.push_back("VerQueryValueW(root) failed for d3d9.dll version");
                        }
                    } else {
                        errors.push_back("GetFileVersionInfoW failed for d3d9.dll");
                    }
                }

                pCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9Module, "Direct3DCreate9Ex");
                pCreate9 = (PFN_Direct3DCreate9)GetProcAddress(d3d9Module, "Direct3DCreate9");
                if (!pCreate9Ex)
                    errors.push_back("GetProcAddress(Direct3DCreate9Ex) failed");
                if (!pCreate9)
                    errors.push_back("GetProcAddress(Direct3DCreate9) failed");
            }
        }
    }

    // -----------------------------------------------------------------------
    // Step 3: create own Ex parent + device. Flags recorded; never modified.
    // -----------------------------------------------------------------------
    const DWORD kExBehaviorFlags = 0x46;
    const DWORD kBaseBehaviorFlags = 0x44;

    if (pCreate9Ex && imageSize > 0 && !versionStr.empty()) {
        exParentCreateHrAttempted = true;
        const HRESULT hrP = pCreate9Ex(D3D_SDK_VERSION, &d3d9exParent);
        exParentCreateHr = hrP;
        d3d9exParentCreated = (SUCCEEDED(hrP) && d3d9exParent != nullptr);
        if (!d3d9exParentCreated) {
            errors.push_back("Direct3DCreate9Ex failed: " + HexHresult(hrP));
        }
    }

    if (d3d9exParentCreated) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = 64;
        pp.BackBufferHeight = 64;
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        pp.BackBufferCount = 1;
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        pp.MultiSampleQuality = 0;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hwnd;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = FALSE;
        pp.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
        pp.Flags = 0;
        pp.FullScreen_RefreshRateInHz = 0;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        // No Present / BeginScene / Clear / Reset / rendering of any kind.
        exCreateDeviceHrAttempted = true;
        exCreateDeviceHr =
            d3d9exParent->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         kExBehaviorFlags, &pp, nullptr, &exDevice);
        if (FAILED(exCreateDeviceHr) || exDevice == nullptr) {
            exDevice = nullptr;
            errors.push_back("IDirect3D9Ex::CreateDeviceEx failed: " + HexHresult(exCreateDeviceHr));
        }
    }

    // -----------------------------------------------------------------------
    // Step 4: read-only dynamic layout identification from the Ex device
    // vtable[125] (CheckResourceResidency) via the official x64 masked
    // signatures. Zero match / multi-match ambiguity / unsafe range =>
    // offsetKnown=false; no guessing, no old patch-table fallback.
    // -----------------------------------------------------------------------
    if (exDevice && d3d9Module && imageSize > 0) {
        uint64_t vtablePtr = 0;
        uint64_t crrAddr = 0;
        bool ptrOk = ReadU64LE((const void*)exDevice, &vtablePtr) &&
                     ReadU64LE((const uint8_t*)(uintptr_t)vtablePtr + 125ull * 8ull, &crrAddr);
        if (!ptrOk) {
            errors.push_back("vtable/CheckResourceResidency pointer slot read rejected");
            signatureMatched.set(false);
        } else if (!AddressInExecutableImageSection(d3d9Module, (uintptr_t)crrAddr, imageSize,
                                                    kMaxFuncScanBytes + kMaxCmpSize - 1)) {
            errors.push_back("CheckResourceResidency address is not inside an executable section of the system d3d9.dll image; refusing to scan");
            signatureMatched.set(false);
        } else {
            // 200 scan start positions, 22-byte windows => max 221 bytes.
            // Validate the entire read range before touching it; never follow
            // trampolines or out-of-module addresses.
            const size_t kReadSpan = kMaxFuncScanBytes + kMaxCmpSize - 1; // 221
            uint8_t spanBuf[kReadSpan];
            if (!ReadBytesSafe((const void*)(uintptr_t)crrAddr, spanBuf, kReadSpan)) {
                errors.push_back("CheckResourceResidency read range failed VirtualQuery/SEH safety checks");
                signatureMatched.set(false);
            } else {
                runtimeProofFuncStart = HexBytes(spanBuf, 32);

                struct Hit { size_t index; size_t pattern; uint32_t a; uint32_t b; };
                std::vector<Hit> hits;
                bool capRejected = false;
                for (size_t i = 0; i < kMaxFuncScanBytes; ++i) {
                    for (size_t j = 0; j < 4; ++j) {
                        uint32_t a = 0, b = 0;
                        if (MaskedSignatureMatch(&spanBuf[i], j, &a, &b)) {
                            if (a > 0xFFFF || b > 0xFFFF) {
                                capRejected = true;
                                continue;
                            }
                            hits.push_back({ i, j, a, b });
                        }
                    }
                }
                signatureMatched.set(!hits.empty());
                if (hits.empty()) {
                    errors.push_back(capRejected
                        ? "signature matched but displacement exceeded 0xFFFF cap; offsetKnown=false"
                        : "no x64 masked signature matched in CheckResourceResidency prologue; offsetKnown=false");
                } else if (hits.size() != 1) {
                    errors.push_back("multiple distinct signature matches (ambiguity); offsetKnown=false");
                } else {
                    matchIndex.set(hits[0].index);
                    matchCodeOffset.set(hits[0].pattern);
                    off1.set(hits[0].a);
                    off2.set(hits[0].b);
                    runtimeProofMatchedWindow = HexBytes(&spanBuf[hits[0].index], kMaxCmpSize);
                }
            }
        }
    }

    // offsetKnown only when exactly one within-cap unique match was recorded;
    // otherwise refuse all private-object dereference (no guessing, no fallback).
    const bool offsetKnown = off1.present && off2.present;

    // -----------------------------------------------------------------------
    // Step 5: Ex-object gate (read-only). *(device+off1) must equal the
    // known Ex parent; *(parent+off2) 4-byte BOOL must be TRUE.
    // -----------------------------------------------------------------------
    if (offsetKnown && exDevice && d3d9exParent) {
        uint64_t internalParent = 0;
        exKnownParentHex.set(HexPtr((uintptr_t)d3d9exParent));
        if (ReadU64LE((const uint8_t*)(const void*)exDevice + off1.value, &internalParent)) {
            exInternalParentHex.set(HexPtr((uintptr_t)internalParent));
            exParentExact.set(internalParent == (uint64_t)(uintptr_t)d3d9exParent);
            if (!exParentExact.value)
                errors.push_back("Ex internal-parent pointer does not equal this tool's own Ex parent; layout gate failed");
        } else {
            exParentExact.set(false);
            errors.push_back("Ex internal-parent read rejected by safety guards");
        }

        uint32_t b = 0;
        if (ReadU32LE((const uint8_t*)(const void*)d3d9exParent + off2.value, &b)) {
            exBoolActual.set(b);
            if (b != TRUE)
                errors.push_back("Ex parent is-d3d9ex BOOL is not TRUE (actual value recorded)");
        } else {
            errors.push_back("Ex parent BOOL read rejected by safety guards");
        }
    } else if (exDevice) {
        errors.push_back("Ex object gate skipped: offsets not proven; private-object dereference refused (no fallback offsets)");
    }

    // -----------------------------------------------------------------------
    // Step 6: plain (non-Ex) device gate. Direct3DCreate9 -> own parent ->
    // CreateDevice(0x44). Internal reads happen only after offsets are
    // proven; COM calls only against pointers already confirmed as this
    // tool's own known objects; canonical identity via QI(IID_IUnknown).
    // -----------------------------------------------------------------------
    if (pCreate9 && d3d9Module) {
        d3d9BaseParent = pCreate9(D3D_SDK_VERSION);
        d3d9BaseParentCreated = (d3d9BaseParent != nullptr);
        if (!d3d9BaseParentCreated)
            errors.push_back("Direct3DCreate9 returned null; base parent unavailable");
    }

    if (d3d9BaseParentCreated) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = 64;
        pp.BackBufferHeight = 64;
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        pp.BackBufferCount = 1;
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        pp.MultiSampleQuality = 0;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hwnd;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = FALSE;
        pp.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
        pp.Flags = 0;
        pp.FullScreen_RefreshRateInHz = 0;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        baseCreateDeviceHrAttempted = true;
        baseCreateDeviceHr =
            d3d9BaseParent->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         kBaseBehaviorFlags, &pp, &baseDevice);
        if (FAILED(baseCreateDeviceHr) || baseDevice == nullptr) {
            baseDevice = nullptr;
            errors.push_back("IDirect3D9::CreateDevice (plain) failed: " + HexHresult(baseCreateDeviceHr));
        }
    }

    if (offsetKnown && baseDevice && d3d9BaseParent) {
        uint64_t internalParent = 0;
        baseKnownParentHex.set(HexPtr((uintptr_t)d3d9BaseParent));
        const bool internalReadOk =
            ReadU64LE((const uint8_t*)(const void*)baseDevice + off1.value, &internalParent);
        if (internalReadOk)
            baseInternalParentHex.set(HexPtr((uintptr_t)internalParent));

        IDirect3D9* publicParent = nullptr;
        const HRESULT hrGD = baseDevice->GetDirect3D(&publicParent); // public method, own object
        baseGetDirect3dHrOk = SUCCEEDED(hrGD);
        if (baseGetDirect3dHrOk && publicParent)
            basePublicParentHex.set(HexPtr((uintptr_t)publicParent));
        if (!baseGetDirect3dHrOk)
            errors.push_back("plain device GetDirect3D failed: " + HexHresult(hrGD));

        // Consistency must hold BEFORE any COM call to the internally-read
        // pointer: internal == known base parent == public GetDirect3D result.
        const bool addressesAgree = internalReadOk && baseGetDirect3dHrOk && publicParent != nullptr &&
                                    internalParent == (uint64_t)(uintptr_t)d3d9BaseParent &&
                                    (uint64_t)(uintptr_t)publicParent == (uint64_t)(uintptr_t)d3d9BaseParent;

        if (!addressesAgree) {
            baseParentCanonicalIdentityMatch.set(false);
            errors.push_back("plain base-parent address consistency failed; refused COM calls on the unverified internal pointer");
            if (publicParent)
                publicParent->Release();
        } else {
            IUnknown* u1 = nullptr;
            IUnknown* u2 = nullptr;
            IUnknown* u3 = nullptr;
            const HRESULT q1 = d3d9BaseParent->QueryInterface(IID_IUnknown, (void**)&u1);
            const HRESULT q2 = publicParent->QueryInterface(IID_IUnknown, (void**)&u2);
            const HRESULT q3 =
                reinterpret_cast<IDirect3D9*>((uintptr_t)internalParent)
                    ->QueryInterface(IID_IUnknown, (void**)&u3); // verified == known parent
            const bool canonicalOk =
                SUCCEEDED(q1) && SUCCEEDED(q2) && SUCCEEDED(q3) && u1 && u2 && u3 &&
                u1 == u2 && u1 == u3;
            if (u1) u1->Release();
            if (u2) u2->Release();
            if (u3) u3->Release();
            baseParentCanonicalIdentityMatch.set(canonicalOk);
            if (!canonicalOk)
                errors.push_back("canonical COM identity (QI IUnknown) mismatch across base parent pointers");

            if (publicParent)
                publicParent->Release();

            uint32_t b = 0;
            if (ReadU32LE((const uint8_t*)(const void*)d3d9BaseParent + off2.value, &b)) {
                baseBoolActual.set(b);
                if (b != FALSE)
                    errors.push_back("plain parent is-d3d9ex BOOL is not FALSE (actual value recorded)");
            } else {
                errors.push_back("plain parent BOOL read rejected by safety guards");
            }
        }
    } else if (baseDevice) {
        errors.push_back("plain base gate skipped: offsets not proven; private-object dereference refused");
    }

    // -----------------------------------------------------------------------
    // Step 7: cleanup of this tool's own objects only.
    // -----------------------------------------------------------------------
    {
        bool cleaned = true;
        if (baseDevice) { baseDevice->Release(); baseDevice = nullptr; }
        if (d3d9BaseParent) { d3d9BaseParent->Release(); d3d9BaseParent = nullptr; }
        if (exDevice) { exDevice->Release(); exDevice = nullptr; }
        if (d3d9exParent) { d3d9exParent->Release(); d3d9exParent = nullptr; }
        if (hwnd) {
            if (!DestroyWindow(hwnd)) {
                cleaned = false;
                errors.push_back("DestroyWindow failed");
            }
            hwnd = nullptr;
        }
        if (classAtom) {
            if (UnregisterClassW(kClassName, GetModuleHandleW(nullptr)) == 0) {
                cleaned = false;
                errors.push_back("UnregisterClassW failed");
            }
            classAtom = 0;
        }
        if (d3d9Module) {
            if (!FreeLibrary(d3d9Module)) {
                cleaned = false;
                errors.push_back("FreeLibrary(d3d9.dll) failed");
            }
            d3d9Module = nullptr;
        }
        cleanupStatus = cleaned ? "complete" : "partial-failure";
    }

    const bool layoutPrototypePass =
        isX64 && sizeof(void*) == 8 &&
        !modulePathW.empty() && imageSize > 0 && !versionStr.empty() &&
        d3d9exParentCreated && exCreateDeviceHr == S_OK &&
        signatureMatched.present && signatureMatched.value && matchIndex.present &&
        offsetKnown &&
        exParentExact.present && exParentExact.value &&
        exBoolActual.present && exBoolActual.value == TRUE &&
        d3d9BaseParentCreated && baseCreateDeviceHr == S_OK &&
        baseParentCanonicalIdentityMatch.present && baseParentCanonicalIdentityMatch.value &&
        baseBoolActual.present && baseBoolActual.value == FALSE &&
        cleanupStatus == "complete" &&
        errors.empty();

    // -----------------------------------------------------------------------
    // JSON output (always valid JSON; failure paths report actual values with
    // null/unknown for gates never reached).
    // -----------------------------------------------------------------------
    JsonObj root;
    root.num("schemaVersion", 1);
    root.str("scope", "OWN_D3D9_LAYOUT_ONLY_NOT_SHARED_NOT_SIM_NOT_XR");
    root.num("selfPid", (long long)GetCurrentProcessId());
    root.str("x64Gate", isX64 && sizeof(void*) == 8 ? "pass" : "fail");

    root.str("modulePath", WideToUtf8(modulePathW));
    root.str("version", versionStr);
    root.nullableNum("imageSize", imageSize > 0, (long long)imageSize);

    JsonObj ex;
    ex.boolean("parentCreated", d3d9exParentCreated);
    ex.nullableStr("parentCreateHresult", exParentCreateHrAttempted,
                   exParentCreateHrAttempted ? HexHresult(exParentCreateHr) : std::string());
    ex.nullableStr("createDeviceHresult", d3d9exParentCreated && exCreateDeviceHrAttempted,
                   exCreateDeviceHrAttempted ? HexHresult(exCreateDeviceHr) : std::string());
    ex.str("behaviorFlags", "0x46");
    root.add("ex", "{" + ex.body + "}");

    JsonObj base;
    base.boolean("parentCreated", d3d9BaseParentCreated);
    base.nullableStr("createDeviceHresult", d3d9BaseParentCreated && baseCreateDeviceHrAttempted,
                     baseCreateDeviceHrAttempted ? HexHresult(baseCreateDeviceHr) : std::string());
    base.str("behaviorFlags", "0x44");
    root.add("base", "{" + base.body + "}");

    root.nullableBool("signatureMatched", signatureMatched.present, signatureMatched.value);
    root.nullableNum("matchIndex", matchIndex.present, (long long)matchIndex.value);
    root.nullableNum("matchCodeOffset", matchCodeOffset.present, (long long)matchCodeOffset.value);

    JsonObj proof;
    proof.str("checkResourceResidency", runtimeProofFuncStart.empty() ? "" : runtimeProofFuncStart);
    proof.str("matchedWindow22Bytes", runtimeProofMatchedWindow);
    root.add("runtimeBytesProof",
            runtimeProofFuncStart.empty() && runtimeProofMatchedWindow.empty()
                ? "null"
                : ("{" + proof.body + "}"));

    root.boolean("offsetKnown", offsetKnown);
    root.nullableNum("off1", offsetKnown ? off1.present : false, offsetKnown ? (long long)off1.value : 0);
    root.nullableNum("off2", offsetKnown ? off2.present : false, offsetKnown ? (long long)off2.value : 0);

    root.nullableBool("exParentExact", exParentExact.present, exParentExact.value);
    root.nullableStr("exKnownParentPtr", exKnownParentHex.present, exKnownParentHex.value);
    root.nullableStr("exInternalParentPtrRead", exInternalParentHex.present, exInternalParentHex.value);
    root.nullableNum("exBoolActual", exBoolActual.present, exBoolActual.present ? (long long)exBoolActual.value : 0);
    root.nullableBool("baseParentCanonicalIdentityMatch", baseParentCanonicalIdentityMatch.present,
                      baseParentCanonicalIdentityMatch.value);
    root.nullableStr("baseKnownParentPtr", baseKnownParentHex.present, baseKnownParentHex.value);
    root.nullableStr("baseInternalParentPtrRead", baseInternalParentHex.present, baseInternalParentHex.value);
    root.nullableStr("basePublicGetDirect3DPtr", basePublicParentHex.present, basePublicParentHex.value);
    root.nullableNum("baseBoolActual", baseBoolActual.present, baseBoolActual.present ? (long long)baseBoolActual.value : 0);

    root.boolean("layoutPrototypePass", layoutPrototypePass);
    root.str("featureSupport", "UNPROVEN");
    root.boolean("fpsUnknown", true);
    root.boolean("noSIMTouched", true);
    root.num("flagWrites", 0);
    root.num("bytePatches", 0);
    root.num("sharedTexturesCreated", 0);
    root.num("renderCalls", 0);

    JsonObj cleanup;
    cleanup.str("status", cleanupStatus);
    root.add("cleanup", "{" + cleanup.body + "}");

    root.add("errors", JsonObj::arrayOfStrings(errors));

    const std::string json = "{" + root.body + "}\n";

    const HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"failed to create result file (win=%lu)\n", (unsigned long)GetLastError());
        return 2;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, json.data(), (DWORD)json.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != (DWORD)json.size()) {
        fwprintf(stderr, L"failed to write result file\n");
        return 2;
    }

    return layoutPrototypePass ? 0 : 1;
}

#endif // !VRSIM_PRODUCTION_SHARED_HELPERS

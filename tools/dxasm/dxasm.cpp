// dxasm: assemble DXIL LLVM IR text (as printed by `dxc -dumpbin`) into a validated, signed DXIL container.
// Usage: dxasm <path-to-dxcompiler.dll> <in.ll> <out.dxil>
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>
#include "dxcapi.h"

static void PrintBlob(IDxcBlob* b) {
    if (b && b->GetBufferSize()) fwrite(b->GetBufferPointer(), 1, b->GetBufferSize(), stderr);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) { fwprintf(stderr, L"usage: dxasm <dxcompiler.dll> <in.ll> <out.dxil>\n"); return 2; }
    std::wstring dir(argv[1]);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    SetDllDirectoryW(dir.c_str());                       // so dxcompiler.dll finds dxil.dll next to it
    HMODULE comp = LoadLibraryW(argv[1]);
    if (!comp) { fwprintf(stderr, L"cannot load %s\n", argv[1]); return 1; }
    auto create = (DxcCreateInstanceProc)GetProcAddress(comp, "DxcCreateInstance");
    HMODULE dxil = LoadLibraryW((dir + L"\\dxil.dll").c_str());
    auto createVal = dxil ? (DxcCreateInstanceProc)GetProcAddress(dxil, "DxcCreateInstance") : create;

    IDxcLibrary* lib = nullptr; IDxcAssembler* as = nullptr; IDxcValidator* val = nullptr;
    if (FAILED(create(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void**)&lib)) ||
        FAILED(create(CLSID_DxcAssembler, __uuidof(IDxcAssembler), (void**)&as)) ||
        FAILED(createVal(CLSID_DxcValidator, __uuidof(IDxcValidator), (void**)&val))) {
        fwprintf(stderr, L"DxcCreateInstance failed\n"); return 1;
    }

    UINT32 cp = CP_UTF8;
    IDxcBlobEncoding* src = nullptr;
    if (FAILED(lib->CreateBlobFromFile(argv[2], &cp, &src))) { fwprintf(stderr, L"cannot read %s\n", argv[2]); return 1; }

    IDxcOperationResult* res = nullptr;
    HRESULT hr = as->AssembleToContainer(src, &res), status = E_FAIL;
    if (FAILED(hr) || !res) { fwprintf(stderr, L"AssembleToContainer hr=%08x\n", hr); return 1; }
    res->GetStatus(&status);
    if (FAILED(status)) {
        IDxcBlobEncoding* err = nullptr; res->GetErrorBuffer(&err); PrintBlob(err);
        fwprintf(stderr, L"\nassembly failed hr=%08x\n", status); return 1;
    }
    IDxcBlob* container = nullptr; res->GetResult(&container);

    IDxcOperationResult* vres = nullptr;
    hr = val->Validate(container, DxcValidatorFlags_InPlaceEdit, &vres);   // validates and writes the hash
    if (FAILED(hr) || !vres) { fwprintf(stderr, L"Validate hr=%08x\n", hr); return 1; }
    vres->GetStatus(&status);
    if (FAILED(status)) {
        IDxcBlobEncoding* err = nullptr; vres->GetErrorBuffer(&err); PrintBlob(err);
        fwprintf(stderr, L"\nvalidation failed hr=%08x\n", status); return 1;
    }

    FILE* f = _wfopen(argv[3], L"wb");
    if (!f) { fwprintf(stderr, L"cannot write %s\n", argv[3]); return 1; }
    fwrite(container->GetBufferPointer(), 1, container->GetBufferSize(), f);
    fclose(f);
    fwprintf(stderr, L"ok: %zu bytes, validated and signed\n", (size_t)container->GetBufferSize());
    return 0;
}

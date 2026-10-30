// Adds an import (dll!function) to a PE32+ image in memory, ahead of the
// existing imports, in a new ".rflimp" section. The Authenticode signature
// overlay and bound imports are dropped, as any edit invalidates them.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace PeImport {

constexpr char SectionName[] = ".rflimp";

inline DWORD Align(DWORD value, DWORD alignment) { return (value + alignment - 1) / alignment * alignment; }

inline IMAGE_NT_HEADERS64* Headers(std::vector<unsigned char>& file) {
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) {
        return nullptr;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }
    return nt;
}

inline bool HasImportSection(std::vector<unsigned char>& file) {
    auto* nt = Headers(file);
    if (!nt) {
        return false;
    }
    auto* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!memcmp(sections[i].Name, SectionName, sizeof(SectionName) - 1)) {
            return true;
        }
    }
    return false;
}

inline bool AddImport(std::vector<unsigned char>& file, const char* dll, const char* function) {
    auto* nt = Headers(file);
    if (!nt) {
        return false;
    }
    auto& opt = nt->OptionalHeader;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    const WORD count = nt->FileHeader.NumberOfSections;
    if (reinterpret_cast<unsigned char*>(sections + count + 1) - file.data() > static_cast<long>(opt.SizeOfHeaders)) {
        return false;
    }
    auto rvaToOffset = [&](DWORD rva) -> DWORD {
        for (WORD i = 0; i < count; ++i) {
            if (rva >= sections[i].VirtualAddress && rva < sections[i].VirtualAddress + sections[i].Misc.VirtualSize) {
                return rva - sections[i].VirtualAddress + sections[i].PointerToRawData;
            }
        }
        return 0;
    };

    DWORD rawEnd = 0;
    for (WORD i = 0; i < count; ++i) {
        rawEnd = std::max(rawEnd, sections[i].PointerToRawData + sections[i].SizeOfRawData);
    }
    auto& importDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const DWORD importOffset = rvaToOffset(importDir.VirtualAddress);
    if (!importOffset || rawEnd > file.size()) {
        return false;
    }
    std::vector<IMAGE_IMPORT_DESCRIPTOR> old;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(file.data() + importOffset);
         reinterpret_cast<unsigned char*>(d + 1) <= file.data() + file.size() && d->Name; ++d) {
        old.push_back(*d);
    }

    const auto& last = sections[count - 1];
    const DWORD rva = Align(last.VirtualAddress + last.Misc.VirtualSize, opt.SectionAlignment);
    const DWORD offset = Align(rawEnd, opt.FileAlignment);

    // Layout: descriptors | INT[2] | IAT[2] | hint/name | dll name
    const DWORD descSize = static_cast<DWORD>((old.size() + 2) * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    const DWORD intRva = rva + descSize, iatRva = intRva + 16, hintRva = iatRva + 16;
    const DWORD nameRva = hintRva + 2 + Align(static_cast<DWORD>(strlen(function) + 1), 2);
    std::vector<unsigned char> data(nameRva - rva + strlen(dll) + 1);
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(data.data());
    desc[0].OriginalFirstThunk = intRva;
    desc[0].Name = nameRva;
    desc[0].FirstThunk = iatRva;
    std::copy(old.begin(), old.end(), desc + 1);
    *reinterpret_cast<ULONGLONG*>(&data[intRva - rva]) = hintRva;
    *reinterpret_cast<ULONGLONG*>(&data[iatRva - rva]) = hintRva;
    memcpy(&data[hintRva - rva + 2], function, strlen(function) + 1);
    memcpy(&data[nameRva - rva], dll, strlen(dll) + 1);

    IMAGE_SECTION_HEADER section{};
    memcpy(section.Name, SectionName, sizeof(SectionName) - 1);
    section.Misc.VirtualSize = static_cast<DWORD>(data.size());
    section.VirtualAddress = rva;
    section.SizeOfRawData = Align(static_cast<DWORD>(data.size()), opt.FileAlignment);
    section.PointerToRawData = offset;
    // The loader writes resolved addresses into the IAT, so the section is writable.
    section.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    sections[count] = section;
    nt->FileHeader.NumberOfSections = count + 1;
    importDir.VirtualAddress = rva;
    importDir.Size = descSize;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] = {};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = {};
    opt.SizeOfImage = Align(rva + section.Misc.VirtualSize, opt.SectionAlignment);
    opt.CheckSum = 0;

    // Headers are edited in place; only now may the buffer reallocate.
    file.resize(offset, 0);
    data.resize(section.SizeOfRawData, 0);
    file.insert(file.end(), data.begin(), data.end());
    return true;
}

} // namespace PeImport

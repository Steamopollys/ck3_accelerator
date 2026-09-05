// standalone tool: open a PE on disk, print (timestamp, sha256 of first 4MB of
// .text). run against ck3.exe to fill in versions.json.

#include <picosha2.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"Usage: fingerprint_tool <path-to-pe>\n");
        return 2;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::fwprintf(stderr, L"Cannot open file\n");
        return 1;
    }

    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());

    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return 1;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 1;
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 1;

    std::uint32_t ts = nt->FileHeader.TimeDateStamp;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* text = nullptr;
    std::size_t text_size = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            text = bytes.data() + sec->PointerToRawData;
            text_size = std::min<std::size_t>(sec->SizeOfRawData, 4 * 1024 * 1024);
            break;
        }
    }
    if (!text) return 1;

    picosha2::hash256_one_by_one h;
    h.process(text, text + text_size);
    h.finish();
    std::string hex = picosha2::get_hash_hex_string(h);

    std::printf("pe_timestamp: %u\n", ts);
    std::printf("text_sha256:  %s\n", hex.c_str());
    return 0;
}

#include "MagelightFonts.h"

#include <Ultralight/Buffer.h>
#include <Ultralight/String.h>

#include <windows.h>
#include <dwrite.h>

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#pragma comment(lib, "dwrite.lib")

namespace
{
    using ultralight::Buffer;
    using ultralight::FontFile;
    using ultralight::RefPtr;
    using ultralight::String;

    // Families tried in order when the platform's own last resort (Arial) cannot be used; the
    // bundled font comes after them.
    constexpr const char* kCandidates[] = { "Segoe UI", "Tahoma", "Verdana", "Calibri", "Microsoft Sans Serif",
                                            "Times New Roman" };
    constexpr std::size_t kMaxNoted = 256;   // distinct families checked for the "cannot be loaded" warning

    std::string Utf8(const String& s)
    {
        const auto& u = s.utf8();
        return std::string(u.data() ? u.data() : "", u.length());
    }

    std::wstring Wide(const String& s)
    {
        const auto w = s.utf16();
        return std::wstring(w.data() ? w.data() : L"", w.length());
    }

    wchar_t AsciiLower(wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; }

    // CSS family names match case-insensitively; only ASCII letters fold, as in WebKit.
    bool SameFamily(const std::wstring& a, const std::wstring& b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (AsciiLower(a[i]) != AsciiLower(b[i])) return false;
        return true;
    }

    bool IsAscii(const std::wstring& s)
    {
        for (const wchar_t c : s)
            if (c >= 0x80) return false;
        return true;
    }

    std::uint32_t Be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
    std::uint16_t Be16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }

    // A TrueType / OpenType font at `at` whose table directory lies inside the data and has the tables
    // FreeType will not open a face without (cmap, hhea, hmtx, maxp, and a full head with a sane unitsPerEm).
    bool SfntAt(const std::uint8_t* d, std::size_t n, std::size_t at)
    {
        if (at > n || n - at < 12) return false;
        const std::uint32_t version = Be32(d + at);
        if (version != 0x00010000 && version != 0x74727565 /*true*/ && version != 0x4F54544F /*OTTO*/) return false;
        const std::uint16_t tables = Be16(d + at + 4);
        if (tables == 0 || (n - at - 12) / 16 < tables) return false;
        unsigned found = 0;
        for (std::uint16_t i = 0; i < tables; ++i) {
            const std::uint8_t* r = d + at + 12 + std::size_t(i) * 16;
            const std::uint32_t tag = Be32(r), off = Be32(r + 8), len = Be32(r + 12);
            if (off > n || len > n - off) return false;
            switch (tag) {
            case 0x636D6170: found |= 1; break;   // cmap
            case 0x68686561: found |= 2; break;   // hhea
            case 0x686D7478: found |= 4; break;   // hmtx
            case 0x6D617870: found |= 8; break;   // maxp
            case 0x68656164:                      // head: 54 bytes, unitsPerEm at 18
                if (len >= 54 && Be16(d + off + 18) >= 16 && Be16(d + off + 18) <= 16384) found |= 16;
                break;
            default: break;
            }
        }
        return found == 31;
    }

    // Ultralight hands the bytes to FreeType as face 0, so a collection is checked at its first font.
    bool IsLoadableFont(const RefPtr<Buffer>& b)
    {
        if (!b || !b->data() || b->size() == 0) return false;
        const auto* d = static_cast<const std::uint8_t*>(b->data());
        const std::size_t n = b->size();
        if (n >= 16 && Be32(d) == 0x74746366 /*ttcf*/) return Be32(d + 8) > 0 && SfntAt(d, n, Be32(d + 12));
        return SfntAt(d, n, 0);
    }

    // Wide-path read: Ultralight opens a font path with a narrow fopen, which decodes it in the ANSI code page.
    RefPtr<Buffer> ReadWhole(const String& path)
    {
        const std::wstring w = Wide(path);
        if (w.empty()) return nullptr;
        const HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        RefPtr<Buffer> out;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (256ll << 20)) {
            const auto n = static_cast<std::size_t>(size.QuadPart);
            if (auto* raw = static_cast<std::uint8_t*>(std::malloc(n))) {
                std::size_t got = 0;
                DWORD step = 0;
                while (got < n && ReadFile(h, raw + got, static_cast<DWORD>(n - got), &step, nullptr) && step)
                    got += step;
                if (got == n)
                    out = Buffer::Create(raw, n, nullptr, [](void*, void* data) { std::free(data); });
                else
                    std::free(raw);
            }
        }
        CloseHandle(h);
        return out;
    }

    // DejaVu Sans from this module's resources (MagelightFonts.rc). The module stays loaded for the
    // process, so the buffer points at the resource without a copy.
    RefPtr<Buffer> BundledFont()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&BundledFont), &self))
            return nullptr;
        const HRSRC res = FindResourceW(self, L"MAGELIGHT_FALLBACK_FONT", MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
        const HGLOBAL mem = res ? LoadResource(self, res) : nullptr;
        void* data = mem ? LockResource(mem) : nullptr;
        const DWORD size = res ? SizeofResource(self, res) : 0;
        if (!data || !size) return nullptr;
        return Buffer::Create(data, size, nullptr, nullptr);
    }

    // Whether the system font collection has the family at all: a page's font list names families
    // other systems have all the time, so only an installed one that fails to load is news.
    bool Installed(const String& family)
    {
        static IDWriteFontCollection* const fonts = [] {
            IDWriteFactory* factory = nullptr;
            IDWriteFontCollection* collection = nullptr;
            if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(&factory))) && factory) {
                if (FAILED(factory->GetSystemFontCollection(&collection, FALSE))) collection = nullptr;
                factory->Release();   // the collection keeps what it needs
            }
            return collection;   // kept for the process
        }();
        if (!fonts) return false;
        const std::wstring w = Wide(family);
        UINT32 index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(fonts->FindFamilyName(w.c_str(), &index, &exists)) && exists;
    }

    class SafeLoader final : public ultralight::FontLoader
    {
    public:
        SafeLoader(ultralight::FontLoader* platform, MagelightFonts::LogFn log) :
            platform_(platform), log_(log), bundledName_(String(MagelightFonts::kBundledFamily))
        {
            if (RefPtr<Buffer> b = BundledFont(); IsLoadableFont(b))
                bundled_ = FontFile::Create(b);
            else
                Log(1, "fonts: the bundled fallback font is missing from the module");

            platformLast_ = platform_->fallback_font();
            const std::string why = Problem(platformLast_);
            if (why.empty()) {
                last_ = platformLast_;
                Log(0, "fonts: last-resort font '" + Utf8(last_) + "'");
            } else {
                last_ = bundled_ ? bundledName_ : platformLast_;
                for (const char* c : kCandidates) {
                    if (Problem(String(c)).empty()) {
                        last_ = String(c);
                        break;
                    }
                }
                Log(1, "fonts: the last-resort font '" + Utf8(platformLast_) + "' cannot be used (" + why + "); '" +
                           Utf8(last_) + "' stands in for it");
            }
            platformLastW_ = Wide(platformLast_);
            lastW_ = Wide(last_);
            bundledW_ = Wide(bundledName_);
        }

        String fallback_font() const override { return last_; }

        String fallback_font_for_characters(const String& characters, int weight, bool italic) const override
        {
            // The platform loader answers its own last resort when no font has the characters.
            const String found = platform_->fallback_font_for_characters(characters, weight, italic);
            return SameFamily(Wide(found), platformLastW_) ? last_ : found;
        }

        RefPtr<FontFile> Load(const String& family, int weight, bool italic) override
        {
            const std::wstring name = Wide(family);
            if (bundled_ && SameFamily(name, bundledW_)) return bundled_;

            RefPtr<FontFile> file = platform_->Load(family, weight, italic);
            // The last-resort family is read and checked here because WebCore dereferences it unchecked;
            // any other family is read and checked here only when its path is not ASCII (see ReadWhole).
            const bool last = SameFamily(name, lastW_);
            bool readHere = false;
            if (file && !file->is_in_memory()) {
                const String path = file->filepath();
                if (last || !IsAscii(Wide(path))) {
                    const RefPtr<Buffer> bytes = ReadWhole(path);
                    file = bytes ? FontFile::Create(bytes) : nullptr;
                    readHere = true;
                }
            }
            if (file && (last || readHere) && !IsLoadableFont(file->buffer())) file = nullptr;

            if (!file) {
                NoteMiss(family);
                if (last) return bundled_;
            }
            return file;
        }

    private:
        void Log(int level, const std::string& message) const
        {
            if (log_) log_(level, message.c_str());
        }

        // Loads `family` the way Ultralight will; empty when it would load, else why not.
        std::string Problem(const String& family)
        {
            const RefPtr<FontFile> file = platform_->Load(family, 400, false);
            if (!file) return Installed(family) ? "installed, but the system cannot hand it over" : "not installed";
            const RefPtr<Buffer> bytes = file->is_in_memory() ? file->buffer() : ReadWhole(file->filepath());
            if (!bytes) return "its file cannot be read: " + Utf8(file->filepath());
            if (!IsLoadableFont(bytes)) return "its file is not a TrueType or OpenType font";
            return {};
        }

        void NoteMiss(const String& family)
        {
            std::string key = Utf8(family);
            for (auto& c : key)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            {
                std::lock_guard lock(mutex_);
                if (noted_.size() >= kMaxNoted || !noted_.insert(key).second) return;
            }
            if (Installed(family))
                Log(1, "fonts: '" + Utf8(family) + "' is installed but cannot be loaded; " +
                           (SameFamily(Wide(family), lastW_) ? std::string("the bundled font stands in for it")
                                                             : std::string("text that asks for it uses the next font in its list")));
        }

        ultralight::FontLoader* platform_;
        MagelightFonts::LogFn log_;
        RefPtr<FontFile> bundled_;
        String bundledName_, platformLast_, last_;
        std::wstring bundledW_, platformLastW_, lastW_;
        std::mutex mutex_;
        std::set<std::string> noted_;
    };
}

namespace MagelightFonts
{
    ultralight::FontLoader* CreateLoader(ultralight::FontLoader* platform, LogFn log)
    {
        if (!platform) return nullptr;
        return new SafeLoader(platform, log);
    }
}

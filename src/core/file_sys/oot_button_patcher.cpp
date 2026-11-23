// SPDX-FileCopyrightText: Copyright 2026 FritCheeLii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <zstd.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "core/file_sys/oot_button_patch_data.h"
#include "core/file_sys/oot_button_patcher.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {
namespace {

struct SarcSlice {
    u32 offset{};
    u32 size{};
};

struct ZstdDDictDeleter {
    void operator()(ZSTD_DDict* d) const noexcept {
        if (d) {
            ZSTD_freeDDict(d);
        }
    }
};

struct ZstdDCtxDeleter {
    void operator()(ZSTD_DCtx* c) const noexcept {
        if (c) {
            ZSTD_freeDCtx(c);
        }
    }
};

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

u16 ReadU16LE(std::span<const u8> buf, size_t off) {
    return static_cast<u16>(buf[off]) | (static_cast<u16>(buf[off + 1]) << 8);
}

u32 ReadU32LE(std::span<const u8> buf, size_t off) {
    return static_cast<u32>(buf[off]) | (static_cast<u32>(buf[off + 1]) << 8) |
           (static_cast<u32>(buf[off + 2]) << 16) | (static_cast<u32>(buf[off + 3]) << 24);
}

bool ParseSarcEntries(std::span<const u8> buf,
                      std::unordered_map<std::string, std::vector<SarcSlice>>& out) {
    if (buf.size() < 32 || std::memcmp(buf.data(), "SARC", 4) != 0) {
        return false;
    }
    const u16 bom = ReadU16LE(buf, 6);
    if (bom != 0xFEFF) {
        return false;
    }
    const u32 data_off = ReadU32LE(buf, 12);
    if (std::memcmp(buf.data() + 20, "SFAT", 4) != 0) {
        return false;
    }
    const u16 node_count = ReadU16LE(buf, 26);
    const size_t sfnt_off = 32 + static_cast<size_t>(node_count) * 16;
    if (buf.size() < sfnt_off + 8 || std::memcmp(buf.data() + sfnt_off, "SFNT", 4) != 0) {
        return false;
    }
    const size_t names_base = sfnt_off + 8;

    out.clear();
    out.reserve(node_count);
    for (u16 i = 0; i < node_count; ++i) {
        const size_t node_off = 32 + static_cast<size_t>(i) * 16;
        const u32 flags_off = ReadU32LE(buf, node_off + 4);
        const u32 start = ReadU32LE(buf, node_off + 8);
        const u32 end = ReadU32LE(buf, node_off + 12);
        const size_t name_off = names_base + static_cast<size_t>(flags_off & 0xFFFFFFU) * 4;
        if (name_off >= buf.size() || end < start ||
            static_cast<u64>(data_off) + end > buf.size()) {
            return false;
        }
        const auto* nul = static_cast<const u8*>(
            std::memchr(buf.data() + name_off, '\0', buf.size() - name_off));
        if (!nul) {
            return false;
        }
        std::string name(reinterpret_cast<const char*>(buf.data() + name_off),
                         static_cast<size_t>(nul - (buf.data() + name_off)));
        out[std::move(name)].push_back(SarcSlice{data_off + start, end - start});
    }
    return true;
}

bool DecompressZstd(std::span<const u8> src, ZSTD_DDict* dict, std::vector<u8>& out) {
    std::unique_ptr<ZSTD_DCtx, ZstdDCtxDeleter> dctx(ZSTD_createDCtx());
    if (!dctx) {
        return false;
    }
    if (dict) {
        if (ZSTD_isError(ZSTD_DCtx_refDDict(dctx.get(), dict))) {
            return false;
        }
    }

    const unsigned long long content_size = ZSTD_getFrameContentSize(src.data(), src.size());
    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size != ZSTD_CONTENTSIZE_ERROR &&
        content_size <= (512ULL << 20)) {
        out.resize(static_cast<size_t>(content_size));
        const size_t res = ZSTD_decompressDCtx(dctx.get(), out.data(), out.size(),
                                               src.data(), src.size());
        if (ZSTD_isError(res)) {
            out.clear();
            return false;
        }
        out.resize(res);
        return true;
    }

    // Streaming fallback if frame content size is omitted.
    out.clear();
    ZSTD_inBuffer input{src.data(), src.size(), 0};
    std::vector<u8> chunk(1024 * 1024);
    while (input.pos < input.size) {
        ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
        const size_t ret = ZSTD_decompressStream(dctx.get(), &output, &input);
        if (ZSTD_isError(ret)) {
            out.clear();
            return false;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(output.pos));
        if (ret == 0) {
            break;
        }
    }
    return !out.empty();
}

bool CompressZstd(std::span<const u8> src, int level, std::vector<u8>& out) {
    const size_t bound = ZSTD_compressBound(src.size());
    out.resize(bound);
    const size_t written = ZSTD_compress(out.data(), out.size(), src.data(), src.size(), level);
    if (ZSTD_isError(written)) {
        out.clear();
        return false;
    }
    out.resize(written);
    return true;
}

std::vector<u8> ReadVfsPath(const VirtualDir& root, std::initializer_list<std::string_view> parts) {
    if (!root || parts.size() == 0) {
        return {};
    }
    VirtualDir dir = root;
    auto it = parts.begin();
    const auto last = std::prev(parts.end());
    for (; it != last; ++it) {
        VirtualDir next;
        for (const auto& sub : dir->GetSubdirectories()) {
            if (EqualsIgnoreCaseAscii(sub->GetName(), *it)) {
                next = sub;
                break;
            }
        }
        if (!next) {
            return {};
        }
        dir = std::move(next);
    }
    for (const auto& file : dir->GetFiles()) {
        if (EqualsIgnoreCaseAscii(file->GetName(), *last)) {
            return file->ReadAllBytes();
        }
    }
    return {};
}

bool FileExistsNonEmpty(const std::filesystem::path& p) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    return !ec && sz > 0;
}

bool WriteFileAtomic(const std::filesystem::path& path, std::span<const u8> data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return false;
        }
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!ofs) {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

bool EnsureOoTButtonIconsGenerated(const VirtualDir& romfs_dir,
                                   const std::filesystem::path& style_output_dir, int style_idx) {
    if (style_idx < 1 || style_idx > 7) {
        return false;
    }

    const std::filesystem::path bntx_out =
        style_output_dir / "UI" / "Layout2Archive" / "Common.Product.100.Nin_Ounce_NVN2.bntx.zs";
    const std::filesystem::path font_out =
        style_output_dir / "Font2" / "Scalable" / "External.Nin_Ounce_NVN2.bfotf.zs";
    const std::filesystem::path rsize_out = style_output_dir / "System" / "Resource" /
                                            "ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs";
    const std::filesystem::path blarc_out =
        style_output_dir / "UI" / "Layout2Archive" / "Common.Product.100.Nin_Ounce_NVN2.blarc.zs";

    const bool need_bntx = !FileExistsNonEmpty(bntx_out);
    const bool need_font = !FileExistsNonEmpty(font_out);
    const bool need_rsize = !FileExistsNonEmpty(rsize_out);
    const bool need_blarc = (style_idx >= 5) && (!FileExistsNonEmpty(blarc_out) ||
                                                !FileExistsNonEmpty(style_output_dir / ".patch_v2"));

    if (!need_bntx && !need_font && !need_rsize && !need_blarc) {
        return true;
    }
    if (!romfs_dir) {
        return false;
    }

    LOG_INFO(Loader, "Generating OoT button icon/layout patch (style={}) from user RomFS...",
             style_idx);

    const auto zsdic_zs = ReadVfsPath(romfs_dir, {"Pack", "ZsDic.pack.zs"});
    if (zsdic_zs.empty()) {
        LOG_WARNING(Loader, "OoT button patcher: Pack/ZsDic.pack.zs not found in RomFS");
        return false;
    }
    std::vector<u8> zsdic_sarc;
    if (!DecompressZstd(zsdic_zs, nullptr, zsdic_sarc)) {
        LOG_WARNING(Loader, "OoT button patcher: failed to decompress ZsDic.pack.zs");
        return false;
    }
    std::unordered_map<std::string, std::vector<SarcSlice>> zsdic_entries;
    if (!ParseSarcEntries(zsdic_sarc, zsdic_entries)) {
        LOG_WARNING(Loader, "OoT button patcher: failed to parse ZsDic.pack SARC");
        return false;
    }
    const auto zs_it = zsdic_entries.find("zs.zsdic");
    if (zs_it == zsdic_entries.end() || zs_it->second.empty()) {
        LOG_WARNING(Loader, "OoT button patcher: zs.zsdic missing in ZsDic.pack");
        return false;
    }
    std::unique_ptr<ZSTD_DDict, ZstdDDictDeleter> zs_dict(
        ZSTD_createDDict(zsdic_sarc.data() + zs_it->second.front().offset,
                         zs_it->second.front().size));
    if (!zs_dict) {
        LOG_WARNING(Loader, "OoT button patcher: ZSTD_createDDict failed");
        return false;
    }

    std::vector<u8> raw;
    std::vector<u8> compressed;

    if (need_bntx) {
        const auto bntx_zs = ReadVfsPath(
            romfs_dir, {"UI", "Layout2Archive", "Common.Product.100.Nin_Ounce_NVN2.bntx.zs"});
        if (bntx_zs.empty() || !DecompressZstd(bntx_zs, zs_dict.get(), raw) ||
            raw.size() != OoTButtonPatchData::ExpectedBntxSize) {
            LOG_WARNING(Loader, "OoT button patcher: unexpected Common.bntx.zs size ({})",
                        raw.size());
            return false;
        }
        std::vector<u8> patch;
        if (!DecompressZstd(OoTButtonPatchData::GetBntxPatch(style_idx), nullptr, patch) ||
            patch.size() < 4) {
            return false;
        }
        const u32 num_chunks = ReadU32LE(patch, 0);
        size_t pos = 4;
        for (u32 c = 0; c < num_chunks; ++c) {
            if (pos + 8 > patch.size()) {
                return false;
            }
            const u32 off = ReadU32LE(patch, pos);
            const u32 clen = ReadU32LE(patch, pos + 4);
            pos += 8;
            if (pos + clen > patch.size() || static_cast<u64>(off) + clen > raw.size()) {
                return false;
            }
            for (u32 k = 0; k < clen; ++k) {
                raw[off + k] ^= patch[pos + k];
            }
            pos += clen;
        }
        if (!CompressZstd(raw, 1, compressed) || !WriteFileAtomic(bntx_out, compressed)) {
            LOG_WARNING(Loader, "OoT button patcher: failed to write Common.bntx.zs");
            return false;
        }
    }

    if (need_font) {
        const auto font_zs =
            ReadVfsPath(romfs_dir, {"Font2", "Scalable", "External.Nin_Ounce_NVN2.bfotf.zs"});
        if (font_zs.empty() || !DecompressZstd(font_zs, zs_dict.get(), raw) ||
            raw.size() != OoTButtonPatchData::ExpectedFontSize) {
            LOG_WARNING(Loader, "OoT button patcher: unexpected External.bfotf.zs size ({})",
                        raw.size());
            return false;
        }
        std::vector<u8> patch;
        if (!DecompressZstd(OoTButtonPatchData::GetFontPatch(style_idx), nullptr, patch) ||
            patch.size() < 4) {
            return false;
        }
        const u32 target_size = ReadU32LE(patch, 0);
        if (patch.size() != 4 + static_cast<size_t>(target_size)) {
            return false;
        }
        raw.resize(target_size, 0);
        for (u32 i = 0; i < target_size; ++i) {
            raw[i] ^= patch[4 + i];
        }
        if (!CompressZstd(raw, 1, compressed) || !WriteFileAtomic(font_out, compressed)) {
            LOG_WARNING(Loader, "OoT button patcher: failed to write External.bfotf.zs");
            return false;
        }
    }

    if (need_rsize) {
        const auto rsize_zs = ReadVfsPath(
            romfs_dir,
            {"System", "Resource", "ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs"});
        if (rsize_zs.empty() || !DecompressZstd(rsize_zs, zs_dict.get(), raw) ||
            raw.size() != OoTButtonPatchData::ExpectedRsizeSize) {
            LOG_WARNING(Loader, "OoT button patcher: unexpected rsizetable.zs size ({})",
                        raw.size());
            return false;
        }
        if (raw[OoTButtonPatchData::RsizePatchOffset] == OoTButtonPatchData::RsizeExpected[0] &&
            raw[OoTButtonPatchData::RsizePatchOffset + 1] ==
                OoTButtonPatchData::RsizeExpected[1]) {
            raw[OoTButtonPatchData::RsizePatchOffset] = OoTButtonPatchData::RsizePatched[0];
            raw[OoTButtonPatchData::RsizePatchOffset + 1] = OoTButtonPatchData::RsizePatched[1];
        }
        if (!CompressZstd(raw, 1, compressed) || !WriteFileAtomic(rsize_out, compressed)) {
            LOG_WARNING(Loader, "OoT button patcher: failed to write rsizetable.zs");
            return false;
        }
    }

    if (need_blarc) {
        const auto blarc_zs = ReadVfsPath(
            romfs_dir, {"UI", "Layout2Archive", "Common.Product.100.Nin_Ounce_NVN2.blarc.zs"});
        if (blarc_zs.empty() || !DecompressZstd(blarc_zs, zs_dict.get(), raw)) {
            LOG_WARNING(Loader, "OoT button patcher: failed to decompress Common.blarc.zs");
            return false;
        }
        std::unordered_map<std::string, std::vector<SarcSlice>> blarc_entries;
        if (!ParseSarcEntries(raw, blarc_entries)) {
            LOG_WARNING(Loader, "OoT button patcher: failed to parse Common.blarc SARC");
            return false;
        }
        for (const auto& p : OoTButtonPatchData::BlarcPatches) {
            const auto it = blarc_entries.find(p.entry_name);
            if (it == blarc_entries.end() || it->second.empty()) {
                LOG_WARNING(Loader, "OoT button patcher: missing blarc entry {}", p.entry_name);
                return false;
            }
            for (const auto& slice : it->second) {
                if (p.offset >= slice.size) {
                    LOG_WARNING(Loader, "OoT button patcher: offset out of range in {}",
                                p.entry_name);
                    return false;
                }
                const size_t abs_off = static_cast<size_t>(slice.offset) + p.offset;
                if (raw[abs_off] != p.expected) {
                    LOG_WARNING(Loader, "OoT button patcher: unexpected byte in {} at {:#x}",
                                p.entry_name, p.offset);
                    return false;
                }
                raw[abs_off] = p.value;
            }
        }
        if (!CompressZstd(raw, 1, compressed) || !WriteFileAtomic(blarc_out, compressed)) {
            LOG_WARNING(Loader, "OoT button patcher: failed to write Common.blarc.zs");
            return false;
        }
        const std::array<u8, 2> marker{'v', '2'};
        WriteFileAtomic(style_output_dir / ".patch_v2", marker);
    }

    LOG_INFO(Loader, "OoT button icon/layout patch (style={}) generated successfully.", style_idx);
    return true;
}

} // namespace FileSys

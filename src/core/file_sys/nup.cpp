// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include "common/logging.h"
#include "common/zstd_compression.h"
#include "core/file_sys/nup.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {

namespace {

constexpr std::array<u8, 4> NUP_MAGIC = {'N', 'U', 'P', 0x01};
constexpr std::array<u8, 4> TOC_MAGIC = {'T', 'O', 'C', 0x01};
constexpr u16 SUPERBLOCK_SIZE = 0x80;
constexpr u32 PACKAGE_RECORD_SIZE = 0x40;
constexpr u32 DIR_RECORD_SIZE = 0x18;
constexpr u32 FILE_RECORD_SIZE = 0x50;
constexpr u32 INVALID_DIR = 0xFFFFFFFF;

u16 ReadLE16(const u8* data) {
    return static_cast<u16>(data[0]) | (static_cast<u16>(data[1]) << 8);
}

u32 ReadLE32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24);
}

u64 ReadLE64(const u8* data) {
    return static_cast<u64>(ReadLE32(data)) | (static_cast<u64>(ReadLE32(data + 4)) << 32);
}

// VfsFile that decompresses a zstd extent from the container on first read
class NupZstdFile : public VfsFile {
public:
    NupZstdFile(VirtualFile container, u64 extent_offset, u64 extent_size,
                u64 uncompressed_size, std::string name)
        : container_(std::move(container)), extent_offset_(extent_offset),
          extent_size_(extent_size), uncompressed_size_(uncompressed_size),
          name_(std::move(name)) {}

    std::string GetName() const override { return name_; }
    std::size_t GetSize() const override { return uncompressed_size_; }
    bool Resize(std::size_t) override { return false; }
    VirtualDir GetContainingDirectory() const override { return nullptr; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (length == 0) {
            return 0;
        }
        const auto& decompressed = Decompress();
        if (decompressed.empty() || offset >= decompressed.size()) {
            return 0;
        }
        const auto read_size = std::min(length, decompressed.size() - offset);
        std::memcpy(data, decompressed.data() + offset, read_size);
        return read_size;
    }

    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view new_name) override {
        name_ = new_name;
        return true;
    }

private:
    const std::vector<u8>& Decompress() const {
        std::call_once(decompression_once_, [this] { DecompressOnce(); });
        return decompressed_;
    }

    void DecompressOnce() const {
        constexpr u64 MAX_DECOMPRESSED_FILE_SIZE = 512ULL * 1024 * 1024;

        if (extent_size_ == 0 || uncompressed_size_ == 0 ||
            uncompressed_size_ > MAX_DECOMPRESSED_FILE_SIZE ||
            uncompressed_size_ > std::numeric_limits<std::size_t>::max()) {
            LOG_ERROR(Loader, "NUP: invalid or excessive decompressed size for {}", name_);
            return;
        }

        auto compressed = container_->ReadBytes(
            static_cast<std::size_t>(extent_size_), static_cast<std::size_t>(extent_offset_));
        if (compressed.size() != extent_size_) {
            LOG_ERROR(Loader, "NUP: incomplete compressed extent for {}", name_);
            return;
        }

        decompressed_.resize(static_cast<std::size_t>(uncompressed_size_));
        std::string error;
        const auto written = Common::Compression::DecompressDataZSTD(
            std::span<const u8>(compressed), std::span<u8>(decompressed_), &error);

        if (written != decompressed_.size()) {
            LOG_ERROR(Loader,
                      "NUP: unexpected decompressed size for {}: expected {}, got {}, "
                      "compressed extent offset {:#x}, size {}, zstd error: {}",
                      name_, uncompressed_size_, written, extent_offset_, extent_size_, error);
            decompressed_.clear();
        }
    }

    VirtualFile container_;
    u64 extent_offset_;
    u64 extent_size_;
    u64 uncompressed_size_;
    std::string name_;
    mutable std::vector<u8> decompressed_;
    mutable std::once_flag decompression_once_;
};

} // namespace

NUP::NUP(VirtualFile file) : nup_file(std::move(file)) {
    if (nup_file == nullptr) {
        status = Loader::ResultStatus::ErrorNullFile;
        return;
    }
    if (!Parse()) {
        if (status == Loader::ResultStatus::ErrorNotInitialized) {
            status = Loader::ResultStatus::ErrorBadPFSHeader;
        }
    }
}

NUP::~NUP() = default;

Loader::ResultStatus NUP::GetStatus() const {
    return status;
}

const std::vector<NupPackageInfo>& NUP::GetPackages() const {
    return packages;
}

const NupPackageInfo* NUP::GetBasePackage() const {
    for (const auto& pkg : packages) {
        if (pkg.kind == NupPackageKind::Base) {
            return &pkg;
        }
    }
    return nullptr;
}

std::vector<const NupPackageInfo*> NUP::GetUpdatePackages() const {
    std::vector<const NupPackageInfo*> result;
    for (const auto& pkg : packages) {
        if (pkg.kind == NupPackageKind::Update) {
            result.push_back(&pkg);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const auto* a, const auto* b) { return a->version < b->version; });
    return result;
}

std::vector<const NupPackageInfo*> NUP::GetAddonPackages() const {
    std::vector<const NupPackageInfo*> result;
    for (const auto& pkg : packages) {
        if (pkg.kind == NupPackageKind::Addon) {
            result.push_back(&pkg);
        }
    }
    return result;
}

bool NUP::Parse() {
    // Read superblock
    std::array<u8, SUPERBLOCK_SIZE> superblock{};
    if (nup_file->Read(superblock.data(), SUPERBLOCK_SIZE) != SUPERBLOCK_SIZE) {
        LOG_ERROR(Loader, "NUP: failed to read superblock");
        return false;
    }

    // Validate magic
    if (std::memcmp(superblock.data(), NUP_MAGIC.data(), 4) != 0) {
        return false;
    }

    const u16 header_size = ReadLE16(superblock.data() + 0x04);
    if (header_size < SUPERBLOCK_SIZE) {
        LOG_ERROR(Loader, "NUP: invalid header_size {:#x}", header_size);
        return false;
    }

    toc_offset = ReadLE64(superblock.data() + 0x08);
    const u64 toc_size = ReadLE64(superblock.data() + 0x10);
    blob_offset = ReadLE64(superblock.data() + 0x38);
    const u64 file_size = ReadLE64(superblock.data() + 0x40);
    container_size = nup_file->GetSize();
    package_count = ReadLE32(superblock.data() + 0x48);
    const u32 flags = ReadLE32(superblock.data() + 0x4C);

    if (package_count == 0) {
        LOG_ERROR(Loader, "NUP: no packages");
        return false;
    }

    if (flags != 0) {
        LOG_ERROR(Loader, "NUP: unknown flags {:#x}", flags);
        return false;
    }

    if (file_size != container_size ||
        toc_offset > blob_offset ||
        toc_size > blob_offset - toc_offset ||
        blob_offset > file_size) {
        LOG_ERROR(Loader, "NUP: invalid layout");
        return false;
    }

    // Read TOC
    toc_data.resize(toc_size);
    if (nup_file->Read(toc_data.data(), toc_size, toc_offset) != toc_size) {
        LOG_ERROR(Loader, "NUP: failed to read TOC");
        return false;
    }

    // Validate TOC header
    if (toc_size < 0x30 || std::memcmp(toc_data.data(), TOC_MAGIC.data(), 4) != 0) {
        LOG_ERROR(Loader, "NUP: invalid TOC magic");
        return false;
    }

    const u32 toc_package_count = ReadLE32(toc_data.data() + 0x04);
    if (toc_package_count != package_count) {
        LOG_ERROR(Loader, "NUP: package count mismatch");
        return false;
    }

    dir_count = ReadLE64(toc_data.data() + 0x08);
    file_count = ReadLE64(toc_data.data() + 0x10);
    string_pool_off = ReadLE64(toc_data.data() + 0x18);
    string_pool_size = ReadLE64(toc_data.data() + 0x20);

    // Calculate section offsets with overflow-safe bounds checks.
    const u64 toc_size_u64 = static_cast<u64>(toc_data.size());
    packages_off = 0x30;

    if (package_count > (toc_size_u64 - packages_off) / PACKAGE_RECORD_SIZE) {
        LOG_ERROR(Loader, "NUP: package records out of bounds");
        return false;
    }
    dirs_off = packages_off + static_cast<u64>(package_count) * PACKAGE_RECORD_SIZE;

    if (dir_count > (toc_size_u64 - dirs_off) / DIR_RECORD_SIZE) {
        LOG_ERROR(Loader, "NUP: directory records out of bounds");
        return false;
    }
    files_off = dirs_off + dir_count * DIR_RECORD_SIZE;

    if (file_count > (toc_size_u64 - files_off) / FILE_RECORD_SIZE) {
        LOG_ERROR(Loader, "NUP: file records out of bounds");
        return false;
    }
    children_off = files_off + file_count * FILE_RECORD_SIZE;
    // Validate child references and their aggregate size.
    u64 total_children = 0;
    for (u64 i = 0; i < dir_count; ++i) {
        const u64 dir_off = dirs_off + i * DIR_RECORD_SIZE;
        const u64 count = ReadLE32(toc_data.data() + dir_off + 0x0C);
        if (count > (toc_size_u64 - children_off) / 4 ||
            total_children > (toc_size_u64 - children_off) / 4 - count) {
            LOG_ERROR(Loader, "NUP: child references out of bounds");
            return false;
        }
        total_children += count;
    }

    children_count = total_children;

    const u64 children_end = children_off + total_children * 4;
    if (string_pool_off > toc_size_u64 ||
        string_pool_size > toc_size_u64 - string_pool_off ||
        children_end > string_pool_off) {
        LOG_ERROR(Loader, "NUP: TOC sections out of bounds or overlapping");
        return false;
    }

    // Parse packages
    packages.resize(package_count);
    for (u32 i = 0; i < package_count; ++i) {
        const u64 pkg_off = packages_off + static_cast<u64>(i) * PACKAGE_RECORD_SIZE;
        const u8* p = toc_data.data() + pkg_off;

        auto& pkg = packages[i];
        pkg.title_id = ReadLE64(p + 0x00);
        const u32 root_dir_idx = ReadLE32(p + 0x08);
        const u32 exefs_dir_idx = ReadLE32(p + 0x0C);
        const u32 romfs_dir_idx = ReadLE32(p + 0x10);
        pkg.version = ReadLE32(p + 0x14);
        pkg.kind = static_cast<NupPackageKind>(p[0x18]);
        pkg.standalone_update = (p[0x19] & 0x01) != 0;

        const u64 meta_offset = ReadLE64(p + 0x20);
        const u32 meta_size = ReadLE32(p + 0x28);

        if (static_cast<u8>(pkg.kind) > 4) {
            LOG_ERROR(Loader, "NUP: unknown package kind {}", static_cast<u8>(pkg.kind));
            return false;
        }

        // Build each directory tree with independent cycle-detection state.
        const auto build_tree = [this](u32 index) -> VirtualDir {
            std::vector<u8> visited(static_cast<std::size_t>(dir_count), 0);
            return BuildDirectoryTree(index, visited);
        };
        if (root_dir_idx < dir_count) {
            pkg.root_dir = build_tree(root_dir_idx);
        }
        if (exefs_dir_idx != INVALID_DIR && exefs_dir_idx < dir_count) {
            pkg.exefs_dir = build_tree(exefs_dir_idx);
        }
        if (romfs_dir_idx != INVALID_DIR && romfs_dir_idx < dir_count) {
            pkg.romfs_dir = build_tree(romfs_dir_idx);
        }

        // Parse meta TLVs
        if (meta_offset > 0 && meta_size > 0) {
            ParseMetaTLVs(pkg, meta_offset, meta_size);
        }

        LOG_INFO(Loader, "NUP: package {} title_id={:016X} kind={} version={}",
                 i, pkg.title_id, static_cast<u8>(pkg.kind), pkg.version);
    }

    status = Loader::ResultStatus::Success;
    return true;
}

VirtualDir NUP::BuildDirectoryTree(u32 dir_index, std::vector<u8>& visited, u32 depth) {
    constexpr u32 MAX_DIRECTORY_DEPTH = 256;
    if (dir_index >= dir_count || dir_index >= visited.size() ||
        visited[dir_index] != 0 || depth >= MAX_DIRECTORY_DEPTH) {
        return nullptr;
    }
    visited[dir_index] = 1;

    const u64 dir_off = dirs_off + static_cast<u64>(dir_index) * DIR_RECORD_SIZE;
    const u8* d = toc_data.data() + dir_off;

    const u32 name_off = ReadLE32(d + 0x00);
    const u32 name_len = ReadLE32(d + 0x04);
    const u32 first_child = ReadLE32(d + 0x08);
    const u32 child_count = ReadLE32(d + 0x0C);

    if (static_cast<u64>(first_child) > children_count ||
        static_cast<u64>(child_count) > children_count - first_child) {
        LOG_ERROR(Loader, "NUP: directory child range out of bounds");
        return nullptr;
    }

    const std::string dir_name = ReadString(name_off, name_len);

    std::vector<VirtualFile> child_files;
    std::vector<VirtualDir> child_dirs;

    for (u32 c = 0; c < child_count; ++c) {
        const u64 child_slot = children_off + (static_cast<u64>(first_child) + c) * 4;
        if (child_slot + 4 > toc_data.size()) {
            break;
        }
        const u32 node_id = ReadLE32(toc_data.data() + child_slot);

        if (node_id < dir_count) {
            // Child is a directory
            auto child_dir = BuildDirectoryTree(node_id, visited, depth + 1);
            if (child_dir) {
                child_dirs.push_back(std::move(child_dir));
            }
        } else {
            // Child is a file
            const u64 file_index = static_cast<u64>(node_id) - dir_count;
            if (file_index >= file_count) {
                continue;
            }

            const u64 file_off = files_off + file_index * FILE_RECORD_SIZE;
            const u8* f = toc_data.data() + file_off;

            const u32 f_name_off = ReadLE32(f + 0x00);
            const u32 f_name_len = ReadLE32(f + 0x04);
            const u64 f_size = ReadLE64(f + 0x08);
            const u64 f_extent_offset = ReadLE64(f + 0x10);
            const u64 f_extent_size = ReadLE64(f + 0x18);
            const u8 f_codec = f[0x40];

            const std::string file_name = ReadString(f_name_off, f_name_len);
            auto vfs_file = MakeFileView(f_extent_offset, f_extent_size, f_size,
                                         f_codec, file_name);
            if (vfs_file) {
                child_files.push_back(std::move(vfs_file));
            }
        }
    }

    return std::make_shared<VectorVfsDirectory>(std::move(child_files), std::move(child_dirs),
                                                 dir_name);
}

VirtualFile NUP::MakeFileView(u64 extent_offset, u64 extent_size, u64 uncompressed_size,
                               u8 codec, const std::string& name) {
    if (uncompressed_size == 0) {
        if (extent_size != 0 || extent_offset > container_size) {
            return nullptr;
        }
        return std::make_shared<VectorVfsFile>(std::vector<u8>{}, name);
    }

    if (extent_offset < blob_offset ||
        extent_offset > container_size ||
        extent_size > container_size - extent_offset ||
        extent_size == 0 ||
        uncompressed_size > std::numeric_limits<std::size_t>::max()) {
        LOG_ERROR(Loader, "NUP: invalid file extent for {}", name);
        return nullptr;
    }

    if (codec == 0 && extent_size != uncompressed_size) {
        LOG_ERROR(Loader, "NUP: raw extent size mismatch for {}", name);
        return nullptr;
    }

    switch (codec) {
    case 0: // raw
        return std::make_shared<OffsetVfsFile>(nup_file, extent_size, extent_offset, name);
    case 1: // zstd
        return std::make_shared<NupZstdFile>(nup_file, extent_offset, extent_size,
                                             uncompressed_size, name);
    default:
        LOG_ERROR(Loader, "NUP: unsupported codec {} for file {}", codec, name);
        return nullptr;
    }
}

void NUP::ParseMetaTLVs(NupPackageInfo& pkg, u64 meta_offset, u32 meta_size) {
    constexpr u32 MAX_METADATA_SIZE = 16 * 1024 * 1024;

    if (meta_size == 0 || meta_size > MAX_METADATA_SIZE ||
        meta_offset < blob_offset || meta_offset > container_size ||
        meta_size > container_size - meta_offset) {
        LOG_ERROR(Loader, "NUP: invalid metadata extent");
        return;
    }

    std::vector<u8> meta(meta_size);
    if (nup_file->Read(meta.data(), meta_size, meta_offset) != meta_size) {
        return;
    }

    u32 pos = 0;
    while (pos <= meta_size && meta_size - pos >= 8) {
        const u16 type = ReadLE16(meta.data() + pos);
        const u32 payload_size = ReadLE32(meta.data() + pos + 4);
        pos += 8;

        if (payload_size > meta_size - pos) {
            LOG_ERROR(Loader, "NUP: metadata TLV payload out of bounds");
            break;
        }

        const u8* payload = meta.data() + pos;

        switch (type) {
        case 0x0001: // NPDM
            pkg.npdm_data.assign(payload, payload + payload_size);
            break;
        case 0x0002: // NACP
            pkg.nacp_data.assign(payload, payload + payload_size);
            break;
        case 0x0003: // Icon
            pkg.icon_data.assign(payload, payload + payload_size);
            break;
        case 0x0004: // Display version
            pkg.display_version.assign(reinterpret_cast<const char*>(payload), payload_size);
            break;
        case 0x0005: // Title name
            pkg.title_name.assign(reinterpret_cast<const char*>(payload), payload_size);
            break;
        case 0x0006: // Publisher
            pkg.publisher.assign(reinterpret_cast<const char*>(payload), payload_size);
            break;
        default:
            break;
        }

        pos += payload_size;
    }
}

std::string NUP::ReadString(u32 offset, u32 length) const {
    const u64 toc_size = static_cast<u64>(toc_data.size());
    if (string_pool_off > toc_size || offset > string_pool_size ||
        length > string_pool_size - offset ||
        offset > toc_size - string_pool_off ||
        length > toc_size - string_pool_off - offset) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(toc_data.data() + string_pool_off + offset),
                       length);
}

} // namespace FileSys

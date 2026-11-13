// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nup.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nup.h"

namespace Loader {

AppLoader_Nup::AppLoader_Nup(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)) {

    nup = std::make_unique<FileSys::NUP>(file);
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto* base = nup->GetBasePackage();
    if (base == nullptr) {
        return;
    }

    title_id = base->title_id;
    title_name = base->title_name;
    publisher = base->publisher;
    display_version = base->display_version;
    icon_data = base->icon_data;
    nacp_data = base->nacp_data;

    // Collect only updates for this exact title. Newer versions have higher priority.
    auto updates = nup->GetUpdatePackages();
    std::stable_sort(updates.begin(), updates.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->version > rhs->version;
    });

    std::vector<FileSys::VirtualDir> exefs_layers;
    std::vector<FileSys::VirtualDir> romfs_layers;

    for (const auto* update : updates) {
        if (update == nullptr || update->title_id != base->title_id ||
            update->kind != FileSys::NupPackageKind::Update || !update->exefs_dir) {
            continue;
        }

        // Validate update ExeFS before allowing it to override the base executable.
        const auto main_file = update->exefs_dir->GetFile("main");
        const auto npdm_file = update->exefs_dir->GetFile("main.npdm");
        if (!main_file || !npdm_file) {
            LOG_WARNING(Loader, "Skipping NUP update v{}: incomplete ExeFS", update->version);
            continue;
        }

        FileSys::ProgramMetadata update_metadata;
        if (update_metadata.Load(npdm_file) != ResultStatus::Success ||
            update_metadata.GetTitleID() != base->title_id) {
            LOG_WARNING(Loader, "Skipping NUP update v{}: invalid or mismatched NPDM",
                        update->version);
            continue;
        }

        exefs_layers.push_back(update->exefs_dir);
        if (update->romfs_dir) {
            romfs_layers.push_back(update->romfs_dir);
        }
        LOG_INFO(Loader, "Accepted NUP update v{} for title {:016X}", update->version,
                 base->title_id);
    }

    // Layer newest update first, then older updates, then the base package.
    if (base->exefs_dir) {
        exefs_layers.push_back(base->exefs_dir);
        auto merged_exefs = FileSys::LayeredVfsDirectory::MakeLayeredDirectory(
            std::move(exefs_layers), "nup_exefs");
        secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(
            std::move(merged_exefs), false);
    }

    if (base->romfs_dir) {
        romfs_layers.push_back(base->romfs_dir);
    }
    if (!romfs_layers.empty()) {
        auto merged_romfs = FileSys::LayeredVfsDirectory::MakeLayeredDirectory(
            std::move(romfs_layers), "nup_romfs");
        romfs = FileSys::CreateRomFS(merged_romfs);
    }
}

AppLoader_Nup::~AppLoader_Nup() = default;

FileType AppLoader_Nup::IdentifyType(const FileSys::VirtualFile& nup_file) {
    if (nup_file == nullptr || nup_file->GetSize() < 0x80) {
        return FileType::Error;
    }

    std::array<u8, 4> magic{};
    if (nup_file->Read(magic.data(), 4) != 4) {
        return FileType::Error;
    }

    constexpr std::array<u8, 4> NUP_MAGIC = {'N', 'U', 'P', 0x01};
    if (magic == NUP_MAGIC) {
        return FileType::NUP;
    }

    return FileType::Error;
}

AppLoader::LoadResult AppLoader_Nup::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }

    if (!secondary_loader) {
        return {ResultStatus::ErrorNoExeFS, {}};
    }

    const auto result = secondary_loader->Load(process, system);
    if (result.first == ResultStatus::Success) {
        // The delegated ExeFS loader is non-standalone; register the merged NUP RomFS here.
        system.GetFileSystemController().RegisterProcess(
            process.GetProcessId(), title_id,
            std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                  system.GetFileSystemController()));
        LOG_INFO(Loader, "Registered NUP filesystem for process {}, title {:016X}, RomFS: {}",
                 process.GetProcessId(), title_id, romfs != nullptr);
        is_loaded = true;
    }

    return result;
}

ResultStatus AppLoader_Nup::ReadRomFS(FileSys::VirtualFile& out_file) {
    if (!romfs) {
        return ResultStatus::ErrorNoRomFS;
    }
    out_file = romfs;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadProgramId(u64& out_program_id) {
    if (title_id == 0) {
        if (secondary_loader) {
            return secondary_loader->ReadProgramId(out_program_id);
        }
        return ResultStatus::ErrorNotImplemented;
    }
    out_program_id = title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadIcon(std::vector<u8>& buffer) {
    if (icon_data.empty()) {
        if (secondary_loader) {
            return secondary_loader->ReadIcon(buffer);
        }
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_data;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadTitle(std::string& title) {
    if (title_name.empty()) {
        if (secondary_loader) {
            return secondary_loader->ReadTitle(title);
        }
        return ResultStatus::ErrorNoControl;
    }
    title = title_name;
    return ResultStatus::Success;
}

ResultStatus AppLoader_Nup::ReadControlData(FileSys::NACP& nacp) {
    if (nacp_data.empty()) {
        if (secondary_loader) {
            return secondary_loader->ReadControlData(nacp);
        }
        return ResultStatus::ErrorNoControl;
    }
    if (nacp_data.size() >= sizeof(FileSys::RawNACP)) {
        FileSys::NACP nacp_obj(std::make_shared<FileSys::VectorVfsFile>(nacp_data, "control.nacp"));
        nacp = nacp_obj;
        return ResultStatus::Success;
    }
    return ResultStatus::ErrorNoControl;
}


bool AppLoader_Nup::IsRomFSUpdatable() const {
    return false;
}

ResultStatus AppLoader_Nup::ReadNSOModules(Modules& modules) {
    if (secondary_loader) {
        return secondary_loader->ReadNSOModules(modules);
    }
    return ResultStatus::ErrorNotImplemented;
}

} // namespace Loader

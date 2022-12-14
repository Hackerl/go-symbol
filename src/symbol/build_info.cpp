#include <go/symbol/build_info.h>
#include <go/binary/binary.h>
#include <zero/log.h>
#include <algorithm>
#include <cstddef>
#include <regex>

constexpr auto BUILD_INFO_OFFSET = 16;
constexpr auto BUILD_INFO_MAGIC_SIZE = 14;

constexpr auto POINTER_FREE_OFFSET = 32;
constexpr auto POINTER_FREE_FLAG = std::byte{0x2};

bool go::symbol::Version::operator==(const Version &rhs) const {
    return major == rhs.major && minor == rhs.minor;
}

bool go::symbol::Version::operator!=(const Version &rhs) const {
    return !operator==(rhs);
}

bool go::symbol::Version::operator<(const Version &rhs) const {
    return major < rhs.major || (major == rhs.major && minor < rhs.minor);
}

bool go::symbol::Version::operator>(const go::symbol::Version &rhs) const {
    return major > rhs.major || (major == rhs.major && minor > rhs.minor);
}

bool go::symbol::Version::operator<=(const go::symbol::Version &rhs) const {
    return !operator>(rhs);
}

bool go::symbol::Version::operator>=(const go::symbol::Version &rhs) const {
    return !operator<(rhs);
}

std::optional<go::symbol::Version> go::symbol::parseVersion(std::string_view str) {
    std::match_results<std::string_view::const_iterator> match;

    if (!std::regex_match(str.begin(), str.end(), match, std::regex(R"(^go(\d+)\.(\d+).*)")))
        return std::nullopt;

    std::optional<int> major = zero::strings::toNumber<int>(match.str(1));
    std::optional<int> minor = zero::strings::toNumber<int>(match.str(2));

    if (!major || !minor)
        return std::nullopt;

    return Version{*major, *minor};
}

go::symbol::BuildInfo::BuildInfo(elf::Reader reader, std::shared_ptr<elf::ISection> section) : mReader(std::move(reader)), mSection(std::move(section)) {
    mPtrSize = std::to_integer<size_t>(mSection->data()[BUILD_INFO_MAGIC_SIZE]);
    mBigEndian = std::to_integer<bool>(mSection->data()[BUILD_INFO_MAGIC_SIZE + 1]);
    mPointerFree = std::to_integer<bool>(mSection->data()[BUILD_INFO_MAGIC_SIZE + 1] & POINTER_FREE_FLAG);
}

std::optional<go::symbol::Version> go::symbol::BuildInfo::version() {
    const std::byte *buffer = mSection->data();

    if (!mPointerFree) {
        std::optional<std::string> str = readString(buffer + BUILD_INFO_OFFSET);

        if (!str)
            return std::nullopt;

        return parseVersion(*str);
    }

    std::optional<std::pair<uint64_t, int>> result = binary::uVarInt(buffer + POINTER_FREE_OFFSET);

    if (!result)
        return std::nullopt;

    return parseVersion({(char *) buffer + POINTER_FREE_OFFSET + result->second, result->first});
}

std::optional<go::symbol::ModuleInfo> go::symbol::BuildInfo::moduleInfo() {
    const std::byte *buffer = mSection->data();
    std::string modInfo;

    if (!mPointerFree) {
        std::optional<std::string> str = readString(buffer + BUILD_INFO_OFFSET + mPtrSize);

        if (!str)
            return std::nullopt;

        modInfo = std::move(*str);
    } else {
        std::optional<std::pair<uint64_t, int>> result = binary::uVarInt(buffer + POINTER_FREE_OFFSET);

        if (!result)
            return std::nullopt;

        const std::byte *ptr = buffer + POINTER_FREE_OFFSET + result->first + result->second;

        result = binary::uVarInt(ptr);

        if (!result)
            return std::nullopt;

        modInfo = {(char *)ptr + result->second, result->first};
    }

    if (modInfo.length() < 32) {
        LOG_ERROR("invalid module info");
        return std::nullopt;
    }

    auto readEntry = [](const std::string &module) -> std::optional<Module> {
        std::vector<std::string> tokens = zero::strings::split(module, "\t");

        if (tokens.size() != 4)
            return std::nullopt;

        return Module{tokens[1], tokens[2], tokens[3]};
    };

    ModuleInfo moduleInfo;

    for (const auto &m: zero::strings::split({modInfo.data() + 16, modInfo.length() - 32}, "\n")) {
        if (zero::strings::startsWith(m, "path")) {
            std::vector<std::string> tokens = zero::strings::split(m, "\t");

            if (tokens.size() != 2)
                continue;

            moduleInfo.path = tokens[1];
        } else if (zero::strings::startsWith(m, "mod")) {
            std::optional<Module> module = readEntry(m);

            if (!module)
                continue;

            moduleInfo.main = std::move(*module);
        } else if (zero::strings::startsWith(m, "dep")) {
            std::optional<Module> module = readEntry(m);

            if (!module)
                continue;

            moduleInfo.deps.push_back(std::move(*module));
        } else if (zero::strings::startsWith(m, "=>")) {
            std::optional<Module> module = readEntry(m);

            if (!module)
                continue;

            moduleInfo.deps.back().replace = std::make_unique<Module>(std::move(*module));
        }
    }

    return moduleInfo;
}

std::optional<std::string> go::symbol::BuildInfo::readString(const std::byte *data) {
    auto read = [=](const std::byte *ptr) -> uint64_t {
        if (mPtrSize == 4)
            return mBigEndian ? be32toh(*(uint32_t *) ptr) : le32toh(*(uint32_t *) ptr);

        return mBigEndian ? be64toh(*(uint64_t *) ptr) : le64toh(*(uint64_t *) ptr);
    };

    std::optional<std::vector<std::byte>> buffer = mReader.readVirtualMemory(read(data), mPtrSize * 2);

    if (!buffer)
        return std::nullopt;

    buffer = mReader.readVirtualMemory(read(buffer->data()), read(buffer->data() + mPtrSize));

    if (!buffer)
        return std::nullopt;

    return std::string{(char *) buffer->data(), buffer->size()};
}
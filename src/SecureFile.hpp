#pragma once

#include "Snapshot.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

inline constexpr mode_t HSR_SECURE_DIR_MODE = 0700;
inline constexpr mode_t HSR_SECURE_FILE_MODE = 0600;

class CFd {
  public:
    explicit CFd(int fd) : m_fd(fd) {}
    ~CFd() {
        close();
    }
    CFd(const CFd&) = delete;
    CFd& operator=(const CFd&) = delete;
    CFd(CFd&& other) noexcept : m_fd(other.release()) {}
    CFd& operator=(CFd&& other) noexcept {
        if (this != &other) {
            close();
            m_fd = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const {
        return m_fd;
    }
    [[nodiscard]] int release() {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }
    void close() {
        if (m_fd >= 0) {
            static_cast<void>(::close(m_fd));
            m_fd = -1;
        }
    }

  private:
    int m_fd = -1;
};

inline bool ensureSecureCacheDir(const std::filesystem::path& cacheDir) {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec) {
        return false;
    }

    struct stat st{};
    if (::lstat(cacheDir.c_str(), &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != ::getuid()) {
        return false;
    }
    if ((st.st_mode & 0777) != HSR_SECURE_DIR_MODE &&
        ::chmod(cacheDir.c_str(), HSR_SECURE_DIR_MODE) != 0) {
        return false;
    }
    return true;
}

inline bool isSecureRegularStat(const struct stat& st) {
    if (!S_ISREG(st.st_mode) || st.st_uid != ::getuid()) {
        return false;
    }
    if ((st.st_mode & 0077) != 0) {
        return false;
    }
    return st.st_size >= 0 && static_cast<unsigned long long>(st.st_size) <= HSR_MAX_SNAPSHOT_BYTES;
}

inline bool isSecureRegularFile(const std::filesystem::path& path) {
    CFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0) {
        return false;
    }

    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        return false;
    }
    return isSecureRegularStat(st);
}

inline std::optional<std::string> readSecureFile(const std::filesystem::path& path) {
    CFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0) {
        return std::nullopt;
    }

    struct stat st{};
    if (::fstat(fd.get(), &st) != 0 || !isSecureRegularStat(st)) {
        return std::nullopt;
    }

    std::string content;
    content.reserve(static_cast<size_t>(st.st_size));
    std::array<char, 8192> buffer{};
    while (true) {
        ssize_t count = ::read(fd.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        if (count == 0) {
            break;
        }
        if (content.size() + static_cast<size_t>(count) > HSR_MAX_SNAPSHOT_BYTES) {
            return std::nullopt;
        }
        content.append(buffer.data(), static_cast<size_t>(count));
    }
    return content;
}

inline bool writeFileAtomic(const std::filesystem::path& target, std::string_view content) {
    if (content.size() > HSR_MAX_SNAPSHOT_BYTES) {
        return false;
    }

    std::filesystem::path tmp = target;
    tmp += ".tmp." + std::to_string(::getpid());

    CFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  HSR_SECURE_FILE_MODE));
    if (fd.get() < 0) {
        return false;
    }

    const char* cursor = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd.get(), cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::unlink(tmp.c_str());
            return false;
        }
        cursor += written;
        remaining -= static_cast<size_t>(written);
    }
    if (::fsync(fd.get()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    fd.close();
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    static_cast<void>(::chmod(target.c_str(), HSR_SECURE_FILE_MODE));
    if (CFd dirFd(::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        dirFd.get() >= 0) {
        static_cast<void>(::fsync(dirFd.get()));
    }
    return true;
}

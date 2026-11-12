#include <kos.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string host_path(const char *path) {
  if (path != nullptr && std::strncmp(path, "/vmu/", 5) == 0) {
    return std::string("vmu/") + (path + 5);
  }
  return path != nullptr ? path : "";
}

void make_parent_dirs(const std::string &path) {
  for (size_t slash = 1; (slash = path.find('/', slash)) != std::string::npos;
       slash++) {
    const std::string dir = path.substr(0, slash);
    if (!dir.empty()) {
      (void)mkdir(dir.c_str(), 0755);
    }
  }
}

}  // namespace

extern "C" {

file_t fs_open(const char *path, int mode) {
  const std::string native = host_path(path);
  if ((mode & O_WRONLY) != 0 || (mode & O_RDWR) != 0) {
    make_parent_dirs(native);
    mode |= O_CREAT;
  }
  return open(native.c_str(), mode, 0644);
}

int fs_close(file_t file) { return close(file); }
ssize_t fs_read(file_t file, void *buffer, size_t bytes) {
  return read(file, buffer, bytes);
}
ssize_t fs_write(file_t file, const void *buffer, size_t bytes) {
  return write(file, buffer, bytes);
}
off_t fs_seek(file_t file, off_t offset, int whence) {
  return lseek(file, offset, whence);
}
off_t fs_tell(file_t file) { return lseek(file, 0, SEEK_CUR); }
int fs_unlink(const char *path) { return unlink(host_path(path).c_str()); }

int vmu_pkg_load_icon(vmu_pkg_t *, const char *) { return 0; }

int vmu_pkg_build(const vmu_pkg_t *pkg, uint8_t **output, int *size) {
  if (pkg == nullptr || output == nullptr || size == nullptr ||
      pkg->data == nullptr || pkg->data_len > static_cast<size_t>(INT32_MAX)) {
    return -1;
  }
  *output = static_cast<uint8_t *>(malloc(pkg->data_len));
  if (*output == nullptr) {
    return -1;
  }
  std::memcpy(*output, pkg->data, pkg->data_len);
  *size = static_cast<int>(pkg->data_len);
  return 0;
}

}  // extern "C"

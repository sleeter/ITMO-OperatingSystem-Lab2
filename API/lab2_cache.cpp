#include "lab2_cache.h"
#include <fcntl.h>
#include <unistd.h>
#include <map>
#include <list>
#include <cstring>
#include <iostream>
#include <cerrno>

#define BLOCK_SIZE 4096
#define MAX_CACHE_SIZE 32
// 4096 * 32 = 128KB

static unsigned long cache_hits = 0;
static unsigned long cache_misses = 0;

struct CacheBlock {
    char* data;
    bool is_dirty;
    bool was_accessed;
};


std::map<int, std::pair<int, off_t>> fd_table;
std::map<std::pair<int, off_t>, CacheBlock> cache_table;
std::list<std::pair<int, off_t>> cache_queue;

std::pair<int, off_t>& get_file_descriptor(int fd);
int write_block(const int fd, const char* buf, const off_t offset);
char* allocate_aligned_buffer();

void free_cache_block() {
    while (!cache_queue.empty()) {
        std::pair<int, off_t> key = cache_queue.front();
        cache_queue.pop_front();
        CacheBlock& block = cache_table[key];
        if (block.was_accessed) {
            block.was_accessed = false;
            cache_queue.push_back(key);
        } else {
            if (block.is_dirty && !write_block(key.first, block.data, key.second * BLOCK_SIZE)) {
                perror("write_block");
            }
            free(block.data);
            cache_table.erase(key);
            break;
        }
    }
}

int lab2_open(const char* path, const int flags, const mode_t mode) {
    const int fd = open(path, flags, mode);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    fd_table[fd] = {fd, 0};
    return fd;
}

int lab2_close(const int fd) {
    const auto iterator = fd_table.find(fd);
    if (iterator == fd_table.end()) {
        errno = EBADF;
        return -1;
    }
    lab2_fsync(fd);
    const int result = close(iterator->second.first);
    fd_table.erase(iterator);
    return result;
}

ssize_t lab2_read(const int fd, void* buf, const size_t count) {
    auto& [found_fd, offset] = get_file_descriptor(fd);
    if (found_fd < 0 || offset < 0) {
        errno = EBADF;
        return -1;
    }
    size_t bytes_read = 0;
    const auto buffer = static_cast<char*>(buf);

    while (bytes_read < count) {
        off_t block_id = offset / BLOCK_SIZE;
        const size_t block_offset = offset % BLOCK_SIZE;
        const size_t bytes_to_read = std::min(BLOCK_SIZE - block_offset, count - bytes_read);

        std::pair key = {found_fd, block_id};
        auto cache_iterator = cache_table.find(key);
        if (cache_iterator != cache_table.end()) {
            cache_hits++;

            CacheBlock& found_block = cache_iterator->second;
            found_block.was_accessed = true;
            std::memcpy(buffer + bytes_read, found_block.data + block_offset, bytes_to_read);
            offset += bytes_to_read;
            bytes_read += bytes_to_read;
        } else {
            cache_misses++;
            if (cache_table.size() >= MAX_CACHE_SIZE) free_cache_block();
            char* aligned_buf = allocate_aligned_buffer();
            const ssize_t ret = pread(found_fd, aligned_buf, BLOCK_SIZE, block_id * BLOCK_SIZE);
            if (ret < 0) {
                perror("pread");
                free(aligned_buf);
                return -1;
            }
            if (ret == 0) {
                free(aligned_buf);
                break;
            }
            const auto valid_data_size = static_cast<size_t>(ret);
            if (ret < BLOCK_SIZE) {
                std::memset(aligned_buf + ret, 0, BLOCK_SIZE - ret);
            }
            const CacheBlock new_block = {aligned_buf, false, true};
            cache_table[key] = new_block;
            cache_queue.push_back(key);

            size_t available_bytes = valid_data_size - block_offset;
            if (available_bytes <= 0) {
                break;
            }
            const size_t bytes_from_block = std::min(bytes_to_read, available_bytes);
            std::memcpy(buffer + bytes_read, aligned_buf + block_offset, bytes_from_block);
            offset += bytes_from_block;
            bytes_read += bytes_from_block;
        }
    }
    return bytes_read;
}

ssize_t lab2_write(const int fd, const void* buf, const size_t count) {
    auto& [found_fd, offset] = get_file_descriptor(fd);
    if (found_fd < 0 || offset < 0) {
        errno = EBADF;
        return -1;
    }
    size_t bytes_written = 0;
    const auto buffer = static_cast<const char*>(buf);

    while (bytes_written < count) {
        off_t block_id = offset / BLOCK_SIZE;
        const size_t block_offset = offset % BLOCK_SIZE;
        const size_t to_write = std::min(BLOCK_SIZE - block_offset, count - bytes_written);

        std::pair key = {found_fd, block_id};
        auto cache_it = cache_table.find(key);
        CacheBlock* block_ptr = nullptr;

        if (cache_it == cache_table.end()) {
            cache_misses++;
            if (cache_table.size() >= MAX_CACHE_SIZE) {
                free_cache_block();
            }
            char* aligned_buf = allocate_aligned_buffer();
            ssize_t ret = pread(found_fd, aligned_buf, BLOCK_SIZE, block_id * BLOCK_SIZE);
            if (ret < 0) {
                perror("pread");
                free(aligned_buf);
                return -1;
            } if (ret == 0) {
                std::memset(aligned_buf, 0, BLOCK_SIZE);
            } else if (ret < BLOCK_SIZE) {
                std::memset(aligned_buf + ret, 0, BLOCK_SIZE - ret);
            }
            CacheBlock& block = cache_table[key] = {aligned_buf, false, true};
            cache_queue.push_back(key);
            block_ptr = &block;
        } else {
            cache_hits++;
            block_ptr = &cache_it->second;
            block_ptr->was_accessed = true;
        }
        std::memcpy(block_ptr->data + block_offset, buffer + bytes_written, to_write);
        block_ptr->is_dirty = true;

        offset += to_write;
        bytes_written += to_write;
    }
    return bytes_written;
}

off_t lab2_lseek(const int fd, const off_t offset, const int whence) {
    auto& [found_fd, file_offset] = get_file_descriptor(fd);
    if (found_fd < 0 || file_offset < 0) {
        errno = EBADF;
        return -1;
    }
    if (whence != SEEK_SET || offset < 0) {
        errno = EINVAL;
        return -1;
    }
    file_offset = offset;
    return file_offset;
}

int lab2_fsync(const int fd_to_sync) {
    const int found_fd = get_file_descriptor(fd_to_sync).first;
    if (found_fd < 0) {
        errno = EBADF;
        return -1;
    }
    for (auto& [key, block] : cache_table) {
        if (key.first == found_fd && block.is_dirty && !write_block(key.first, block.data, key.second * BLOCK_SIZE)) {
            perror("write_block");
            return -1;
        }
        block.is_dirty = false;
    }
    return fsync(found_fd);
}

int write_block(const int fd, const char* buf, const off_t offset) {
    ssize_t ret = pwrite(fd, buf, BLOCK_SIZE, offset * BLOCK_SIZE);
    if (ret != BLOCK_SIZE) {
        return 0;
    }
    return 1;
}
std::pair<int, off_t>& get_file_descriptor(const int fd) {
    const auto iterator = fd_table.find(fd);
    if (iterator == fd_table.end()) {
        std::pair<int, off_t> invalid_fd = {-1, -1};
        return invalid_fd;
    }
    return iterator->second;
}

char* allocate_aligned_buffer() {
    void* buf = nullptr;
    if (posix_memalign(&buf, BLOCK_SIZE, BLOCK_SIZE) != 0) {
        perror("posix_memalign");
        return nullptr;
    }
    return static_cast<char*>(buf);
}

unsigned long lab2_get_cache_hits() {
    return cache_hits;
}

unsigned long lab2_get_cache_misses() {
    return cache_misses;
}

void lab2_reset_cache_counters() {
    cache_hits = 0;
    cache_misses = 0;
}
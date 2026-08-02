#include "simpledb/mmap_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

mmap_file_t *mmap_file_open(const char *filepath) {
    if (!filepath)
        return NULL;

    // open the file
    int fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "[simpledb error] Failed to open column file '%s': %s (errno=%d)\n",
                filepath, strerror(errno), errno);
        return NULL;
    }

    // get the file size
    struct stat st;
    if (fstat(fd, &st) == -1) {
        fprintf(stderr, "[simpledb error] Failed to fstat column file '%s': %s (errno=%d)\n",
                filepath, strerror(errno), errno);
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;

    // create the mmap
    void *addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr,
                "[simpledb error] Failed to mmap column file '%s' (%zu bytes): %s (errno=%d)\n",
                filepath, file_size, strerror(errno), errno);
        close(fd);
        return NULL;
    }

    mmap_file_t *file = (mmap_file_t *)malloc(sizeof(mmap_file_t));
    file->data = addr;
    file->size = file_size;
    file->fd = fd;

    return file;
}

void mmap_file_close(mmap_file_t *file) {
    if (!file)
        return;

    if (file->data && file->size > 0) {
        munmap((void *)file->data, file->size);
    }

    if (file->fd != -1) {
        close(file->fd);
    }

    free(file);
}

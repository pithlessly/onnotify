#include <stdbool.h>   /* bool */
#include <stddef.h>    /* size_t, ssize_t */
#include <stdint.h>    /* int32_t */
#include <string.h>    /* memchr, strlen, memmove */
#include <stdlib.h>    /* getenv, exit, malloc, free, realpath */
#include <stdio.h>     /* fprintf, stderr, snprintf */
#include <limits.h>    /* PATH_MAX */
#include <fcntl.h>     /* open, O_RDONLY, close */
#include <sys/file.h>  /* flock, LOCK_SH, LOCK_UN */
#include <unistd.h>    /* getcwd, read */
#include <errno.h>     /* errno, strerror, ENOENT, ENXIO */
#include <sys/types.h> /* pid_t */
#include <ctype.h>     /* isdigit */

static
int
opendb(char const *whoami, char const *progname) {
#define TEMPLATE "/tmp/notifydb.%s/db"
    int size = snprintf(NULL, 0, TEMPLATE, whoami);
    size_t bufsz = 1 + (size_t) size;
    char *path = malloc(bufsz);
    snprintf(path, bufsz, TEMPLATE, whoami);
#undef TEMPLATE

    int db_fd = open(path, O_RDONLY);
    if (db_fd < 0) {
        if (errno == ENOENT)
            fprintf(stderr, "%s: no processes to notify\n", progname);
        else
            fprintf(stderr, "%s: open(%s): %s\n", progname, path, strerror(errno));
        /* free(path); */
        exit(1);
    }

    /* lock the DB so that nothing writes to it while we check it */
    if (flock(db_fd, LOCK_SH) < 0) {
        fprintf(stderr, "%s: flock(%s): %s\n", progname, path, strerror(errno));
        /* free(path); */
        exit(1);
    }

    free(path);
    return db_fd;
}

static
bool
is_match(char const *path_in_db, char const *candidate_path) {
    // possibilities for matching pairs of paths:
    // - foo, foo
    // - foo, foo/...
    // - foo/, foo
    while (*path_in_db == *candidate_path && *path_in_db != '\0')
        path_in_db++, candidate_path++;
    return *path_in_db == '\0' && (*candidate_path == '\0' || *candidate_path == '/')
        || *path_in_db == '/' && *(path_in_db + 1) == '\0';
}

#define DB_BUF (PATH_MAX * 2)
static
int32_t
db_search(char const *progname, int fd, char ncandidates, char const *const candidates[]) {
    char buffer[DB_BUF];
    size_t buflen = 0;
    size_t offset = 0;
    for (;;) {
        /* fill up the rest of the buffer with more records */
        ssize_t nread = read(fd, buffer + buflen, DB_BUF - buflen);
        if (nread < 0) {
            fprintf(stderr, "%s: read(): %s\n", progname, strerror(errno));
            return -1;
        }
        buflen += nread;

        if (nread == 0) {
            if (buflen == 0) {
                break;
            } else {
                fprintf(stderr, "%s: incomplete record at end of file\n", progname);
                fprintf(stderr, "buflen=%zu offset=%zu nread=%zu\n", buflen, offset, nread);
                return -1;
            }
        }

        char *cursor = buffer;
        /* loop as long as there is a complete record */
        while (memchr(cursor, '\n', buffer + buflen - cursor)) {
            /* skip past the timestamp */
            while (isdigit(*cursor)) cursor++;
            if (*cursor++ != ' ') {
                fprintf(stderr, "%s: malformed record at position %zu:\n  expected ' ' after timestamp\n", progname, offset + (cursor - buffer));
                return -1;
            }
            int32_t record_id = 0;
            while (isdigit(*cursor)) {
                char value = *cursor - '0';
                record_id = (record_id * 10) + value;
                if (record_id > 99999999) {
                    fprintf(stderr, "%s: malformed record at position %zu:\n  ID is too big\n", progname, offset + (cursor - buffer));
                    return -1;
                }
                cursor++;
            }
            if (*cursor++ != ' ') {
                fprintf(stderr, "%s: malformed record at position %zu:\n  expected ' ' after ID\n", progname, offset + (cursor - buffer));
                return -1;
            }
            char *path = cursor;
            /* replace the newline at the end of the record with a NUL terminator */
            while (*cursor != '\n') cursor++;
            *cursor++ = '\0';
            for (size_t i = 0; i < ncandidates; i++)
                if (is_match(path, candidates[i]))
                    return record_id;
        }

        if (cursor == buffer) {
            if (buflen == DB_BUF)
                /* buffer is full to max, but we still couldn't parse a complete record out of it */
                fprintf(stderr, "%s: malformed record (too long) near position %zu\n", progname, offset);
            else
                fprintf(stderr, "%s: malformed record at end of file\n", progname);
            return -1;
        }

        size_t nconsumed = cursor - buffer;
        offset += nconsumed;
        memmove(buffer, cursor, buflen - nconsumed);
        buflen -= nconsumed;
    }

    fprintf(stderr, "%s: no processes to notify\n", progname);
    return -1;
}

static
bool
write_1_byte_to_fifo(char const *progname, char const *whoami, long id) {
#define TEMPLATE "/tmp/notifydb.%s/fifo.%ld"
    int size = snprintf(NULL, 0, TEMPLATE, whoami, id);
    size_t bufsz = 1 + (size_t) size;
    char *path = malloc(bufsz);
    snprintf(path, bufsz, TEMPLATE, whoami, id);
#undef TEMPLATE

    int fifo_fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        char const *msg =
            errno == ENXIO
            ? "no one is waiting on the other end of the FIFO"
            : strerror(errno);
        fprintf(stderr, "%s: open(%s): %s\n", progname, path, msg);
        free(path);
        return false;
    }

    char const buf = '1';
    ssize_t nwritten = write(fifo_fd, &buf, 1);
    bool ok;
    if (nwritten < 0) {
        fprintf(stderr, "%s: writing to %s: %s\n", progname, path, strerror(errno));
        ok = false;
    } else
        ok = true;
    free(path);
    close(fifo_fd);
    return ok;
}

int
main(int argc, char **argv) {
    if (argc < 1)
        exit(1);

    char const *progname = argv[0];
    if (argc > 2) {
        fprintf(stderr, "usage: %s [path]\n", progname);
        exit(1);
    }

    char cwd[PATH_MAX + 1];

    if (!getcwd(cwd, PATH_MAX)) {
        fprintf(stderr, "%s: getcwd(): %s\n", progname, strerror(errno));
        exit(1);
    }

    char realpath_buf[2][PATH_MAX + 1];
    char const *candidates[2];
    size_t ncandidates;

    if (argc == 2) {
        candidates[0] = argv[1];
        candidates[1] = cwd;
        ncandidates = 2;
    } else {
        candidates[0] = cwd;
        ncandidates = 1;
    }

    for (size_t i = 0; i < ncandidates; i++) {
        char const **cand = &candidates[i];
        char *buf = realpath_buf[i];
        if (!realpath(*cand, buf)) {
            fprintf(stderr, "%s: realpath(%s): %s\n", progname, *cand, strerror(errno));
            exit(1);
        }
        *cand = buf;
    }

    char *whoami = getenv("LOGNAME");
    if (!whoami || !*whoami) {
        fprintf(stderr, "%s: LOGNAME is unset\n", progname);
        exit(1);
    }

    if (memchr(whoami, '/', strlen(whoami))) {
        fprintf(stderr, "%s: LOGNAME contains a slash\n", progname);
        exit(1);
    }

    int fd = opendb(whoami, progname);
    int32_t id32 = db_search(progname, fd, ncandidates, candidates);
    if (id32 < 0) {
        /* flock(fd, LOCK_UN); */
        /* close(fd); */
        return 1;
    }

    flock(fd, LOCK_UN);
    close(fd);

    return !write_1_byte_to_fifo(progname, whoami, id32);
}

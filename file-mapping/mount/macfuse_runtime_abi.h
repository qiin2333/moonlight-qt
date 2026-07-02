#pragma once

#if defined(Q_OS_MACOS)

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <utime.h>

// Minimal libfuse2 ABI declarations used for runtime loading on macOS.
// The layout follows macFUSE/libfuse2 public headers for FUSE_USE_VERSION=26.
// Reference: https://github.com/macfuse/library/blob/master/include/fuse.h
struct fuse;
struct fuse_conn_info;
struct fuse_context;
struct fuse_file_info;
struct fuse_pollhandle;
struct fuse_bufvec;
struct setattr_x;

typedef int (*fuse_fill_dir_t)(void*, const char*, const struct stat*, off_t);
typedef struct fuse_dirhandle* fuse_dirh_t;
typedef int (*fuse_dirfil_t)(fuse_dirh_t, const char*, int, ino_t);

struct fuse_file_info {
    int flags;
    unsigned long fh_old;
    int writepage;
    unsigned int direct_io : 1;
    unsigned int keep_cache : 1;
    unsigned int flush : 1;
    unsigned int nonseekable : 1;
    unsigned int flock_release : 1;
    unsigned int padding : 25;
    unsigned int purge_attr : 1;
    unsigned int purge_ubc : 1;
    uint64_t fh;
    uint64_t lock_owner;
};

struct fuse_operations {
    int (*getattr)(const char*, struct stat*);
    int (*readlink)(const char*, char*, size_t);
    int (*getdir)(const char*, fuse_dirh_t, fuse_dirfil_t);
    int (*mknod)(const char*, mode_t, dev_t);
    int (*mkdir)(const char*, mode_t);
    int (*unlink)(const char*);
    int (*rmdir)(const char*);
    int (*symlink)(const char*, const char*);
    int (*rename)(const char*, const char*);
    int (*link)(const char*, const char*);
    int (*chmod)(const char*, mode_t);
    int (*chown)(const char*, uid_t, gid_t);
    int (*truncate)(const char*, off_t);
    int (*utime)(const char*, struct utimbuf*);
    int (*open)(const char*, struct fuse_file_info*);
    int (*read)(const char*, char*, size_t, off_t, struct fuse_file_info*);
    int (*write)(const char*, const char*, size_t, off_t, struct fuse_file_info*);
    int (*statfs)(const char*, struct statvfs*);
    int (*flush)(const char*, struct fuse_file_info*);
    int (*release)(const char*, struct fuse_file_info*);
    int (*fsync)(const char*, int, struct fuse_file_info*);
    int (*setxattr)(const char*, const char*, const char*, size_t, int, uint32_t);
    int (*getxattr)(const char*, const char*, char*, size_t, uint32_t);
    int (*listxattr)(const char*, char*, size_t);
    int (*removexattr)(const char*, const char*);
    int (*opendir)(const char*, struct fuse_file_info*);
    int (*readdir)(const char*, void*, fuse_fill_dir_t, off_t, struct fuse_file_info*);
    int (*releasedir)(const char*, struct fuse_file_info*);
    int (*fsyncdir)(const char*, int, struct fuse_file_info*);
    void* (*init)(struct fuse_conn_info*);
    void (*destroy)(void*);
    int (*access)(const char*, int);
    int (*create)(const char*, mode_t, struct fuse_file_info*);
    int (*ftruncate)(const char*, off_t, struct fuse_file_info*);
    int (*fgetattr)(const char*, struct stat*, struct fuse_file_info*);
    int (*lock)(const char*, struct fuse_file_info*, int, struct flock*);
    int (*utimens)(const char*, const struct timespec[2]);
    int (*bmap)(const char*, size_t, uint64_t*);
    unsigned int flag_nullpath_ok : 1;
    unsigned int flag_nopath : 1;
    unsigned int flag_utime_omit_ok : 1;
    unsigned int flag_reserved : 29;
    int (*ioctl)(const char*, int, void*, struct fuse_file_info*, unsigned int, void*);
    int (*poll)(const char*, struct fuse_file_info*, struct fuse_pollhandle*, unsigned*);
    int (*write_buf)(const char*, struct fuse_bufvec*, off_t, struct fuse_file_info*);
    int (*read_buf)(const char*, struct fuse_bufvec**, size_t, off_t, struct fuse_file_info*);
    int (*flock)(const char*, struct fuse_file_info*, int);
    int (*fallocate)(const char*, int, off_t, off_t, struct fuse_file_info*);
    int (*reserved00)(void*, void*, void*, void*, void*, void*, void*, void*);
    int (*reserved01)(void*, void*, void*, void*, void*, void*, void*, void*);
    int (*renamex)(const char*, const char*, unsigned int);
    int (*statfs_x)(const char*, struct statfs*);
    int (*setvolname)(const char*);
    int (*exchange)(const char*, const char*, unsigned long);
    int (*getxtimes)(const char*, struct timespec*, struct timespec*);
    int (*setbkuptime)(const char*, const struct timespec*);
    int (*setchgtime)(const char*, const struct timespec*);
    int (*setcrtime)(const char*, const struct timespec*);
    int (*chflags)(const char*, uint32_t);
    int (*setattr_x)(const char*, struct setattr_x*);
    int (*fsetattr_x)(const char*, struct setattr_x*, struct fuse_file_info*);
};

struct fuse_context {
    struct fuse* fuse;
    uid_t uid;
    gid_t gid;
    pid_t pid;
    void* private_data;
    mode_t umask;
};

#endif

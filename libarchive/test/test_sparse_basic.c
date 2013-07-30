/*-
 * Copyright (c) 2010-2012 Michihiro NAKAJIMA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "test.h"
__FBSDID("$FreeBSD$");

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_LINUX_TYPES_H
#include <linux/types.h>
#endif
#ifdef HAVE_LINUX_FIEMAP_H
#include <linux/fiemap.h>
#endif
#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif

/*
 * NOTE: On FreeBSD and Solaris, this test needs ZFS.
 * You may should perfom this test as
 * 'TMPDIR=<a directory on the ZFS> libarchive_test'.
 */

struct sparse {
	enum { DATA, HOLE, END } type;
	size_t	size;
};

static void create_sparse_file(const char *, const struct sparse *);

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <winioctl.h>
/*
 * Create a sparse file on Windows.
 */

#if !defined(PATH_MAX)
#define	PATH_MAX	MAX_PATH
#endif
#if !defined(__BORLANDC__)
#define getcwd _getcwd
#endif

static int
is_sparse_supported(const char *path)
{
	char root[MAX_PATH+1];
	char vol[MAX_PATH+1];
	char sys[MAX_PATH+1];
	DWORD flags;
	BOOL r;

	strncpy(root, path, sizeof(root)-1);
	if (((root[0] >= 'c' && root[0] <= 'z') ||
	    (root[0] >= 'C' && root[0] <= 'Z')) &&
		root[1] == ':' &&
	    (root[2] == '\\' || root[2] == '/'))
		root[3] = '\0';
	else
		return (0);
	assertEqualInt((r = GetVolumeInformation(root, vol,
	    sizeof(vol), NULL, NULL, &flags, sys, sizeof(sys))), 1);
	return (r != 0 && (flags & FILE_SUPPORTS_SPARSE_FILES) != 0);
}

static void
create_sparse_file(const char *path, const struct sparse *s)
{
	char buff[1024];
	HANDLE handle;
	DWORD dmy;

	memset(buff, ' ', sizeof(buff));

	handle = CreateFileA(path, GENERIC_WRITE, 0,
	    NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
	    NULL);
	assert(handle != INVALID_HANDLE_VALUE);
	assert(DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0,
	    NULL, 0, &dmy, NULL) != 0);
	while (s->type != END) {
		if (s->type == HOLE) {
			LARGE_INTEGER distance;

			distance.QuadPart = s->size;
			assert(SetFilePointerEx(handle, distance,
			    NULL, FILE_CURRENT) != 0);
		} else {
			DWORD w, wr;
			size_t size;

			size = s->size;
			while (size) {
				if (size > sizeof(buff))
					w = sizeof(buff);
				else
					w = (DWORD)size;
				assert(WriteFile(handle, buff, w, &wr, NULL) != 0);
				size -= wr;
			}
		}
		s++;
	}
	assertEqualInt(CloseHandle(handle), 1);
}

#else

#if defined(_PC_MIN_HOLE_SIZE)

/*
 * FreeBSD and Solaris can detect 'hole' of a sparse file
 * through lseek(HOLE) on ZFS. (UFS does not support yet)
 */

static int
is_sparse_supported(const char *path)
{
	return (pathconf(path, _PC_MIN_HOLE_SIZE) > 0);
}

#elif defined(__linux__)&& defined(HAVE_LINUX_FIEMAP_H)

/*
 * FIEMAP, which can detect 'hole' of a sparse file, has
 * been supported from 2.6.28
 */

static int
is_sparse_supported(const char *path)
{
	const struct sparse sparse_file[] = {
 		/* This hole size is too small to create a sparse
		 * files for almost filesystem. */
		{ HOLE,	 1024 }, { DATA, 10240 },
		{ END,	0 }
	};
	int fd, r;
	struct fiemap *fm;
	char buff[1024];
	const char *testfile = "can_sparse";

	(void)path; /* UNUSED */
	memset(buff, 0, sizeof(buff));
	create_sparse_file(testfile, sparse_file);
	fd = open(testfile,  O_RDWR);
	if (fd < 0)
		return (0);
	fm = (struct fiemap *)buff;
	fm->fm_start = 0;
	fm->fm_length = ~0ULL;;
	fm->fm_flags = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = (sizeof(buff) - sizeof(*fm))/
		sizeof(struct fiemap_extent);
	r = ioctl(fd, FS_IOC_FIEMAP, fm);
	close(fd);
	unlink(testfile);
	return (r >= 0);
}

#else

/*
 * Other system may do not have the API such as lseek(HOLE),
 * which detect 'hole' of a sparse file.
 */

static int
is_sparse_supported(const char *path)
{
	(void)path; /* UNUSED */
	return (0);
}

#endif

/*
 * Create a sparse file on POSIX like system.
 */

static void
create_sparse_file(const char *path, const struct sparse *s)
{
	char buff[1024];
	int fd;

	memset(buff, ' ', sizeof(buff));
	assert((fd = open(path, O_CREAT | O_WRONLY, 0600)) != -1);
	while (s->type != END) {
		if (s->type == HOLE) {
			assert(lseek(fd, s->size, SEEK_CUR) != (off_t)-1);
		} else {
			size_t w, size;

			size = s->size;
			while (size) {
				if (size > sizeof(buff))
					w = sizeof(buff);
				else
					w = size;
				assert(write(fd, buff, w) != (ssize_t)-1);
				size -= w;
			}
		}
		s++;
	}
	close(fd);
}

#endif

/*
 * Sparse test with directory traversals.
 */
static void
verify_sparse_file(struct archive *a, const char *path,
    const struct sparse *sparse, int blocks)
{
	struct archive_entry *ae;
	const void *buff;
	size_t bytes_read;
	int64_t offset;
	int64_t total;
	int data_blocks, hole;

	create_sparse_file(path, sparse);
	assert((ae = archive_entry_new()) != NULL);
	assertEqualIntA(a, ARCHIVE_OK, archive_read_disk_open(a, path));
	assertEqualIntA(a, ARCHIVE_OK, archive_read_next_header2(a, ae));
	/* Verify the number of holes only, not its offset nor its
	 * length because those alignments are deeply dependence on
	 * its filesystem. */ 
	assertEqualInt(blocks, archive_entry_sparse_count(ae));
	total = 0;
	data_blocks = 0;
	hole = 0;
	while (ARCHIVE_OK == archive_read_data_block(a, &buff, &bytes_read,
	    &offset)) {
		if (offset > total || offset == 0) {
			if (offset > total)
				hole = 1;
			data_blocks++;
		}
		total = offset + bytes_read;
	}
	if (!hole && data_blocks == 1)
		data_blocks = 0;/* There are no holes */
	assertEqualInt(blocks, data_blocks);

	assertEqualIntA(a, ARCHIVE_OK, archive_read_close(a));
	archive_entry_free(ae);
}

#if defined(_WIN32) && !defined(__CYGWIN__)
#define	close		_close
#define	open		_open
#endif

/*
 * Sparse test without directory traversals.
 */
static void
verify_sparse_file2(struct archive *a, const char *path,
    const struct sparse *sparse, int blocks, int preopen)
{
	struct archive_entry *ae;
	int fd;

	(void)sparse; /* UNUSED */
	assert((ae = archive_entry_new()) != NULL);
	archive_entry_set_pathname(ae, path);
	if (preopen)
		fd = open(path, O_RDONLY | O_BINARY);
	else
		fd = -1;
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_read_disk_entry_from_file(a, ae, fd, NULL));
	if (fd >= 0)
		close(fd);
	/* Verify the number of holes only, not its offset nor its
	 * length because those alignments are deeply dependence on
	 * its filesystem. */ 
	assertEqualInt(blocks, archive_entry_sparse_count(ae));
	archive_entry_free(ae);
}

static void
test_sparse_whole_file_data()
{
	struct archive_entry *ae;
	int64_t offset;
	int i;

	assert((ae = archive_entry_new()) != NULL);
	archive_entry_set_size(ae, 1024*10);

	/*
	 * Add sparse block data up to the file size.
	 */
	offset = 0;
	for (i = 0; i < 10; i++) {
		archive_entry_sparse_add_entry(ae, offset, 1024);
		offset += 1024;
	}

	failure("There should be no sparse");
	assertEqualInt(0, archive_entry_sparse_count(ae));
	archive_entry_free(ae);
}

DEFINE_TEST(test_sparse_basic)
{
	char *cwd;
	struct archive *a;
	/*
	 * The alignment of the hole of sparse files deeply depends
	 * on filesystem. In my experience, sparse_file2 test with
	 * 204800 bytes hole size did not pass on ZFS and the result
	 * of that test seemed the size was too small, thus you should
	 * keep a hole size more than 409600 bytes to pass this test
	 * on all platform.
	 */
	const struct sparse sparse_file0[] = {
		{ DATA,	 1024 }, { HOLE,   2048000 },
		{ DATA,	 2048 }, { HOLE,   2048000 },
		{ DATA,	 4096 }, { HOLE,  20480000 },
		{ DATA,	 8192 }, { HOLE, 204800000 },
		{ DATA,     1 }, { END,	0 }
	};
	const struct sparse sparse_file1[] = {
		{ HOLE,	409600 }, { DATA, 1 },
		{ HOLE,	409600 }, { DATA, 1 },
		{ HOLE,	409600 }, { END,  0 }
	};
	const struct sparse sparse_file2[] = {
		{ HOLE,	409600 * 1 }, { DATA, 1024 },
		{ HOLE,	409600 * 2 }, { DATA, 1024 },
		{ HOLE,	409600 * 3 }, { DATA, 1024 },
		{ HOLE,	409600 * 4 }, { DATA, 1024 },
		{ HOLE,	409600 * 5 }, { DATA, 1024 },
		{ HOLE,	409600 * 6 }, { DATA, 1024 },
		{ HOLE,	409600 * 7 }, { DATA, 1024 },
		{ HOLE,	409600 * 8 }, { DATA, 1024 },
		{ HOLE,	409600 * 9 }, { DATA, 1024 },
		{ HOLE,	409600 * 10}, { DATA, 1024 },/* 10 */
		{ HOLE,	409600 * 1 }, { DATA, 1024 * 1 },
		{ HOLE,	409600 * 2 }, { DATA, 1024 * 2 },
		{ HOLE,	409600 * 3 }, { DATA, 1024 * 3 },
		{ HOLE,	409600 * 4 }, { DATA, 1024 * 4 },
		{ HOLE,	409600 * 5 }, { DATA, 1024 * 5 },
		{ HOLE,	409600 * 6 }, { DATA, 1024 * 6 },
		{ HOLE,	409600 * 7 }, { DATA, 1024 * 7 },
		{ HOLE,	409600 * 8 }, { DATA, 1024 * 8 },
		{ HOLE,	409600 * 9 }, { DATA, 1024 * 9 },
		{ HOLE,	409600 * 10}, { DATA, 1024 * 10},/* 20 */
		{ END,	0 }
	};
	const struct sparse sparse_file3[] = {
 		/* This hole size is too small to create a sparse
		 * files for almost filesystem. */
		{ HOLE,	 1024 }, { DATA, 10240 },
		{ END,	0 }
	};

	/*
	 * Test for the case that sparse data indicates just the whole file
	 * data.
	 */
	test_sparse_whole_file_data();

	/* Check if the filesystem where CWD on can
	 * report the number of the holes of a sparse file. */
#ifdef PATH_MAX
	cwd = getcwd(NULL, PATH_MAX);/* Solaris getcwd needs the size. */
#else
	cwd = getcwd(NULL, 0);
#endif
	if (!assert(cwd != NULL))
		return;
	if (!is_sparse_supported(cwd)) {
		free(cwd);
		skipping("This filesystem or platform do not support "
		    "the reporting of the holes of a sparse file through "
		    "API such as lseek(HOLE)");
		return;
	}

	/*
	 * Get sparse data through directory traversals.
	 */
	assert((a = archive_read_disk_new()) != NULL);

	verify_sparse_file(a, "file0", sparse_file0, 5);
	verify_sparse_file(a, "file1", sparse_file1, 2);
	verify_sparse_file(a, "file2", sparse_file2, 20);
	verify_sparse_file(a, "file3", sparse_file3, 0);

	assertEqualInt(ARCHIVE_OK, archive_read_free(a));

	/*
	 * Get sparse data through archive_read_disk_entry_from_file().
	 */
	assert((a = archive_read_disk_new()) != NULL);

	verify_sparse_file2(a, "file0", sparse_file0, 5, 0);
	verify_sparse_file2(a, "file0", sparse_file0, 5, 1);

	assertEqualInt(ARCHIVE_OK, archive_read_free(a));
	free(cwd);
}

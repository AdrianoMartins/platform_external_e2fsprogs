/*
 * util.c --- miscellaneous utilities
 * 
 * Copyright (C) 1993, 1994, 1995, 1996, 1997 Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <termios.h>

#include "e2fsck.h"

#include <sys/time.h>
#include <sys/resource.h>

void fatal_error(e2fsck_t ctx, const char *msg)
{
	if (msg) 
		fprintf (stderr, "e2fsck: %s\n", msg);
	ctx->flags |= E2F_FLAG_ABORT;
	if (ctx->flags & E2F_FLAG_SETJMP_OK)
		longjmp(ctx->abort_loc, 1);
	exit(FSCK_ERROR);
}

void *e2fsck_allocate_memory(e2fsck_t ctx, unsigned int size,
			     const char *description)
{
	void *ret;
	char buf[256];

#ifdef DEBUG_ALLOCATE_MEMORY
	printf("Allocating %d bytes for %s...\n", size, description);
#endif
	ret = malloc(size);
	if (!ret) {
		sprintf(buf, "Can't allocate %s\n", description);
		fatal_error(ctx, buf);
	}
	memset(ret, 0, size);
	return ret;
}

int ask_yn(const char * string, int def)
{
	int		c;
	struct termios	termios, tmp;
	const char	*defstr;

	tcgetattr (0, &termios);
	tmp = termios;
	tmp.c_lflag &= ~(ICANON | ECHO);
	tmp.c_cc[VMIN] = 1;
	tmp.c_cc[VTIME] = 0;
	tcsetattr (0, TCSANOW, &tmp);

	if (def == 1)
		defstr = "<y>";
	else if (def == 0)
		defstr = "<n>";
	else
		defstr = " (y/n)";
	printf("%s%s? ", string, defstr);
	while (1) {
		fflush (stdout);
		if ((c = getchar()) == EOF)
			break;
		c = toupper(c);
		if (c == 'Y') {
			def = 1;
			break;
		}
		else if (c == 'N') {
			def = 0;
			break;
		}
		else if ((c == ' ' || c == '\n') && (def != -1))
			break;
	}
	if (def)
		printf ("yes\n\n");
	else
		printf ("no\n\n");
	tcsetattr (0, TCSANOW, &termios);
	return def;
}

int ask (e2fsck_t ctx, const char * string, int def)
{
	if (ctx->options & E2F_OPT_NO) {
		printf ("%s? no\n\n", string);
		return 0;
	}
	if (ctx->options & E2F_OPT_YES) {
		printf ("%s? yes\n\n", string);
		return 1;
	}
	if (ctx->options & E2F_OPT_PREEN) {
		printf ("%s? %s\n\n", string, def ? "yes" : "no");
		return def;
	}
	return ask_yn(string, def);
}

void e2fsck_read_bitmaps(e2fsck_t ctx)
{
	ext2_filsys fs = ctx->fs;
	errcode_t	retval;

	if (ctx->invalid_bitmaps) {
		com_err(ctx->program_name, 0,
			"e2fsck_read_bitmaps: illegal bitmap block(s) for %s",
			ctx->device_name);
		fatal_error(ctx, 0);
	}

	ehandler_operation("reading inode and block bitmaps");
	retval = ext2fs_read_bitmaps(fs);
	ehandler_operation(0);
	if (retval) {
		com_err(ctx->program_name, retval,
			"while retrying to read bitmaps for %s",
			ctx->device_name);
		fatal_error(ctx, 0);
	}
}

void e2fsck_write_bitmaps(e2fsck_t ctx)
{
	ext2_filsys fs = ctx->fs;
	errcode_t	retval;

	if (ext2fs_test_bb_dirty(fs)) {
		ehandler_operation("writing block bitmaps");
		retval = ext2fs_write_block_bitmap(fs);
		ehandler_operation(0);
		if (retval) {
			com_err(ctx->program_name, retval,
				"while retrying to write block bitmaps for %s",
				ctx->device_name);
			fatal_error(ctx, 0);
		}
	}

	if (ext2fs_test_ib_dirty(fs)) {
		ehandler_operation("writing inode bitmaps");
		retval = ext2fs_write_inode_bitmap(fs);
		ehandler_operation(0);
		if (retval) {
			com_err(ctx->program_name, retval,
				"while retrying to write inode bitmaps for %s",
				ctx->device_name);
			fatal_error(ctx, 0);
		}
	}
}

void preenhalt(e2fsck_t ctx)
{
	ext2_filsys fs = ctx->fs;

	if (!(ctx->options & E2F_OPT_PREEN))
		return;
	fprintf(stderr, "\n\n%s: UNEXPECTED INCONSISTENCY; "
		"RUN fsck MANUALLY.\n\t(i.e., without -a or -p options)\n",
	       ctx->device_name);
	if (fs != NULL) {
		fs->super->s_state |= EXT2_ERROR_FS;
		ext2fs_mark_super_dirty(fs);
		ext2fs_close(fs);
	}
	exit(FSCK_UNCORRECTED);
}

#ifdef RESOURCE_TRACK
void init_resource_track(struct resource_track *track)
{
#ifdef HAVE_GETRUSAGE
	struct rusage r;
#endif
	
	track->brk_start = sbrk(0);
	gettimeofday(&track->time_start, 0);
#ifdef HAVE_GETRUSAGE
#ifdef solaris
	memcpy(&r, 0, sizeof(struct rusage));
#endif
	getrusage(RUSAGE_SELF, &r);
	track->user_start = r.ru_utime;
	track->system_start = r.ru_stime;
#else
	track->user_start.tv_sec = track->user_start.tv_usec = 0;
	track->system_start.tv_sec = track->system_start.tv_usec = 0;
#endif
}

#ifdef __GNUC__
#define _INLINE_ __inline__
#else
#define _INLINE_
#endif

static _INLINE_ float timeval_subtract(struct timeval *tv1,
				       struct timeval *tv2)
{
	return ((tv1->tv_sec - tv2->tv_sec) +
		((float) (tv1->tv_usec - tv2->tv_usec)) / 1000000);
}

void print_resource_track(const char *desc, struct resource_track *track)
{
#ifdef HAVE_GETRUSAGE
	struct rusage r;
#endif
	struct timeval time_end;

	gettimeofday(&time_end, 0);

	if (desc)
		printf("%s :", desc);
	
#ifdef HAVE_GETRUSAGE
	getrusage(RUSAGE_SELF, &r);

	printf("Memory used: %d, elapsed time: %6.3f/%6.3f/%6.3f\n",
	       (int) (((char *) sbrk(0)) - ((char *) track->brk_start)),
	       timeval_subtract(&time_end, &track->time_start),
	       timeval_subtract(&r.ru_utime, &track->user_start),
	       timeval_subtract(&r.ru_stime, &track->system_start));
#else
	printf("Memory used: %d, elapsed time: %6.3f\n",
	       (int) (((char *) sbrk(0)) - ((char *) track->brk_start)),
	       timeval_subtract(&time_end, &track->time_start));
#endif
}
#endif /* RESOURCE_TRACK */

void e2fsck_read_inode(e2fsck_t ctx, unsigned long ino,
			      struct ext2_inode * inode, const char *proc)
{
	int retval;

	retval = ext2fs_read_inode(ctx->fs, ino, inode);
	if (retval) {
		com_err("ext2fs_read_inode", retval,
			"while reading inode %ld in %s", ino, proc);
		fatal_error(ctx, 0);
	}
}

extern void e2fsck_write_inode(e2fsck_t ctx, unsigned long ino,
			       struct ext2_inode * inode, const char *proc)
{
	int retval;

	retval = ext2fs_write_inode(ctx->fs, ino, inode);
	if (retval) {
		com_err("ext2fs_write_inode", retval,
			"while writing inode %ld in %s", ino, proc);
		fatal_error(ctx, 0);
	}
}

#ifdef MTRACE
void mtrace_print(char *mesg)
{
	FILE	*malloc_get_mallstream();
	FILE	*f = malloc_get_mallstream();

	if (f)
		fprintf(f, "============= %s\n", mesg);
}
#endif

blk_t get_backup_sb(ext2_filsys fs)
{
	if (!fs || !fs->super)
		return 8193;
	return fs->super->s_blocks_per_group + 1;
}


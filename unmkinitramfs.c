/* unmkinitramfs: Unpack an initramfs */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <err.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The "new" cpio header supported by the kernel.  All fields after
 * magic hold 32-bit values in hexadecimal.  This is immediately
 * followed by the name, then contents.
 */
struct cpio_new {
	char	c_magic[6];
	char	c_ino[8];
	char	c_mode[8];
	char	c_uid[8];
	char	c_gid[8];
	char	c_nlink[8];
	char	c_mtime[8];
	char	c_filesize[8];
	char	c_dev_maj[8];
	char	c_dev_min[8];
	char	c_rdev_maj[8];
	char	c_rdev_min[8];
	char	c_namesize[8];
	char	c_chksum[8];
} __attribute__((packed));

#define CPIO_NEW_MAGIC		"070701"
#define CPIO_NEW_CRC_MAGIC	"070702"
#define CPIO_MAGIC_LEN		6

/* Name of the last entry in a cpio archive */
#define CPIO_NAME_TRAILER	"TRAILER!!!"

struct cpio_entry {
	const char *	name;
	off_t		start, end;
};

struct cpio_handle {
	FILE *		file;
	const char *	name;
	off_t		next_off;
	char		name_buf[PATH_MAX];
};

struct cpio_proc {
	int	pid;
	int	pipe;
};

enum format {
	FORMAT_CPIO_NEW,
	FORMAT_GZIP,
	FORMAT_BZIP2,
	FORMAT_XZ,
	FORMAT_LZO,
	FORMAT_LZ4,
	FORMAT_ZSTD,
};

#define MAX_MAGIC_LEN		12

struct magic_entry {
	unsigned int		magic_len;
	unsigned char		magic[MAX_MAGIC_LEN];
	enum format		format;
};

#define MAGIC_ENTRY(magic, format)	{ sizeof(magic) - 1, magic, format }

static const struct magic_entry magic_table[] = {
	MAGIC_ENTRY(CPIO_NEW_MAGIC,		FORMAT_CPIO_NEW),
	MAGIC_ENTRY(CPIO_NEW_CRC_MAGIC,		FORMAT_CPIO_NEW),
	MAGIC_ENTRY("\x1f\x8b\x08",		FORMAT_GZIP),
	MAGIC_ENTRY("BZh",			FORMAT_BZIP2),
	MAGIC_ENTRY("\xfd""7zXZ\0",		FORMAT_XZ),
	MAGIC_ENTRY("\x89LZO\0\r\n\x1a\n",	FORMAT_LZO),
	/* lz4 "legacy" format, the only version that the kernel supports */
	MAGIC_ENTRY("\x02\x21\x4c\x18",		FORMAT_LZ4),
	MAGIC_ENTRY("\x28\xb5\x2f\xfd",		FORMAT_ZSTD),
	{ 0 }
};

static const char *const decomp_table[][2] = {
	[FORMAT_GZIP] =		{ "gzip", "-cd" },
	[FORMAT_BZIP2] =	{ "bzip2", "-cd" },
	[FORMAT_XZ] =		{ "xzcat" },
	[FORMAT_LZO] =		{ "lzop", "-cd" },
	[FORMAT_LZ4] =		{ "lz4cat" },
	[FORMAT_ZSTD] =		{ "zstd", "-cdq" },
};

/* mkdir() but return success if name already exists as directory */
static bool mkdir_allow_exist(const char *name, mode_t mode)
{
	struct stat st;
	int orig_errno;

	if (mkdir(name, mode) == 0)
		return true;

	orig_errno = errno;
	if (orig_errno == EEXIST && stat(name, &st) == 0 && S_ISDIR(st.st_mode))
		return true;

	errno = orig_errno;
	return false;
}

/* write() with loop in case of partial writes */
static bool write_all(int fd, const void *buf, size_t len)
{
	size_t pos;
	ssize_t ret;

	pos = 0;
	do {
		ret = write(fd, (const char *)buf + pos, len - pos);
		if (ret < 0)
			return false;
		pos += ret;
	} while (pos < len);

	return true;
}

/*
 * Warn about failure of fread.  This may be due to a file error
 * reported in errno, or EOF which is not.
 */
static void warn_after_fread_failure(FILE *file, const char *name)
{
	if (ferror(file))
		warn("%s", name);
	else
		warnx("%s: unexpected EOF", name);
}

/*
 * Parse one of the hexadecimal fields.  Don't use strtoul() because
 * it requires null-termination.
 */
static bool cpio_parse_hex(const char *field, uint32_t *value_p)
{
	const char digits[] = "0123456789ABCDEF", *p;
	uint32_t value = 0;
	unsigned int i;
	bool found_digit = false;

	/* Skip leading spaces */
	for (i = 0; i < 8 && field[i] == ' '; ++i)
		;

	/* Parse digits up to end of field or null */
	for (; i < 8 && field[i] != 0; ++i) {
		p = strchr(digits, field[i]);
		if (!p)
			return false;
		value = (value << 4) | (p - digits);
		found_digit = true;
	}

	*value_p = value;
	return found_digit;
}

/* Align offset of file contents or header */
static off_t cpio_align(off_t off)
{
	return (off + 3) & ~3;
}

static struct cpio_handle *cpio_open(FILE *file, const char *name)
{
	struct cpio_handle *cpio;

	cpio = calloc(1, sizeof(*cpio));
	if (!cpio)
		return NULL;

	cpio->file = file;
	cpio->name = name;
	cpio->next_off = ftell(file);
	return cpio;
}

/*
 * Read next cpio header and name.
 * Return:
 * -1 on error
 *  0 if entry is trailer
 *  1 if entry is anything else
 */
static int cpio_get_next(struct cpio_handle *cpio, struct cpio_entry *entry)
{
	struct cpio_new header;
	uint32_t file_size, name_size;

	if (fseek(cpio->file, cpio->next_off, SEEK_SET) < 0 ||
	    fread(&header, sizeof(header), 1, cpio->file) != 1) {
		warn_after_fread_failure(cpio->file, cpio->name);
		return -1;
	}

	if ((memcmp(header.c_magic, CPIO_NEW_MAGIC, CPIO_MAGIC_LEN) != 0 &&
	     memcmp(header.c_magic, CPIO_NEW_CRC_MAGIC, CPIO_MAGIC_LEN) != 0) ||
	    !cpio_parse_hex(header.c_filesize, &file_size) ||
	    !cpio_parse_hex(header.c_namesize, &name_size)) {
		warnx("%s: cpio archive has invalid header", cpio->name);
		return -1;
	}

	entry->name = cpio->name_buf;
	entry->start = cpio->next_off;

	/* Calculate offset of the next header */
	cpio->next_off = cpio_align(
		cpio_align(cpio->next_off + sizeof(header) + name_size)
		+ file_size);
	entry->end = cpio->next_off;

	if (name_size > sizeof(cpio->name_buf)) {
		warnx("%s: cpio member name is too long", cpio->name);
		return -1;
	}

	if (fread(cpio->name_buf, name_size, 1, cpio->file) != 1) {
		warn_after_fread_failure(cpio->file, cpio->name);
		return -1;
	}

	if (name_size == 0 || cpio->name_buf[name_size - 1] != 0) {
		warnx("%s: cpio member name is invalid", cpio->name);
		return -1;
	}

	return strcmp(entry->name, CPIO_NAME_TRAILER) != 0;
}

static void cpio_close(struct cpio_handle *cpio)
{
	free(cpio);
}

static bool detect_early_initramfs(FILE *in_file, const char *in_filename)
{
	struct cpio_handle *cpio;
	struct cpio_entry entry;
	bool ret = false;
	off_t start = ftell(in_file);

	cpio = cpio_open(in_file, in_filename);
	if (!cpio)
		return false;

	while (cpio_get_next(cpio, &entry) > 0) {
		if (strncmp(entry.name, "kernel/", 7) == 0) {
			ret = true;
			break;
		}
	}

	cpio_close(cpio);

	fseek(in_file, start, SEEK_SET);
	return ret;
}

static bool copy_to_pipe(FILE *in_file, const char *in_filename,
			 off_t start, off_t end, int out_pipe)
{
	char buf[0x10000];
	off_t in_pos;
	size_t want_len, read_len;

	/* Set input position */
	fseek(in_file, start, SEEK_SET);
	in_pos = start;

	while (in_pos < end) {
		/* How much do we want to copy? */
		want_len = sizeof(buf);
		if ((ssize_t)want_len > end - in_pos)
			want_len = end - in_pos;

		/* Read to buffer; update input position */
		read_len = fread(buf, 1, want_len, in_file);
		if (!read_len) {
			warn_after_fread_failure(in_file, in_filename);
			return false;
		}
		in_pos += read_len;

		/* Write to pipe */
		if (!write_all(out_pipe, buf, read_len)) {
			warn("pipe write");
			return false;
		}
	}

	return true;
}

static bool handle_uncompressed(FILE *in_file, const char *in_filename,
				int out_pipe)
{
	struct cpio_handle *cpio;
	struct cpio_entry entry;
	uint32_t pad;
	int ret;

	cpio = cpio_open(in_file, in_filename);
	if (!cpio)
		return false;

	while ((ret = cpio_get_next(cpio, &entry)) > 0) {
		if (!copy_to_pipe(in_file, in_filename, entry.start, entry.end,
				  out_pipe)) {
			ret = -1;
			break;
		}
	}

	cpio_close(cpio);

	if (ret < 0)
		return false;

	/* Skip trailer and any zero padidng */
	fseek(in_file, entry.end, SEEK_SET);
	while (fread(&pad, sizeof(pad), 1, in_file)) {
		if (pad != 0) {
			fseek(in_file, -sizeof(pad), SEEK_CUR);
			break;
		}
	}

	return true;
}

static bool handle_compressed(FILE *in_file, enum format format, int out_pipe)
{
	const char *const *argv = decomp_table[format];
	int in_fd = fileno(in_file);
	off_t in_pos = ftell(in_file);
	int pid, wstatus;

	pid = fork();
	if (pid < 0)
		return false;

	/* Child */
	if (pid == 0) {
		/*
		 * Make in_file stdin.  Reset the position of the file
		 * descriptor because stdio will have read-ahead from
		 * the position it reported.
		 */
		dup2(in_fd, 0);
		close(in_fd);
		lseek(0, in_pos, SEEK_SET);

		/* Make out_pipe stdout */
		dup2(out_pipe, 1);
		close(out_pipe);

		execlp(argv[0], argv[0], argv[1], NULL);
		_exit(127);
	}

	/* Parent: wait for child */
	if (waitpid(pid, &wstatus, 0) != pid ||
	    !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
		warnx("%s failed", argv[0]);
		return false;
	}
	return true;
}

static bool write_trailer(int out_pipe)
{
	struct {
		struct cpio_new header;
		char name[sizeof(CPIO_NAME_TRAILER)];
		char pad[-(sizeof(struct cpio_new) + sizeof(CPIO_NAME_TRAILER))
			 & 3];
	} __attribute__((packed)) trailer;
	char name_size[8 + 1];

	static_assert((sizeof(trailer) & 3) == 0, "pad miscalculated");

	memset(&trailer.header, '0', sizeof(trailer.header));
	memcpy(trailer.header.c_magic, CPIO_NEW_MAGIC, CPIO_MAGIC_LEN);
	sprintf(name_size, "%08zX", sizeof(CPIO_NAME_TRAILER));
	memcpy(trailer.header.c_namesize, name_size,
	       sizeof(trailer.header.c_namesize));

	strcpy(trailer.name, CPIO_NAME_TRAILER);

	memset(&trailer.pad, 0, sizeof(trailer.pad));

	if (!write_all(out_pipe, &trailer, sizeof(trailer))) {
		warn("pipe write");
		return false;
	}

	return true;
}

static bool spawn_cpio(int optc, const char **optv, struct cpio_proc *proc)
{
	const char *argv[10];
	int pipe_fds[2], pid;
	size_t argc;

	/* Combine base cpio command with extra options */
	argc = 0;
	argv[argc++] = "cpio";
	argv[argc++] = "-i";
	argv[argc++] = "--preserve-modification-time";
	argv[argc++] = "--no-absolute-filenames";
	argv[argc++] = "--quiet";
	assert(argc + optc < sizeof(argv) / sizeof(argv[0]));
	while (optc--)
		argv[argc++] = *optv++;
	argv[argc] = NULL;

	if (pipe(pipe_fds)) {
		warn("pipe");
		return false;
	}
	pid = fork();
	if (pid < 0) {
		warn("fork");
		return false;
	}

	/* Child */
	if (pid == 0) {
		/*
		 * Close write end of the pipe; make the read end
		 * stdout.
		 */
		close(pipe_fds[1]);
		dup2(pipe_fds[0], 0);
		close(pipe_fds[0]);

		execvp("cpio", (char **)argv);
		_exit(127);
	}

	/*
	 * Parent: close read end of the pipe; return child pid and
	 * write end of pipe.
	 */
	close(pipe_fds[0]);
	proc->pid = pid;
	proc->pipe = pipe_fds[1];
	return true;
}

static bool end_cpio(const struct cpio_proc *proc, bool ok)
{
	int wstatus;

	close(proc->pipe);

	if (ok) {
		if (waitpid(proc->pid, &wstatus, 0) != proc->pid ||
		    !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
			warnx("cpio failed");
			return false;
		}
	} else {
		kill(proc->pid, SIGTERM);
	}

	return true;
}

static const struct option long_opts[] = {
	{ "help",	no_argument,	NULL,	'h' },
	{ "list",	no_argument,	NULL,	'l' },
	{ "verbose",	no_argument,	NULL,	'v' },
	{ NULL }
};

static void usage(FILE *stream)
{
	fprintf(stream, "\
\n\
Usage: unmkinitramfs [-v|--verbose] INITRAMFS-FILE DIRECTORY\n\
\n\
Options:\n\
  -v | --verbose   Display verbose messages about extraction\n\
\n\
See unmkinitramfs(8) for further details.\n\
\n"
		);
}

int main(int argc, char **argv)
{
	int opt;
	bool do_list = false;
	bool verbose = false;
	const char *in_filename;
	FILE *in_file;
	const char *out_dirname = NULL;
	char *out_subdirname = NULL;
	const char *cpio_optv[3];
	int cpio_optc;
	struct cpio_proc cpio_proc = { 0 };
	unsigned int early_count = 0;
	bool ok;

	/* Parse options */
	opterr = 0;
	while ((opt = getopt_long(argc, argv, "hv", long_opts, NULL)) >= 0) {
		switch (opt) {
		case '?':
			usage(stderr);
			return 2;
		case 'h':
			usage(stdout);
			return 0;
		case 'l':
			do_list = true;
			break;
		case 'v':
			verbose = true;
			break;
		}
	}

	/* Check number of non-option arguments */
	if (argc - optind != (do_list ? 1 : 2)) {
		usage(stderr);
		return 2;
	}

	/* Set up input file and output directory */
	in_filename = argv[optind];
	in_file = fopen(in_filename, "rb");
	if (!in_file)
		err(1, "%s", in_filename);
	if (!do_list) {
		out_dirname = argv[optind + 1];
		if (!mkdir_allow_exist(out_dirname, 0777))
			err(1, "%s", out_dirname);
		out_subdirname = malloc(strlen(out_dirname) + 20);
		if (!out_subdirname)
			err(1, "malloc");
	}

	/* Set up extra options for cpio */
	cpio_optc = 0;
	if (do_list) {
		cpio_optv[cpio_optc++] = "--list";
	} else {
		cpio_optv[cpio_optc++] = "-D";
		cpio_optv[cpio_optc++] = out_subdirname;
	}
	if (verbose)
		cpio_optv[cpio_optc++] = "-v";

	/* Iterate over archives within the initramfs */
	for (;;) {
		unsigned char magic_buf[MAX_MAGIC_LEN];
		size_t read_len;
		const struct magic_entry *me;

		/* Peek at first bytes of next archive; handle EOF */
		read_len = fread(magic_buf, 1, sizeof(magic_buf), in_file);
		if (read_len == 0) {
			/*
			 * EOF with no compresed archive.  Add back a
			 * trailer to keep cpio happy.
			 */
			if (ferror(in_file)) {
				warn("%s", in_filename);
				ok = false;
			}
			if (cpio_proc.pid && !write_trailer(cpio_proc.pipe))
				ok = false;
			break;
		}
		fseek(in_file, -(long)read_len, SEEK_CUR);

		/* Identify format */
		for (me = magic_table; me->magic_len; ++me)
			if (read_len >= me->magic_len &&
			    memcmp(magic_buf, me->magic, me->magic_len) == 0)
				break;
		if (me->magic_len == 0) {
			warnx("%s: unrecognised compression or corrupted file",
			      in_filename);
			ok = false;
			break;
		}

		/*
		 * Extract the archive to an "early" or "early<n>"
		 * subdirectory if:
		 * - We have not already started the main cpio process
		 * - We are not listing
		 * - This looks like an early initramfs (uncompressed
		 *   and contains files under kernel/)
		 */
		if (!cpio_proc.pid && !do_list &&
		    me->format == FORMAT_CPIO_NEW &&
		    detect_early_initramfs(in_file, in_filename)) {
			struct cpio_proc early_cpio_proc;

			if (++early_count == 1)
				sprintf(out_subdirname, "%s/early",
					out_dirname);
			else
				sprintf(out_subdirname, "%s/early%u",
					out_dirname, early_count);
			if (!mkdir_allow_exist(out_subdirname, 0777)) {
				warn("%s", out_subdirname);
				ok = false;
				break;
			}
			if (!spawn_cpio(cpio_optc, cpio_optv,
					&early_cpio_proc)) {
				ok = false;
				break;
			}
			ok = handle_uncompressed(in_file, in_filename,
						 early_cpio_proc.pipe)
				&& write_trailer(early_cpio_proc.pipe);
			if (!end_cpio(&early_cpio_proc, ok))
				ok = false;
			if (!ok)
				break;
		} else {
			/*
			 * Otherwise, extract to either the base
			 * output directory or a "main" subdirectory,
			 * depending on whether we already created
			 * subdirectories.
			 */
			if (!cpio_proc.pid) {
				if (do_list) {
					;
				} else if (early_count) {
					sprintf(out_subdirname, "%s/main",
						out_dirname);
					if (!mkdir_allow_exist(out_subdirname,
							       0777)) {
						warn("%s", out_subdirname);
						ok = false;
						break;
					}
				} else {
					strcpy(out_subdirname, out_dirname);
				}
				if (!spawn_cpio(cpio_optc, cpio_optv,
						&cpio_proc)) {
					ok = false;
					break;
				}
			}
			if (me->format == FORMAT_CPIO_NEW) {
				ok = handle_uncompressed(in_file, in_filename,
							 cpio_proc.pipe);
				if (!ok)
					break;
			} else {
				ok = handle_compressed(in_file, me->format,
						       cpio_proc.pipe);
				break;
			}
		}
	}

	fclose(in_file);

	if (cpio_proc.pid && !end_cpio(&cpio_proc, ok))
		ok = false;

	return !ok;
}

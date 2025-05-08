/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  Architecture-independent mounting code.

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#include "config.h"
#include "mount_util.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#if !defined( __NetBSD__) && !defined(__FreeBSD__) && !defined(__DragonFly__)
#include <mntent.h>
#else
#define IGNORE_MTAB
#endif
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <stdio.h>
#include <sys/un.h>

#if defined(__NetBSD__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__FreeBSD_kernel__)
#define umount2(mnt, flags) unmount(mnt, ((flags) == 2) ? MNT_FORCE : 0)
#endif

#ifdef IGNORE_MTAB
#define mtab_needs_update(mnt) 0
#else
static int mtab_needs_update(const char *mnt)
{
	int res;
	struct stat stbuf;

	/* If mtab is within new mount, don't touch it */
	if (strncmp(mnt, _PATH_MOUNTED, strlen(mnt)) == 0 &&
	    _PATH_MOUNTED[strlen(mnt)] == '/')
		return 0;

	/*
	 * Skip mtab update if /etc/mtab:
	 *
	 *  - doesn't exist,
	 *  - is a symlink,
	 *  - is on a read-only filesystem.
	 */
	res = lstat(_PATH_MOUNTED, &stbuf);
	if (res == -1) {
		if (errno == ENOENT)
			return 0;
	} else {
		uid_t ruid;
		int err;

		if (S_ISLNK(stbuf.st_mode))
			return 0;

		ruid = getuid();
		if (ruid != 0)
			setreuid(0, -1);

		res = access(_PATH_MOUNTED, W_OK);
		err = (res == -1) ? errno : 0;
		if (ruid != 0)
			setreuid(ruid, -1);

		if (err == EROFS)
			return 0;
	}

	return 1;
}
#endif /* IGNORE_MTAB */

static int add_mount(const char *progname, const char *fsname,
		       const char *mnt, const char *type, const char *opts)
{
	int res;
	int status;
	sigset_t blockmask;
	sigset_t oldmask;

	sigemptyset(&blockmask);
	sigaddset(&blockmask, SIGCHLD);
	res = sigprocmask(SIG_BLOCK, &blockmask, &oldmask);
	if (res == -1) {
		fprintf(stderr, "%s: sigprocmask: %s\n", progname, strerror(errno));
		return -1;
	}

	res = fork();
	if (res == -1) {
		fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
		goto out_restore;
	}
	if (res == 0) {
		char *env = NULL;

		sigprocmask(SIG_SETMASK, &oldmask, NULL);

		if(setuid(geteuid()) == -1) {
			fprintf(stderr, "%s: setuid: %s\n", progname, strerror(errno));
			res = -1;
			goto out_restore;
		}

		execle("/bin/mount", "/bin/mount", "--no-canonicalize", "-i",
		       "-f", "-t", type, "-o", opts, fsname, mnt, NULL, &env);
		fprintf(stderr, "%s: failed to execute /bin/mount: %s\n",
			progname, strerror(errno));
		exit(1);
	}
	res = waitpid(res, &status, 0);
	if (res == -1)
		fprintf(stderr, "%s: waitpid: %s\n", progname, strerror(errno));

	if (status != 0)
		res = -1;

 out_restore:
	sigprocmask(SIG_SETMASK, &oldmask, NULL);

	return res;
}

int fuse_mnt_add_mount(const char *progname, const char *fsname,
		       const char *mnt, const char *type, const char *opts)
{
	if (!mtab_needs_update(mnt))
		return 0;

	return add_mount(progname, fsname, mnt, type, opts);
}

static int exec_umount(const char *progname, const char *rel_mnt, int lazy)
{
	int res;
	int status;
	sigset_t blockmask;
	sigset_t oldmask;

	sigemptyset(&blockmask);
	sigaddset(&blockmask, SIGCHLD);
	res = sigprocmask(SIG_BLOCK, &blockmask, &oldmask);
	if (res == -1) {
		fprintf(stderr, "%s: sigprocmask: %s\n", progname, strerror(errno));
		return -1;
	}

	res = fork();
	if (res == -1) {
		fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
		goto out_restore;
	}
	if (res == 0) {
		char *env = NULL;

		sigprocmask(SIG_SETMASK, &oldmask, NULL);

		if(setuid(geteuid()) == -1) {
			fprintf(stderr, "%s: setuid: %s\n", progname, strerror(errno));
			res = -1;
			goto out_restore;
		}

		if (lazy) {
			execle("/bin/umount", "/bin/umount", "-i", rel_mnt,
			       "-l", NULL, &env);
		} else {
			execle("/bin/umount", "/bin/umount", "-i", rel_mnt,
			       NULL, &env);
		}
		fprintf(stderr, "%s: failed to execute /bin/umount: %s\n",
			progname, strerror(errno));
		exit(1);
	}
	res = waitpid(res, &status, 0);
	if (res == -1)
		fprintf(stderr, "%s: waitpid: %s\n", progname, strerror(errno));

	if (status != 0) {
		res = -1;
	}

 out_restore:
	sigprocmask(SIG_SETMASK, &oldmask, NULL);
	return res;

}

int fuse_mnt_umount(const char *progname, const char *abs_mnt,
		    const char *rel_mnt, int lazy)
{
	int res;

	if (!mtab_needs_update(abs_mnt)) {
		res = umount2(rel_mnt, lazy ? 2 : 0);
		if (res == -1)
			fprintf(stderr, "%s: failed to unmount %s: %s\n",
				progname, abs_mnt, strerror(errno));
		return res;
	}

	return exec_umount(progname, rel_mnt, lazy);
}

static int remove_mount(const char *progname, const char *mnt)
{
	int res;
	int status;
	sigset_t blockmask;
	sigset_t oldmask;

	sigemptyset(&blockmask);
	sigaddset(&blockmask, SIGCHLD);
	res = sigprocmask(SIG_BLOCK, &blockmask, &oldmask);
	if (res == -1) {
		fprintf(stderr, "%s: sigprocmask: %s\n", progname, strerror(errno));
		return -1;
	}

	res = fork();
	if (res == -1) {
		fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
		goto out_restore;
	}
	if (res == 0) {
		char *env = NULL;

		sigprocmask(SIG_SETMASK, &oldmask, NULL);

		if(setuid(geteuid()) == -1) {
			fprintf(stderr, "%s: setuid: %s\n", progname, strerror(errno));
			res = -1;
			goto out_restore;
		}

		execle("/bin/umount", "/bin/umount", "--no-canonicalize", "-i",
		       "--fake", mnt, NULL, &env);
		fprintf(stderr, "%s: failed to execute /bin/umount: %s\n",
			progname, strerror(errno));
		exit(1);
	}
	res = waitpid(res, &status, 0);
	if (res == -1)
		fprintf(stderr, "%s: waitpid: %s\n", progname, strerror(errno));

	if (status != 0)
		res = -1;

 out_restore:
	sigprocmask(SIG_SETMASK, &oldmask, NULL);
	return res;
}

int fuse_mnt_remove_mount(const char *progname, const char *mnt)
{
	if (!mtab_needs_update(mnt))
		return 0;

	return remove_mount(progname, mnt);
}

char *fuse_mnt_resolve_path(const char *progname, const char *orig)
{
	char buf[PATH_MAX];
	char *copy;
	char *dst;
	char *end;
	char *lastcomp;
	const char *toresolv;

	if (!orig[0]) {
		fprintf(stderr, "%s: invalid mountpoint '%s'\n", progname,
			orig);
		return NULL;
	}

	copy = strdup(orig);
	if (copy == NULL) {
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
		return NULL;
	}

	toresolv = copy;
	lastcomp = NULL;
	for (end = copy + strlen(copy) - 1; end > copy && *end == '/'; end --);
	if (end[0] != '/') {
		char *tmp;
		end[1] = '\0';
		tmp = strrchr(copy, '/');
		if (tmp == NULL) {
			lastcomp = copy;
			toresolv = ".";
		} else {
			lastcomp = tmp + 1;
			if (tmp == copy)
				toresolv = "/";
		}
		if (strcmp(lastcomp, ".") == 0 || strcmp(lastcomp, "..") == 0) {
			lastcomp = NULL;
			toresolv = copy;
		}
		else if (tmp)
			tmp[0] = '\0';
	}
	if (realpath(toresolv, buf) == NULL) {
		fprintf(stderr, "%s: bad mount point %s: %s\n", progname, orig,
			strerror(errno));
		free(copy);
		return NULL;
	}
	if (lastcomp == NULL)
		dst = strdup(buf);
	else {
		dst = (char *) malloc(strlen(buf) + 1 + strlen(lastcomp) + 1);
		if (dst) {
			unsigned buflen = strlen(buf);
			if (buflen && buf[buflen-1] == '/')
				sprintf(dst, "%s%s", buf, lastcomp);
			else
				sprintf(dst, "%s/%s", buf, lastcomp);
		}
	}
	free(copy);
	if (dst == NULL)
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
	return dst;
}

int fuse_mnt_check_fuseblk(void)
{
	char buf[256];
	FILE *f = fopen("/proc/filesystems", "r");
	if (!f)
		return 1;

	while (fgets(buf, sizeof(buf), f))
		if (strstr(buf, "fuseblk\n")) {
			fclose(f);
			return 1;
		}

	fclose(f);
	return 0;
}

int fuse_mnt_parse_fuse_fd(const char *mountpoint)
{
	int fd = -1;
	int len = 0;

	if (sscanf(mountpoint, "/dev/fd/%u%n", &fd, &len) == 1 &&
	    len == strlen(mountpoint)) {
		return fd;
	}

	return -1;
}

volatile sig_atomic_t g_fuse_pause = 0;

// 信号处理函数
void handle_sighup(int signum) {
	fprintf(stderr, "[libfuse] pause by SIGHUP\n");
	g_fuse_pause = 1;
    sleep(3);
	int res = send_fuse_fd();
	if (res != 0) {
		fprintf(stderr, "[helper.c] send fd failed\n");
	}
	// sleep(3);
	// exit(0);
}

#define SOCK_PATH "/tmp/alluxio_send_fuse_fd_socket"

volatile sig_atomic_t g_fuse_fd = -1;

int recv_fuse_fd_from_socket(int socket) {
	struct msghdr msg = {0};
	struct iovec io;
	char buf[1];
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;

	io.iov_base = buf;
	io.iov_len = sizeof(buf);
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	if (recvmsg(socket, &msg, 0) == -1) {
		fprintf(stderr, "[recivie] recvmsg failed\n");
		return -1;
	}
	if (msg.msg_controllen == 0) {
		fprintf(stderr, "[recivie] msg.msg_controllen == 0\n");
	}
	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL) {
		fprintf(stderr, "[recivie] No control message received.\n");
	} else {
		fprintf(stderr, "[recivie] Control message received.\n");
	}
	if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
		g_fuse_fd = *((int *)CMSG_DATA(cmsg));
		fprintf(stderr, "Received file descriptor: %d\n", g_fuse_fd);
	}
    return 0;
}

int recv_fuse_fd() {
	fprintf(stderr, "[recivie] try delete SOCK_PATH=%s\n", SOCK_PATH);
	if (access(SOCK_PATH, F_OK) == 0) {
		if (unlink(SOCK_PATH) == 0) {
			fprintf(stderr, "[recivie] Deleted existing socket file: %s\n", SOCK_PATH);
		} else {
			fprintf(stderr, "[recivie] Failed to delete file");
		}
	}
	int server_socket, client_socket;
	struct sockaddr_un server_addr, client_addr;

	fprintf(stderr, "[recivie] create server socket\n");
	server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_socket == -1) {
		fprintf(stderr, "[recivie] create server socket failed\n");
		return -1;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sun_family = AF_UNIX;
	strcpy(server_addr.sun_path, SOCK_PATH);

	fprintf(stderr, "[recivie] bind address\n");
	if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
		fprintf(stderr, "[recivie] bind address failed\n");
		return -1;
	}

	fprintf(stderr, "[recivie] listen\n");
	if (listen(server_socket, 1) == -1) {
		fprintf(stderr, "[recivie] listen failed\n");
		return -1;
	}

	fprintf(stderr, "[recivie] accept connection\n");
	client_socket = accept(server_socket, NULL, NULL);
	if (client_socket == -1) {
		fprintf(stderr, "[recivie] accept connection failed\n");
		return -1;
	}

	fprintf(stderr, "[recivie] accept fd\n");
	int ret = recv_fuse_fd_from_socket(client_socket);
	fprintf(stderr, "[recivie] fd is %d\n", g_fuse_fd);

	close(client_socket);
	close(server_socket);
	unlink(SOCK_PATH);

	return ret;
}

int send_fuse_fd() {
	int client_socket;
	struct sockaddr_un server_addr;

	fprintf(stderr, "[send] create socket\n");
	client_socket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (client_socket == -1) {
		perror("socket");
		return -1;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sun_family = AF_UNIX;
	strcpy(server_addr.sun_path, SOCK_PATH);

	fprintf(stderr, "[send] connect\n");
	if (connect(client_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
		perror("connect");
		return -1;
	}

	fprintf(stderr, "[send] send fuse fd to socket, fd = %d\n", g_fuse_fd);
	int ret = send_fuse_fd_to_socket(client_socket);
	fprintf(stderr, "[send] send fuse fd to socket success\n", g_fuse_fd);

	close(client_socket);

	return ret;
}

int send_fuse_fd_to_socket(int sock_fd)
{
	int fd = g_fuse_fd;
    int retval;
	struct msghdr msg;
	struct cmsghdr *p_cmsg;
	struct iovec vec;
	size_t cmsgbuf[CMSG_SPACE(sizeof(fd)) / sizeof(size_t)];
	int *p_fds;
	char sendchar = 0;

	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);
	p_cmsg = CMSG_FIRSTHDR(&msg);
	p_cmsg->cmsg_level = SOL_SOCKET;
	p_cmsg->cmsg_type = SCM_RIGHTS;
	p_cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
	p_fds = (int *) CMSG_DATA(p_cmsg);
	*p_fds = fd;
	msg.msg_controllen = sizeof(cmsgbuf);
	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &vec;
	msg.msg_iovlen = 1;
	msg.msg_flags = 0;
	/* "To pass file descriptors or credentials you need to send/read at
	 * least one byte" (man 7 unix) */
	vec.iov_base = &sendchar;
	vec.iov_len = sizeof(sendchar);
	while ((retval = sendmsg(sock_fd, &msg, 0)) == -1 && errno == EINTR);
	if (retval != 1) {
		perror("sending file descriptor");
		return -1;
	}
	return 0;
}

int do_recv_fuse_fd_from_socket_by_env() {
    int ret = 0;
	fprintf(stderr, "[libfuse] chechk env RECV_FUSE_FD_FROM_SOCKET");
	const char *env = getenv("RECV_FUSE_FD_FROM_SOCKET");
	if (env != NULL) {
		fprintf(stderr, "[libfuse] RECV_FUSE_FD_FROM_SOCKET is set\n");
		ret = recv_fuse_fd();
		fprintf(stderr, "[recivie] recivie fd from socket, fd = %d\n", g_fuse_fd);
		verify_fuse_fd(g_fuse_fd);
	} else {
		fprintf(stderr, "[libfuse] RECV_FUSE_FD_FROM_SOCKET is not set\n");
	}
    return ret;
}

void verify_fuse_fd(int fd) {
	if (fcntl(fd, F_GETFD) == -1) {
		fprintf(stderr, "[verify] Received fd is invalid\n");
        return;
	}
	struct stat st;
	if (fstat(fd, &st) == -1) {
		perror("fstat");
		return;
	}

	fprintf(stderr, "[verify] FD = %d\n", fd);
	fprintf(stderr, "[verify] st_mode = 0%o\n", st.st_mode);
	fprintf(stderr, "[verify] st_rdev = %ld\n", (long)st.st_rdev);
	fprintf(stderr, "[verify] st_dev = %ld, st_ino = %ld\n", (long)st.st_dev, (long)st.st_ino);

	if (S_ISCHR(st.st_mode)) {
		fprintf(stderr, "[verify] FD is a character device\n");
	} else {
		fprintf(stderr, "[verify] FD is NOT a character device\n");
	}

	// 比对 /dev/fuse
	int fuse_fd = open("/dev/fuse", O_RDONLY);
	if (fuse_fd == -1) {
		perror("open /dev/fuse");
		return;
	}

	struct stat fuse_stat;
	if (fstat(fuse_fd, &fuse_stat) == -1) {
		perror("fstat /dev/fuse");
		close(fuse_fd);
		return;
	}

	if (st.st_rdev == fuse_stat.st_rdev) {
		fprintf(stderr, "[verify]  FD matches /dev/fuse (rdev = %ld)\n", (long)st.st_rdev);
	} else {
		fprintf(stderr, "[verify] FD does NOT match /dev/fuse (expected rdev = %ld, got rdev = %ld)\n",
				(long)fuse_stat.st_rdev, (long)st.st_rdev);
	}

	close(fuse_fd);
}



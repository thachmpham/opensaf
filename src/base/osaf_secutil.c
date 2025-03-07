/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2014 The OpenSAF Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
 * under the GNU Lesser General Public License Version 2.1, February 1999.
 * The complete license can be accessed from the following location:
 * http://opensource.org/licenses/lgpl-license.php
 * See the Copying file included with the OpenSAF distribution for full
 * licensing terms.
 *
 * Author(s): Ericsson AB
 *
 */

/*
 * This file contains infrastructure for sending, receiving over a UNIX socket
 * This can be used by services to securely get credentials for a client.
 *
 * When initialized on the server side a tiny server thread is created
 * that listen to a named connection oriented UNIX socket. For each incoming
 * connection request it calls a user specified callback with file descriptor
 * and peer credentials.
 *
 * A typical use case is for a library to create the MDS socket and then
 * use the client side function to send the MDS address as a data message.
 * The client credentials are stored on the server side and verified
 * when needed.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <pthread.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include "base/osaf_poll.h"

#include "base/logtrace.h"
#include "base/osaf_secutil.h"

#define DEFAULT_GRP_MEM_BUF_SIZE 16384

static struct group *osaf_getgrent_r(struct group *gbuf, char **buf,
				     size_t *buflen);

// singleton (one per process) callback
static client_auth_data_callback_t client_auth_data_callback;

/**
 * accepts a new client and get its credentials, call the handler
 * @param servsock
 */
static void handle_new_connection(int servsock)
{
	int client_fd;
	struct sockaddr_un remote;
	socklen_t addrlen = sizeof(remote);
	struct ucred client_cred;
	socklen_t ucred_length = sizeof(struct ucred);

	TRACE_ENTER();

	// a client wants to connect, accept
	if ((client_fd = accept(servsock, (struct sockaddr *)&remote,
				&addrlen)) == -1) {
		LOG_ER("accept failed - %s", strerror(errno));
		goto done;
	}

	// get client credentials
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &client_cred,
		       &ucred_length) == -1) {
		LOG_WA("could not get credentials from socket - %s",
		       strerror(errno));
		goto done;
	}

	// wait a while for data to get available on socket
	struct pollfd fds = {.fd = client_fd, .events = POLLIN, .revents = 0};
	int64_t timeout = 10000; // TODO allow configuration?
	int res = osaf_poll(&fds, 1, timeout);

	if (res == 0) {
		TRACE_3("poll timeout % " PRId64 "", timeout);
		// timeout
		goto done;
	}

	// callback with file descriptor and credentials
	if (fds.revents & POLLIN) {
		client_auth_data_callback(client_fd, &client_cred);
	} else
		osafassert(0); // TODO

done:
	// the socket is short lived, close it
	if (client_fd > 0)
		close(client_fd);

	TRACE_LEAVE();
}

/**
 * Single time used helper function to the socket server
 */
static int server_sock_create(const char *pathname)
{
	int server_sockfd;
	socklen_t addrlen;
	struct sockaddr_un unaddr = {0};

	if ((server_sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		LOG_ER("%s: socket failed - %s", __FUNCTION__, strerror(errno));
		return -1;
	}

	int flags = fcntl(server_sockfd, F_GETFD, 0);
	if ((flags < 0) || (flags > 1)) {
		LOG_ER("%s: fcntl F_GETFD failed - %s", __FUNCTION__,
		       strerror(errno));
		close(server_sockfd);
		return -1;
	}

	if (fcntl(server_sockfd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		LOG_ER("%s: fcntl F_SETFD failed - %s", __FUNCTION__,
		       strerror(errno));
		close(server_sockfd);
		return -1;
	}

	unlink(pathname);

	unaddr.sun_family = AF_UNIX;
	strcpy(unaddr.sun_path, pathname);
	addrlen = strlen(unaddr.sun_path) + sizeof(unaddr.sun_family);
	if (bind(server_sockfd, (struct sockaddr *)&unaddr, addrlen) == -1) {
		LOG_ER("%s: bind failed - %s", __FUNCTION__, strerror(errno));
		return -1;
	}

	/* Connecting to the socket object requires read/write permission. */
	if (chmod(pathname, 0777) == -1) {
		LOG_ER("%s: chmod failed - %s", __FUNCTION__, strerror(errno));
		return -1;
	}

	if (listen(server_sockfd, 5) == -1) {
		LOG_ER("%s: listen failed - %s", __FUNCTION__, strerror(errno));
		return -1;
	}

	return server_sockfd;
}

/**
 * main for authentication server
 */
static void *auth_server_main(void *_fd)
{
	int fd = *((int *)_fd);
	struct pollfd fds = {.fd = fd, .events = POLLIN, .revents = 0};

	TRACE_ENTER();

	free(_fd);

	for (;;) {
		osaf_poll(&fds, 1, -1);

		if (fds.revents & POLLIN) {
			handle_new_connection(fd);
		}
	}

	osafassert(0);
	return 0;
}

/**
 * Get all supplementary groups via process ID from /proc/<pid>/status
 * @param pid: this param to get all information of process
 * @return a string containing all group id separated by a space
 *
 * NOTE: The caller has to free the allocated memory returned by this function.
 */
static char *get_supplementary_group_list(pid_t pid)
{
	char process_info_path[255];
	FILE *fd = NULL;

	sprintf(process_info_path, "/proc/%d/status", pid);
	while (true) {
		fd = fopen(process_info_path, "r");
		if (fd != NULL) break;

		if (errno != EINTR) {
			LOG_ER("%s fopen Fail %s", __func__, strerror(errno));
			return NULL;
		}
	}

	size_t line_buf_size = 0;
	char *line_buf = NULL;
	char *group_list = NULL;

	while (getline(&line_buf, &line_buf_size, fd) != -1) {
		const char *groups = "Groups:";
		size_t groups_len = strlen(groups);
		if (strncmp(line_buf, groups, groups_len) != 0) continue;

		// No suplementary groups.
		if (line_buf[groups_len + 1] == '\0') break;

		// Exclude 'Groups:' string and a following tab
		size_t len = strlen(line_buf) - groups_len - 1;
		assert(len && "Invalid sumplementary group list");
		group_list = (char *)calloc(len, sizeof(char));
		memcpy(group_list, line_buf + groups_len + 1, len);

		// Remove a character 'new line' at the end of the string
		group_list[len - 1] = '\0';
		break;
	}

	free(line_buf);
	fclose(fd);
	return group_list;
}

/*************** public interface follows*************************** */

int osaf_auth_server_create(const char *pathname,
			    client_auth_data_callback_t callback)
{
	pthread_t thread;
	pthread_attr_t attr;

	TRACE_ENTER();

	// callback is singleton
	osafassert(client_auth_data_callback == NULL);

	// save provided callback for later use when clients connect
	client_auth_data_callback = callback;

	// create server socket
	int *fd = calloc(1, sizeof(int));
	*fd = server_sock_create(pathname);

	osafassert(pthread_attr_init(&attr) == 0);
	osafassert(
	    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);

	if (pthread_create(&thread, &attr, auth_server_main, fd) != 0) {
		LOG_ER("pthread_create FAILED: %s", strerror(errno));
		return -1;
	}

	osafassert(pthread_attr_destroy(&attr) == 0);

	TRACE_LEAVE();
	return 0;
}

/* used by server, logging is OK */
bool osaf_user_is_member_of_group(uid_t uid, const char *groupname)
{
	bool ret = false;
	long grpmembufsize = DEFAULT_GRP_MEM_BUF_SIZE;
	char *grpmembuf = malloc(grpmembufsize);
	if (grpmembuf == NULL) {
		LOG_ER("%s: Failed to allocate %ld bytes", __FUNCTION__,
		       grpmembufsize);
		return ret;
	}

	long pwdmembufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (pwdmembufsize < 0)
		pwdmembufsize = 16384;
	char *pwdmembuf = malloc(pwdmembufsize);
	if (pwdmembuf == NULL) {
		LOG_ER("%s: Failed to allocate %ld bytes", __FUNCTION__,
		       pwdmembufsize);
		free(grpmembuf);
		return ret;
	}

	// get group file entry with list of member user names
	// in case error, an error number is returned
	// errno=ERANGE means that grpmembufsize is insufficient
	// realloc grpmembuf with grpmembufsize increase
	// the increase will not surpass maxgrpmembufsize
	struct group gbuf;
	struct group *client_grp;
	long maxgrpmembufsize = DEFAULT_GRP_MEM_BUF_SIZE;
	int grnam_retval = 0;

	grnam_retval = getgrnam_r(groupname, &gbuf, grpmembuf, grpmembufsize,
							&client_grp);
	if (grnam_retval == ERANGE) {
		char *buff_size = NULL;
		buff_size = getenv("MAX_GRP_MEM_BUF_SIZE");

		if (buff_size != NULL) {
			errno = 0;
			maxgrpmembufsize = strtol(buff_size, NULL, 10);
			if (errno == ERANGE || maxgrpmembufsize > UINT16_MAX) {
				TRACE_3("MAX_GRP_MEM_BUF_SIZE=%ld set too large,"
				    " align to %d", maxgrpmembufsize, UINT16_MAX);
				maxgrpmembufsize = UINT16_MAX;
			} else if (maxgrpmembufsize < DEFAULT_GRP_MEM_BUF_SIZE) {
				TRACE_3("MAX_GRP_MEM_BUF_SIZE=%ld set too small,"
				    " align to %d", maxgrpmembufsize,
				    DEFAULT_GRP_MEM_BUF_SIZE);
				maxgrpmembufsize = DEFAULT_GRP_MEM_BUF_SIZE;
			}
		}
	}
	while (grnam_retval == ERANGE && grpmembufsize < maxgrpmembufsize) {
		grpmembufsize += NSS_BUFLEN_GROUP;
		if (grpmembufsize > maxgrpmembufsize)
			grpmembufsize = maxgrpmembufsize;
		char *temp = realloc(grpmembuf, grpmembufsize);

		if (temp == NULL) {
			LOG_ER("%s: realloc failed, %s", __func__,
			       strerror(errno));
			goto done;
		}
		grpmembuf = temp;
		grnam_retval = getgrnam_r(groupname, &gbuf, grpmembuf, grpmembufsize,
									&client_grp);
	}

	if (grnam_retval != 0) {
		LOG_ER("%s: get group file entry failed for '%s' - %s",
				__func__, groupname, strerror(grnam_retval));
		goto done;
	}

	if (client_grp == NULL) {
		LOG_ER("%s: group '%s' does not exist", __FUNCTION__,
		       groupname);
		goto done;
	}

	// get password file entry for user
	struct passwd pbuf;
	struct passwd *client_pwd;
	int pwuid_retval =
	    getpwuid_r(uid, &pbuf, pwdmembuf, pwdmembufsize, &client_pwd);
	if (pwuid_retval != 0) {
		LOG_WA("%s: get password file entry failed for uid=%u - %s",
		       __FUNCTION__, (unsigned)uid, strerror(pwuid_retval));
		goto done;
	}
	if (client_pwd == NULL) {
		LOG_WA("%s: user id %u does not exist", __FUNCTION__,
		       (unsigned)uid);
		goto done;
	}

	// check the primary group of the user
	if (client_pwd->pw_gid == client_grp->gr_gid) {
		ret = true;
		goto done;
	}

	/* loop list of usernames that are members of the group trying find a
	 * match with the specified user name */
	for (char **member = client_grp->gr_mem; *member != NULL; member++) {
		if (strcmp(client_pwd->pw_name, *member) == 0) {
			ret = true;
			goto done;
		}
	}

done:
	free(pwdmembuf);
	free(grpmembuf);
	return ret;
}

bool osaf_pid_has_supplementary_group(pid_t pid, const char *groupname)
{
	struct group *gr = getgrnam(groupname);
	if (gr == NULL) return false;

	gid_t gid = gr->gr_gid;
	char *group_list = get_supplementary_group_list(pid);
	if (group_list == NULL) return false;

	const char *delimiter = " ";
	char *pch = strtok(group_list, delimiter);
	while (pch != NULL && (gid_t)atoi(pch) != gid) {
		pch = strtok(NULL, delimiter);
	}

	free(group_list);
	return pch != NULL;
}

/* used in libraries, do not log. Only trace */
int osaf_auth_server_connect(const char *path, const void *req_buf,
			     size_t req_size, void *resp_buf, size_t resp_size,
			     int64_t timeout)
{
	int sock_fd, len;
	struct sockaddr_un remote = {0};

	TRACE_ENTER();

	if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		TRACE_3("socket failed - %s", strerror(errno));
		return -errno;
	}

	remote.sun_family = AF_UNIX;
	strcpy(remote.sun_path, path);
	len = strlen(remote.sun_path) + sizeof(remote.sun_family);
	if (connect(sock_fd, (struct sockaddr *)&remote, len) == -1) {
		TRACE_3("connect to '%s' failed - %s", remote.sun_path,
			strerror(errno));
		len = -errno;
		goto done;
	}

	if (send(sock_fd, req_buf, req_size, MSG_NOSIGNAL) == -1) {
		TRACE_3("send failed - %s", strerror(errno));
		len = -errno;
		goto done;
	}

	struct pollfd fds = {.fd = sock_fd, .events = POLLIN, .revents = 0};
	int res = osaf_poll(&fds, 1, timeout);

	if (res == 0) {
		TRACE_3("poll timeout %" PRId64 "", timeout);
		len = 0;
		goto done;
	} else if (res == 1) {
		if (fds.revents & POLLIN) {
			if ((len = recv(sock_fd, resp_buf, resp_size, 0)) ==
			    -1) {
				TRACE_3("recv failed - %s", strerror(errno));
				len = -errno;
				goto done;
			}
		} else {
			TRACE_3("revents:%x", fds.revents);
			len = -1;
			goto done;
		}
	} else {
		// osaf_poll has failed (which it shouldn't)
		osafassert(0);
	}

done:
	(void)close(sock_fd); // ignore error, server side normally closes it

	TRACE_LEAVE();
	return len;
}

int osaf_getgrouplist(const char *user, gid_t group, gid_t *groups,
		      int *ngroups)
{

	char *gr_buf = NULL;
	size_t gr_bufsize = 4096;
	struct group gbuf;
	struct group *gr;
	int size = 0;

	if (size < *ngroups)
		groups[size] = group;
	++size;

	setgrent();
	while ((gr = osaf_getgrent_r(&gbuf, &gr_buf, &gr_bufsize)) != NULL) {
		if (gr->gr_gid != group) {
			for (int i = 0; gr->gr_mem[i] != NULL; ++i) {
				if (strcmp(gr->gr_mem[i], user) == 0) {
					if (size < *ngroups)
						groups[size] = gr->gr_gid;
					++size;
					break;
				}
			}
		}
	}
	endgrent();
	free(gr_buf);

	int result = size <= *ngroups ? size : -1;
	*ngroups = size;
	return result;
}

static struct group *osaf_getgrent_r(struct group *gbuf, char **buf,
				     size_t *buflen)
{
	struct group *gbufp = NULL;
	if (*buf == NULL)
		*buf = malloc(*buflen);
	for (;;) {
		if (*buf == NULL) {
			LOG_ER("could not allocate %zu bytes", *buflen);
			break;
		}
		int result = getgrent_r(gbuf, *buf, *buflen, &gbufp);
		if (result != ERANGE)
			break;
		*buflen *= 2;
		char *new_buf = realloc(*buf, *buflen);
		if (new_buf == NULL)
			free(*buf);
		*buf = new_buf;
	}
	return gbufp;
}

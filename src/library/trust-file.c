/*
 * trust-file.c - Functions for working with trust files 
 * Copyright (c) 2020 Red Hat Inc.
 * All Rights Reserved.
 *
 * This software may be freely redistributed and/or modified under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING. If not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor
 * Boston, MA 02110-1335, USA.
 *
 * Authors:
 *   Zoltan Fridrich <zfridric@redhat.com>
 */

#include "config.h"

#include <ctype.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fapolicyd-backend.h"
#include "file.h"
#include "llist.h"
#include "message.h"
#include "trust-file.h"



#define TRUST_FILE_PATH "/etc/fapolicyd/fapolicyd.trust"
#define TRUST_DIR_PATH "/etc/fapolicyd/trust.d/"
#define BUFFER_SIZE 4096+1+1+1+10+1+64+1
#define FILE_READ_FORMAT  "%4096s %lu %64s" // path size SHA256
#define FILE_WRITE_FORMAT "%s %lu %s\n"     // path size SHA256
#define FTW_NOPENFD 1024
#define FTW_FLAGS (FTW_ACTIONRETVAL | FTW_PHYS)

#define HEADER1 "# This file contains a list of trusted files\n"
#define HEADER2 "#\n"
#define HEADER3 "#  FULL PATH        SIZE                             SHA256\n"
#define HEADER4 "# /home/user/my-ls 157984 61a9960bf7d255a85811f4afcac51067b8f2e4c75e21cf4f2af95319d4ed1b87\n"



list_t _list;
char *_path;
int _count;



/**
 * Take a path and create a string that is ready to be written to the disk.
 *
 * @param path Path to create a string from
 * @param count The count variable is used to select which format to use.
 *    non-zero = trust db format, zero = lmdb format.
 *    Writes the size of the path string into the \p count at the end
 * @return Path string ready to be written to the disk or NULL on error 
 */
static char *make_path_string(const char *path, int *count)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		msg(LOG_ERR, "Cannot open %s", path);
		return NULL;
	}

	// Get the size
	struct stat sb;
	if (fstat(fd, &sb)) {
		msg(LOG_ERR, "Cannot stat %s", path);
		close(fd);
		return NULL;
	}

	// Get the hash
	char *hash = get_hash_from_fd(fd);
	close(fd);

	// Format the output
	char *line;
	*count = *count ?
		asprintf(&line, FILE_WRITE_FORMAT, path, sb.st_size, hash) :
		asprintf(&line, DATA_FORMAT, 0, sb.st_size, hash);
	free(hash);

	if (*count < 0) {
		msg(LOG_ERR, "Cannot format entry for %s", path);
		return NULL;
	}
	return line;
}

/**
 * Write a list into a file
 *
 * @param list List to write into a file
 * @param dest Destination file
 * @return 0 on success, 1 on error
 */
static int write_out_list(list_t *list, const char *dest)
{
	FILE *f = fopen(dest, "w");
	if (!f) {
		msg(LOG_ERR, "Cannot delete %s", dest);
		list_empty(list);
		return 1;
	}

	size_t hlen = strlen(HEADER1);
	fwrite(HEADER1, hlen, 1, f);
	hlen = strlen(HEADER2);
	fwrite(HEADER2, hlen, 1, f);
	hlen = strlen(HEADER3);
	fwrite(HEADER3, hlen, 1, f);
	hlen = strlen(HEADER4);
	fwrite(HEADER4, hlen, 1, f);

	for (list_item_t *lptr = list->first; lptr; lptr = lptr->next) {
		char buf[BUFFER_SIZE + 1];
		char *str = (char *)(lptr->data);
		hlen = snprintf(buf, sizeof(buf), "%s %s\n", (char *)lptr->index, str + 2);
		fwrite(buf, hlen, 1, f);
	}

	fclose(f);
	return 0;
}



int trust_file_append(const char *fpath, const list_t *list) {
	int fd = open(fpath, O_CREAT | O_WRONLY | O_APPEND, 0600);
	if (fd == -1) {
		msg(LOG_ERR, "Cannot open %s", fpath);
		return 1;
	}

	for (list_item_t *lptr = list->first; lptr; lptr = lptr->next) {
		int count = 1;
		char *line = make_path_string(lptr->index, &count);
		if (!line)
			continue;

		if (write(fd, line, count) == -1) {
			msg(LOG_ERR, "failed writing to %s\n", fpath);
			free(line);
			close(fd);
			return 2;
		}
		free(line);
	}

	close(fd);
	return 0;
}

int trust_file_load(const char *fpath, list_t *list)
{
	FILE *file = fopen(fpath, "r");
	if (!file) {
		msg(LOG_ERR, "Cannot open %s", fpath);
		return 1;
	}

	char buffer[BUFFER_SIZE];
	while (fgets(buffer, BUFFER_SIZE, file)) {
		char name[4097], sha[65], *index, *data;
		unsigned long sz;
		unsigned int tsource = SRC_FILE_DB;

		if (iscntrl(buffer[0]) || buffer[0] == '#')
			continue;

		if (sscanf(buffer, FILE_READ_FORMAT, name, &sz, sha) != 3) {
			msg(LOG_WARNING, "Can't parse %s", buffer);
			fclose(file);
			return 2;
		}

		if (asprintf(&data, DATA_FORMAT, tsource, sz, sha) == -1)
			data = NULL;

		index = strdup(name);

		if (!index || !data) {
			free(index);
			free(data);
			continue;
		}

		if (list_contains(list, index)) {
			msg(LOG_WARNING, "%s contains a duplicate %s", fpath, index);
			free(index);
			free(data);
			continue;
		}

		if (list_append(list, index, data)) {
			free(index);
			free(data);
		}
	}

	fclose(file);
	return 0;
}

int trust_file_delete_path(const char *fpath, const char *path)
{
	list_t list;
	list_init(&list);
	trust_file_load(fpath, &list);

	int count = 0;
	size_t path_len = strlen(path);
	list_item_t *lptr = list.first, *prev = NULL, *tmp;

	while (lptr) {
		if (!strncmp(lptr->index, path, path_len)) {
			++count;
			tmp = lptr->next;

			if (prev)
				prev->next = lptr->next;
			else
				list.first = lptr->next;
			if (!lptr->next)
				list.last = prev;
			--list.count;
			list_destroy_item(&lptr);

			lptr = tmp;
			continue;
		}
		prev = lptr;
		lptr = lptr->next;
	}

	if (count)
		write_out_list(&list, fpath);

	list_empty(&list);
	return count;
}

int trust_file_update_path(const char *fpath, const char *path)
{
	list_t list;
	list_init(&list);
	trust_file_load(fpath, &list);

	int count = 0;
	size_t path_len = strlen(path);

	for (list_item_t *lptr = list.first; lptr; lptr = lptr->next) {
		if (!strncmp(lptr->index, path, path_len)) {
			int i = 0;
			free((char *)lptr->data);
			lptr->data = make_path_string(lptr->index, &i);
			++count;
		}
	}

	if (count)
		write_out_list(&list, fpath);

	list_empty(&list);
	return count;
}

int trust_file_rm_duplicates(const char *fpath, list_t *list)
{
	FILE *file = fopen(fpath, "r");
	if (!file) {
		msg(LOG_ERR, "Cannot open %s", fpath);
		return 1;
	}

	char buffer[BUFFER_SIZE];
	while (fgets(buffer, BUFFER_SIZE, file)) {
		char thash[65], tpath[4097];
		long unsigned size;

		if (iscntrl(buffer[0]) || buffer[0] == '#')
			continue;

		if (sscanf(buffer, FILE_READ_FORMAT, tpath, &size, thash) != 3) {
			msg(LOG_WARNING, "Can't parse %s", buffer);
			fclose(file);
			return 2;
		}

		list_remove(list, tpath);
		if (list->count == 0)
			break;
	}

	fclose(file);
	return 0;
}



static int ftw_load(const char *fpath,
		const struct stat *sb __attribute__ ((unused)),
		int typeflag,
		struct FTW *ftwbuf __attribute__ ((unused)))
{
	if (typeflag == FTW_F)
		trust_file_load(fpath, &_list);
	return FTW_CONTINUE;
}

static int ftw_delete_path(const char *fpath,
		const struct stat *sb __attribute__ ((unused)),
		int typeflag,
		struct FTW *ftwbuf __attribute__ ((unused)))
{
	if (typeflag == FTW_F)
		_count += trust_file_delete_path(fpath, _path);
	return FTW_CONTINUE;
}

static int ftw_update_path(const char *fpath,
		const struct stat *sb __attribute__ ((unused)),
		int typeflag,
		struct FTW *ftwbuf __attribute__ ((unused)))
{
	if (typeflag == FTW_F)
		_count += trust_file_update_path(fpath, _path);
	return FTW_CONTINUE;
}

static int ftw_rm_duplicates(const char *fpath,
		const struct stat *sb __attribute__ ((unused)),
		int typeflag,
		struct FTW *ftwbuf __attribute__ ((unused)))
{
	if (_list.count == 0)
		return FTW_STOP;
	if (typeflag == FTW_F)
		trust_file_rm_duplicates(fpath, &_list);
	return FTW_CONTINUE;
}



void trust_file_load_all(list_t *list)
{
	list_empty(&_list);
	trust_file_load(TRUST_FILE_PATH, &_list);
	nftw(TRUST_DIR_PATH, &ftw_load, FTW_NOPENFD, FTW_FLAGS);
	list_merge(list, &_list);
}

int trust_file_delete_path_all(const char *path)
{
	_path = strdup(path);
	_count = trust_file_delete_path(TRUST_FILE_PATH, path);
	nftw(TRUST_DIR_PATH, &ftw_delete_path, FTW_NOPENFD, FTW_FLAGS);
	free(_path);
	return _count;
}

int trust_file_update_path_all(const char *path)
{
	_path = strdup(path);
	_count = trust_file_update_path(TRUST_FILE_PATH, path);
	nftw(TRUST_DIR_PATH, &ftw_update_path, FTW_NOPENFD, FTW_FLAGS);
	free(_path);
	return _count;
}

void trust_file_rm_duplicates_all(list_t *list)
{
	list_empty(&_list);
	list_merge(&_list, list);
	trust_file_rm_duplicates(TRUST_FILE_PATH, &_list);
	nftw(TRUST_DIR_PATH, &ftw_rm_duplicates, FTW_NOPENFD, FTW_FLAGS);
	list_merge(list, &_list);
}

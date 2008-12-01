/* database.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation. See http://www.gnu.org/ for details.
 */

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>

#include "apk_defines.h"
#include "apk_package.h"
#include "apk_database.h"
#include "apk_state.h"
#include "apk_applet.h"

struct install_ctx {
	struct apk_database *db;
	struct apk_package *pkg;

	int script;
	struct apk_db_dir *dircache;
	struct hlist_node **file_dir_node;
	struct hlist_node **file_pkg_node;
};

static apk_blob_t pkg_name_get_key(apk_hash_item item)
{
	return APK_BLOB_STR(((struct apk_name *) item)->name);
}

static const struct apk_hash_ops pkg_name_hash_ops = {
	.node_offset = offsetof(struct apk_name, hash_node),
	.get_key = pkg_name_get_key,
	.hash_key = apk_blob_hash,
	.compare = apk_blob_compare,
	.delete_item = (apk_hash_delete_f) apk_name_free,
};

static apk_blob_t pkg_info_get_key(apk_hash_item item)
{
	return APK_BLOB_BUF(((struct apk_package *) item)->csum);
}

static unsigned long csum_hash(apk_blob_t csum)
{
	/* Checksum's highest bits have the most "randomness", use that
	 * directly as hash */
	return *(unsigned long *) csum.ptr;
}

static const struct apk_hash_ops pkg_info_hash_ops = {
	.node_offset = offsetof(struct apk_package, hash_node),
	.get_key = pkg_info_get_key,
	.hash_key = csum_hash,
	.compare = apk_blob_compare,
	.delete_item = (apk_hash_delete_f) apk_pkg_free,
};

static apk_blob_t apk_db_dir_get_key(apk_hash_item item)
{
	return APK_BLOB_STR(((struct apk_db_dir *) item)->dirname);
}

static const struct apk_hash_ops dir_hash_ops = {
	.node_offset = offsetof(struct apk_db_dir, hash_node),
	.get_key = apk_db_dir_get_key,
	.hash_key = apk_blob_hash,
	.compare = apk_blob_compare,
	.delete_item = (apk_hash_delete_f) free,
};

struct apk_name *apk_db_get_name(struct apk_database *db, apk_blob_t name)
{
	struct apk_name *pn;

	pn = (struct apk_name *) apk_hash_get(&db->available.names, name);
	if (pn != NULL)
		return pn;

	pn = calloc(1, sizeof(struct apk_name));
	if (pn == NULL)
		return NULL;

	pn->name = apk_blob_cstr(name);
	apk_hash_insert(&db->available.names, pn);

	return pn;
}

void apk_name_free(struct apk_name *name)
{
	free(name->name);
	free(name);
}

static struct apk_db_dir *apk_db_dir_ref(struct apk_database *db,
					 struct apk_db_dir *dir,
					 int create_dir)
{
	if (dir->refs == 0) {
		if (dir->parent != NULL)
			apk_db_dir_ref(db, dir->parent, create_dir);
		db->installed.stats.dirs++;
		if (create_dir && dir->mode) {
			mkdir(dir->dirname, dir->mode);
			chown(dir->dirname, dir->uid, dir->gid);
		}
	}
	dir->refs++;

	return dir;
}

static void apk_db_dir_unref(struct apk_database *db, struct apk_db_dir *dir)
{
	dir->refs--;
	if (dir->refs > 0)
		return;

	db->installed.stats.dirs--;
	rmdir(dir->dirname);

	if (dir->parent != NULL)
		apk_db_dir_unref(db, dir->parent);
}

static struct apk_db_dir *apk_db_dir_get(struct apk_database *db,
					 apk_blob_t name)
{
	struct apk_db_dir *dir;
	apk_blob_t bparent;
	int i;

	if (name.len && name.ptr[name.len-1] == '/')
		name.len--;

	dir = (struct apk_db_dir *) apk_hash_get(&db->installed.dirs, name);
	if (dir != NULL)
		return dir;

	dir = calloc(1, sizeof(*dir) + name.len + 1);
	memcpy(dir->dirname, name.ptr, name.len);
	dir->dirname[name.len] = 0;
	apk_hash_insert(&db->installed.dirs, dir);

	if (name.len == 0)
		dir->parent = NULL;
	else if (apk_blob_rsplit(name, '/', &bparent, NULL))
		dir->parent = apk_db_dir_get(db, bparent);
	else
		dir->parent = apk_db_dir_get(db, APK_BLOB_NULL);

	if (dir->parent != NULL)
		dir->flags = dir->parent->flags;

	for (i = 0; i < db->protected_paths->num; i++) {
		if (db->protected_paths->item[i][0] == '-' &&
		    strcmp(&db->protected_paths->item[i][1], dir->dirname) == 0)
			dir->flags &= ~APK_DBDIRF_PROTECTED;
		else if (strcmp(db->protected_paths->item[i], dir->dirname) == 0)
			dir->flags |= APK_DBDIRF_PROTECTED;
	}

	return dir;
}

static struct apk_db_file *apk_db_file_new(struct apk_db_dir *dir,
					   apk_blob_t name,
					   struct hlist_node **after)
{
	struct apk_db_file *file;

	file = calloc(1, sizeof(*file) + name.len + 1);
	hlist_add_after(&file->dir_files_list, after);
	file->dir = dir;
	memcpy(file->filename, name.ptr, name.len);
	file->filename[name.len] = 0;

	return file;
}

static void apk_db_file_set_owner(struct apk_database *db,
				  struct apk_db_file *file,
				  struct apk_package *owner,
				  int create_dir,
				  struct hlist_node **after)
{
	if (file->owner != NULL) {
		hlist_del(&file->pkg_files_list, &file->owner->owned_files);
	} else {
		db->installed.stats.files++;
	}
	file->dir = apk_db_dir_ref(db, file->dir, create_dir);
	file->owner = owner;
	hlist_add_after(&file->pkg_files_list, after);
}

static struct apk_db_file *apk_db_file_get(struct apk_database *db,
					   apk_blob_t name,
					   struct install_ctx *ctx)
{
	struct apk_db_dir *dir;
	struct apk_db_file *file;
	struct hlist_node *cur;
	apk_blob_t bdir, bfile;

	dir = NULL;
	if (!apk_blob_rsplit(name, '/', &bdir, &bfile)) {
		dir = apk_db_dir_get(db, APK_BLOB_NULL);
		bfile = name;
	} else if (ctx != NULL && ctx->dircache != NULL) {
		dir = ctx->dircache;
		if (strncmp(dir->dirname, bdir.ptr, bdir.len) != 0 ||
		    dir->dirname[bdir.len] != 0)
			dir = NULL;
	}
	if (dir == NULL)
		dir = apk_db_dir_get(db, bdir);
	if (ctx != NULL && dir != ctx->dircache) {
		ctx->dircache = dir;
		ctx->file_dir_node = hlist_tail_ptr(&dir->files);
	}

	hlist_for_each_entry(file, cur, &dir->files, dir_files_list) {
		if (strncmp(file->filename, bfile.ptr, bfile.len) == 0 &&
		    file->filename[bfile.len] == 0)
			return file;
	}

	file = apk_db_file_new(dir, bfile, ctx->file_dir_node);
	ctx->file_dir_node = &file->dir_files_list.next;

	return file;
}

static struct apk_package *apk_db_pkg_add(struct apk_database *db, struct apk_package *pkg)
{
	struct apk_package *idb;

	idb = apk_hash_get(&db->available.packages, APK_BLOB_BUF(pkg->csum));
	if (idb == NULL) {
		idb = pkg;
		pkg->id = db->pkg_id++;
		apk_hash_insert(&db->available.packages, pkg);
		*apk_package_array_add(&pkg->name->pkgs) = pkg;
	} else {
		idb->repos |= pkg->repos;
		apk_pkg_free(pkg);
	}
	return idb;
}

static int apk_db_index_read(struct apk_database *db, struct apk_istream *is, int repo)
{
	struct apk_package *pkg = NULL;
	struct apk_db_dir *dir = NULL;
	struct apk_db_file *file = NULL;
	struct hlist_node **file_dir_node = NULL;
	struct hlist_node **file_pkg_node = NULL;

	char buf[1024];
	apk_blob_t l, r;
	int n, field;

	r = APK_BLOB_PTR_LEN(buf, 0);
	while (1) {
		n = is->read(is, &r.ptr[r.len], sizeof(buf) - r.len);
		if (n <= 0)
			break;
		r.len += n;

		while (apk_blob_splitstr(r, "\n", &l, &r)) {
			if (l.len < 2 || l.ptr[1] != ':') {
				if (pkg == NULL)
					continue;

				if (repo != -1)
					pkg->repos |= BIT(repo);
				else
					apk_pkg_set_state(db, pkg, APK_STATE_INSTALL);

				if (apk_db_pkg_add(db, pkg) != pkg && repo == -1) {
					apk_error("Installed database load failed");
					return -1;
				}
				pkg = NULL;
				continue;
			}

			/* Get field */
			field = l.ptr[0];
			l.ptr += 2;
			l.len -= 2;

			/* If no package, create new */
			if (pkg == NULL) {
				pkg = apk_pkg_new();
				dir = NULL;
				file_dir_node = NULL;
				file_pkg_node = hlist_tail_ptr(&pkg->owned_files);
			}

			/* Standard index line? */
			if (apk_pkg_add_info(db, pkg, field, l) == 0)
				continue;

			if (repo != -1) {
				apk_error("Invalid index entry '%c'", field);
				return -1;
			}

			/* Check FDB special entries */
			switch (field) {
			case 'F':
				if (pkg->name == NULL) {
					apk_error("FDB directory entry before package entry");
					return -1;
				}
				dir = apk_db_dir_get(db, l);
				file_dir_node = hlist_tail_ptr(&dir->files);
				break;
			case 'M':
				if (dir == NULL) {
					apk_error("FDB directory metadata entry before directory entry");
					return -1;
				}
				sscanf(l.ptr, "%d:%d:%o", &dir->uid, &dir->gid, &dir->mode);
				break;
			case 'R':
				if (dir == NULL) {
					apk_error("FDB file entry before directory entry");
					return -1;
				}
				file = apk_db_file_new(dir, l, file_dir_node);
				apk_db_file_set_owner(db, file, pkg, FALSE, file_pkg_node);
				file_dir_node = &file->dir_files_list.next;
				file_pkg_node = &file->pkg_files_list.next;
				break;
			case 'Z':
				if (file == NULL) {
					apk_error("FDB checksum entry before file entry");
					return -1;
				}
				if (apk_hexdump_parse(APK_BLOB_BUF(file->csum), l)) {
					apk_error("Not a valid checksum");
					return -1;
				}
				break;
			default:
				apk_error("FDB entry '%c' unsupported", n);
				return -1;
			}
		}

		memcpy(&buf[0], r.ptr, r.len);
		r = APK_BLOB_PTR_LEN(buf, r.len);
	}

	return 0;
}

static int apk_db_write_fdb(struct apk_database *db, struct apk_ostream *os)
{
	struct apk_package *pkg;
	struct apk_db_dir *dir;
	struct apk_db_file *file;
	struct hlist_node *c2;
	char buf[1024];
	apk_blob_t blob;
	int n;

	list_for_each_entry(pkg, &db->installed.packages, installed_pkgs_list) {
		blob = apk_pkg_format_index_entry(pkg, sizeof(buf), buf);
		if (blob.ptr)
			os->write(os, blob.ptr, blob.len - 1);

		dir = NULL;
		hlist_for_each_entry(file, c2, &pkg->owned_files, pkg_files_list) {
			if (file->owner == NULL)
				continue;

			n = 0;
			if (dir != file->dir) {
				dir = file->dir;
				n += snprintf(&buf[n], sizeof(buf)-n,
					      "F:%s\n"
					      "M:%d:%d:%o\n",
					      dir->dirname,
					      dir->uid, dir->gid, dir->mode);
			}

			n += snprintf(&buf[n], sizeof(buf)-n,
				      "R:%s\n",
				      file->filename);
			if (csum_valid(file->csum)) {
				n += snprintf(&buf[n], sizeof(buf)-n, "Z:");
				n += apk_hexdump_format(sizeof(buf)-n, &buf[n],
							APK_BLOB_BUF(file->csum));
				n += snprintf(&buf[n], sizeof(buf)-n, "\n");
			}

			if (os->write(os, buf, n) != n)
				return -1;
		}
		os->write(os, "\n", 1);
	}

	return 0;
}

struct apk_script_header {
	csum_t csum;
	unsigned int type;
	unsigned int size;
};

static int apk_db_scriptdb_write(struct apk_database *db, struct apk_ostream *os)
{
	struct apk_package *pkg;
	struct apk_script *script;
	struct apk_script_header hdr;
	struct hlist_node *c2;

	list_for_each_entry(pkg, &db->installed.packages, installed_pkgs_list) {
		hlist_for_each_entry(script, c2, &pkg->scripts, script_list) {
			memcpy(hdr.csum, pkg->csum, sizeof(csum_t));
			hdr.type = script->type;
			hdr.size = script->size;

			if (os->write(os, &hdr, sizeof(hdr)) != sizeof(hdr))
				return -1;

			if (os->write(os, script->script, script->size) != script->size)
				return -1;
		}
	}

	return 0;
}

static int apk_db_scriptdb_read(struct apk_database *db, struct apk_istream *is)
{
	struct apk_package *pkg;
	struct apk_script_header hdr;

	while (is->read(is, &hdr, sizeof(hdr)) == sizeof(hdr)) {
		pkg = apk_db_get_pkg(db, hdr.csum);
		if (pkg != NULL)
			apk_pkg_add_script(pkg, is, hdr.type, hdr.size);
	}

	return 0;
}

int apk_db_create(const char *root)
{
	apk_blob_t deps = APK_BLOB_STR("busybox, alpine-baselayout, "
				       "apk-tools, alpine-conf\n");
	int fd;

	fchdir(apk_cwd_fd);
	chdir(root);

	mkdir("tmp", 01777);
	mkdir("dev", 0755);
	mknod("dev/null", 0666, makedev(1, 3));
	mkdir("var", 0755);
	mkdir("var/lib", 0755);
	mkdir("var/lib/apk", 0755);

	fd = creat("var/lib/apk/world", 0600);
	if (fd < 0)
		return -1;
	write(fd, deps.ptr, deps.len);
	close(fd);

	return 0;
}

static int apk_db_read_state(struct apk_database *db)
{
	struct apk_istream *is;
	apk_blob_t blob;

	if (db->root == NULL)
		return 0;

	/* Read:
	 * 1. installed repository
	 * 2. source repositories
	 * 3. master dependencies
	 * 4. package statuses
	 * 5. files db
	 * 6. script db
	 */
	fchdir(db->root_fd);

	blob = apk_blob_from_file("var/lib/apk/world");
	if (APK_BLOB_IS_NULL(blob)) {
		apk_error("Please run 'apk create' to initialize root");
		return -1;
	}
	apk_deps_parse(db, &db->world, blob);
	free(blob.ptr);

	is = apk_istream_from_file("var/lib/apk/installed");
	if (is != NULL) {
		apk_db_index_read(db, is, -1);
		is->close(is);
	}

	is = apk_istream_from_file("var/lib/apk/scripts");
	if (is != NULL) {
		apk_db_scriptdb_read(db, is);
		is->close(is);
	}

	return 0;
}

static int add_protected_path(void *ctx, apk_blob_t blob)
{
	struct apk_database *db = (struct apk_database *) ctx;

	*apk_string_array_add(&db->protected_paths) = apk_blob_cstr(blob);
	return 0;
}

int apk_db_open(struct apk_database *db, const char *root)
{
	apk_blob_t blob;

	memset(db, 0, sizeof(*db));
	apk_hash_init(&db->available.names, &pkg_name_hash_ops, 1000);
	apk_hash_init(&db->available.packages, &pkg_info_hash_ops, 4000);
	apk_hash_init(&db->installed.dirs, &dir_hash_ops, 1000);
	list_init(&db->installed.packages);

	if (root != NULL) {
		db->root = strdup(root);
		db->root_fd = open(root, O_RDONLY);
		if (db->root_fd < 0) {
			apk_error("%s: %s", root, strerror(errno));
			free(db->root);
			return -1;
		}
	}

	blob = APK_BLOB_STR("etc:-etc/init.d");
	apk_blob_for_each_segment(blob, ":", add_protected_path, db);

	if (apk_db_read_state(db) != 0)
		return -1;

	fchdir(db->root_fd);
	blob = apk_blob_from_file("etc/apk/repositories");
	if (!APK_BLOB_IS_NULL(blob)) {
		apk_blob_for_each_segment(blob, "\n", apk_db_add_repository, db);
		free(blob.ptr);
	}

	if (apk_repository != NULL)
		apk_db_add_repository(db, APK_BLOB_STR(apk_repository));

	return 0;
}

struct write_ctx {
	struct apk_database *db;
	int fd;
};

static int apk_db_write_config(struct apk_database *db)
{
	struct apk_ostream *os;
	char buf[1024];
	int n;

	if (db->root == NULL)
		return 0;

	fchdir(db->root_fd);

	os = apk_ostream_to_file("var/lib/apk/world", 0600);
	if (os == NULL)
		return -1;
	n = apk_deps_format(buf, sizeof(buf), db->world);
	os->write(os, buf, n);
	os->close(os);

	os = apk_ostream_to_file("var/lib/apk/installed", 0600);
	if (os == NULL)
		return -1;
	apk_db_write_fdb(db, os);
	os->close(os);

	os = apk_ostream_to_file("var/lib/apk/scripts", 0600);
	if (os == NULL)
		return -1;
	apk_db_scriptdb_write(db, os);
	os->close(os);

	return 0;
}

void apk_db_close(struct apk_database *db)
{
	apk_hash_free(&db->available.names);
	apk_hash_free(&db->available.packages);
	apk_hash_free(&db->installed.dirs);
	if (db->root != NULL) {
		close(db->root_fd);
		free(db->root);
	}
}

struct apk_package *apk_db_get_pkg(struct apk_database *db, csum_t sum)
{
	return apk_hash_get(&db->available.packages,
			    APK_BLOB_PTR_LEN((void*) sum, sizeof(csum_t)));
}

struct apk_package *apk_db_pkg_add_file(struct apk_database *db, const char *file)
{
	struct apk_package *info;

	info = apk_pkg_read(db, file);
	if (info != NULL)
		apk_db_pkg_add(db, info);
	return info;
}

static int write_index_entry(apk_hash_item item, void *ctx)
{
	struct apk_ostream *os = (struct apk_ostream *) ctx;
	char buf[1024];
	apk_blob_t blob;

	blob = apk_pkg_format_index_entry(item, sizeof(buf), buf);
	if (APK_BLOB_IS_NULL(blob))
		return 0;

	if (os->write(os, blob.ptr, blob.len) != blob.len)
		return -1;

	return 0;
}

void apk_db_index_write(struct apk_database *db, struct apk_ostream *os)
{
	apk_hash_foreach(&db->available.packages, write_index_entry, (void *) os);
}

int apk_db_add_repository(apk_database_t _db, apk_blob_t repository)
{
	struct apk_database *db = _db.db;
	struct apk_istream *is;
	char tmp[256];
	int r;

	if (db->num_repos >= APK_MAX_REPOS)
		return -1;

	r = db->num_repos++;
	db->repos[r] = (struct apk_repository){
		.url = apk_blob_cstr(repository)
	};

	snprintf(tmp, sizeof(tmp), "%s/APK_INDEX.gz", db->repos[r].url);
	is = apk_istream_from_file_gz(tmp);
	if (is == NULL) {
		apk_error("Failed to open index file %s", tmp);
		return -1;
	}
	apk_db_index_read(db, is, r);
	is->close(is);

	return 0;
}

int apk_db_recalculate_and_commit(struct apk_database *db)
{
	struct apk_state *state;
	int r;

	state = apk_state_new(db);
	r = apk_state_satisfy_deps(state, db->world);
	if (r == 0) {
		r = apk_state_commit(state, db);
		if (r != 0) {
			apk_error("Failed to commit changes");
			return r;
		}
		apk_db_write_config(db);

		apk_message("OK: %d packages, %d dirs, %d files",
			    db->installed.stats.packages,
			    db->installed.stats.dirs,
			    db->installed.stats.files);
	} else {
		apk_error("Failed to build installation graph");
	}
	apk_state_unref(state);

	return r;
}

static int apk_db_install_archive_entry(void *_ctx,
					const struct apk_file_info *ae,
					struct apk_istream *is)
{
	struct install_ctx *ctx = (struct install_ctx *) _ctx;
	struct apk_database *db = ctx->db;
	struct apk_package *pkg = ctx->pkg;
	apk_blob_t name = APK_BLOB_STR(ae->name);
	struct apk_db_dir *dir;
	struct apk_db_file *file;
	struct apk_file_info fi;
	char alt_name[PATH_MAX];
	const char *p;
	int r = 0, type = APK_SCRIPT_INVALID;

	/* Package metainfo and script processing */
	if (ae->name[0] == '.') {
		/* APK 2.0 format */
		if (strcmp(ae->name, ".INSTALL") != 0)
			return 0;
		type = APK_SCRIPT_GENERIC;
	} else if (strncmp(ae->name, "var/db/apk/", 11) == 0) {
		/* APK 1.0 format */
		p = &ae->name[11];
		if (strncmp(p, pkg->name->name, strlen(pkg->name->name)) != 0)
			return 0;
		p += strlen(pkg->name->name) + 1;
		if (strncmp(p, pkg->version, strlen(pkg->version)) != 0)
			return 0;
		p += strlen(pkg->version) + 1;

		type = apk_script_type(p);
		if (type == APK_SCRIPT_INVALID)
			return 0;
	}

	/* Handle script */
	if (type != APK_SCRIPT_INVALID) {
		apk_pkg_add_script(pkg, is, type, ae->size);

		if (type == APK_SCRIPT_GENERIC ||
		    type == ctx->script) {
			r = apk_pkg_run_script(pkg, db->root_fd, ctx->script);
			if (r != 0)
				apk_error("%s-%s: Failed to execute pre-install/upgrade script",
					  pkg->name->name, pkg->version);
		}

		return r;
	}

	/* Installable entry */
	if (ctx->file_pkg_node == NULL)
		ctx->file_pkg_node = hlist_tail_ptr(&pkg->owned_files);

	if (!S_ISDIR(ae->mode)) {
		file = apk_db_file_get(db, name, ctx);
		if (file == NULL)
			return -1;

		if (file->owner != NULL &&
		    file->owner->name != pkg->name &&
		    strcmp(file->owner->name->name, "busybox") != 0) {
			apk_error("%s: Trying to overwrite %s owned by %s.\n",
				  pkg->name->name, ae->name,
				  file->owner->name->name);
			return -1;
		}

		apk_db_file_set_owner(db, file, pkg, TRUE, ctx->file_pkg_node);
		ctx->file_pkg_node = &file->pkg_files_list.next;

		if (strncmp(file->filename, ".keep_", 6) == 0)
			return 0;

		if ((file->dir->flags & APK_DBDIRF_PROTECTED) &&
		    csum_valid(file->csum) &&
		    apk_file_get_info(ae->name, &fi) == 0 &&
		    memcmp(file->csum, fi.csum, sizeof(csum_t)) != 0) {
			/* Protected file, which is modified locally.
			 * Extract to separate place */
			snprintf(alt_name, sizeof(alt_name),
				 "%s/%s.apk-new",
				 dir->dirname, file->filename);
			r = apk_archive_entry_extract(ae, is, alt_name);
		} else {
			r = apk_archive_entry_extract(ae, is, NULL);
		}
		memcpy(file->csum, ae->csum, sizeof(csum_t));
	} else {
		if (name.ptr[name.len-1] == '/')
			name.len--;
		dir = apk_db_dir_get(db, name);
		dir->mode = ae->mode & 07777;
		dir->uid = ae->uid;
		dir->gid = ae->gid;
	}

	return r;
}

static void apk_db_purge_pkg(struct apk_database *db,
			     struct apk_package *pkg)
{
	struct apk_db_file *file;
	struct hlist_node *c, *n;
	char fn[1024];

	hlist_for_each_entry_safe(file, c, n, &pkg->owned_files, pkg_files_list) {
		file->owner = NULL;
		snprintf(fn, sizeof(fn), "%s/%s",
			 file->dir->dirname,
			 file->filename);
		unlink(fn);

		apk_db_dir_unref(db, file->dir);
		__hlist_del(c, &pkg->owned_files.first);

		db->installed.stats.files--;
	}
	apk_pkg_set_state(db, pkg, APK_STATE_NO_INSTALL);
}

int apk_db_install_pkg(struct apk_database *db,
		       struct apk_package *oldpkg,
		       struct apk_package *newpkg)
{
	struct apk_bstream *bs;
	struct install_ctx ctx;
	csum_t csum;
	char file[256];
	int r;

	if (fchdir(db->root_fd) < 0)
		return errno;

	/* Purge the old package if there */
	if (oldpkg != NULL) {
		if (newpkg == NULL) {
			r = apk_pkg_run_script(oldpkg, db->root_fd,
					       APK_SCRIPT_PRE_DEINSTALL);
			if (r != 0)
				return r;
		}
		apk_db_purge_pkg(db, oldpkg);
		if (newpkg == NULL) {
			apk_pkg_run_script(oldpkg, db->root_fd,
					   APK_SCRIPT_POST_DEINSTALL);
			return 0;
		}
	}

	/* Install the new stuff */
	if (newpkg->filename == NULL) {
		snprintf(file, sizeof(file),
			 "%s/%s-%s.apk",
			 db->repos[0].url, newpkg->name->name, newpkg->version);
		bs = apk_bstream_from_url(file);
	} else
		bs = apk_bstream_from_file(newpkg->filename);

	if (bs == NULL) {
		apk_error("%s: %s", file, strerror(errno));
		return errno;
	}

	ctx = (struct install_ctx) {
		.db = db,
		.pkg = newpkg,
		.script = (oldpkg == NULL) ?
			APK_SCRIPT_PRE_INSTALL : APK_SCRIPT_PRE_UPGRADE,
	};
	if (apk_parse_tar_gz(bs, apk_db_install_archive_entry, &ctx) != 0)
		goto err_close;

	bs->close(bs, csum, NULL);

	apk_pkg_set_state(db, newpkg, APK_STATE_INSTALL);

	if (memcmp(csum, newpkg->csum, sizeof(csum)) != 0)
		apk_warning("%s-%s: checksum does not match",
			    newpkg->name->name, newpkg->version);

	r = apk_pkg_run_script(newpkg, db->root_fd,
			       (oldpkg == NULL) ?
			       APK_SCRIPT_POST_INSTALL : APK_SCRIPT_POST_UPGRADE);
	if (r != 0) {
		apk_error("%s-%s: Failed to execute post-install/upgrade script",
			  newpkg->name->name, newpkg->version);
	} else if (apk_quiet) {
		write(STDOUT_FILENO, ".", 1);
	}
	return r;
err_close:
	bs->close(bs, NULL, NULL);
	return -1;
}
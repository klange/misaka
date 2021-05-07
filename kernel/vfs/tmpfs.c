/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2014-2018 K. Lange
 */
#include <stdint.h>
#include <errno.h>
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/tokenize.h>
#include <kernel/tmpfs.h>
#include <kernel/spinlock.h>

#include <kernel/arch/x86_64/mmu.h>

extern uint64_t now(void);

/* 4KB */
#define BLOCKSIZE 0x1000

#define TMPFS_TYPE_FILE 1
#define TMPFS_TYPE_DIR  2
#define TMPFS_TYPE_LINK 3

static char * buf_space = NULL;

static spin_lock_t tmpfs_lock = { 0 };
static spin_lock_t tmpfs_page_lock = { 0 };

struct tmpfs_dir * tmpfs_root = NULL;

static fs_node_t * tmpfs_from_dir(struct tmpfs_dir * d);

static struct tmpfs_file * tmpfs_file_new(char * name) {

	spin_lock(tmpfs_lock);

	struct tmpfs_file * t = malloc(sizeof(struct tmpfs_file));
	t->name = strdup(name);
	t->type = TMPFS_TYPE_FILE;
	t->length = 0;
	t->pointers = 2;
	t->block_count = 0;
	t->mask = 0;
	t->uid = 0;
	t->gid = 0;
	t->atime = now();
	t->mtime = t->atime;
	t->ctime = t->atime;
	t->blocks = malloc(t->pointers * sizeof(char *));
	for (size_t i = 0; i < t->pointers; ++i) {
		t->blocks[i] = NULL;
	}

	spin_unlock(tmpfs_lock);
	return t;
}

static int symlink_tmpfs(fs_node_t * parent, char * target, char * name) {
	struct tmpfs_dir * d = (struct tmpfs_dir *)parent->device;
	printf("Creating tmpfs file (symlink) %s in %s\n", name, d->name);

	spin_lock(tmpfs_lock);
	foreach(f, d->files) {
		struct tmpfs_file * t = (struct tmpfs_file *)f->value;
		if (!strcmp(name, t->name)) {
			spin_unlock(tmpfs_lock);
			return -EEXIST; /* Already exists */
		}
	}
	spin_unlock(tmpfs_lock);

	struct tmpfs_file * t = tmpfs_file_new(name);
	t->type = TMPFS_TYPE_LINK;
	t->target = strdup(target);

	t->mask = 0777;
	t->uid = current_process->user;
	t->gid = current_process->user;

	spin_lock(tmpfs_lock);
	list_insert(d->files, t);
	spin_unlock(tmpfs_lock);

	return 0;
}

static int readlink_tmpfs(fs_node_t * node, char * buf, size_t size) {
	struct tmpfs_file * t = (struct tmpfs_file *)(node->device);
	if (t->type != TMPFS_TYPE_LINK) {
		printf("tmpfs: not a symlink?\n");
		return -1;
	}

	if (size < strlen(t->target) + 1) {
		memcpy(buf, t->target, size-1);
		buf[size-1] = '\0';
		return size-2;
	} else {
		memcpy(buf, t->target, strlen(t->target) + 1);
		return strlen(t->target);
	}
}

static struct tmpfs_dir * tmpfs_dir_new(char * name, struct tmpfs_dir * parent) {
	spin_lock(tmpfs_lock);

	struct tmpfs_dir * d = malloc(sizeof(struct tmpfs_dir));
	d->name = strdup(name);
	d->type = TMPFS_TYPE_DIR;
	d->mask = 0;
	d->uid = 0;
	d->gid = 0;
	d->atime = now();
	d->mtime = d->atime;
	d->ctime = d->atime;
	d->files = list_create();

	spin_unlock(tmpfs_lock);
	return d;
}

static void tmpfs_file_free(struct tmpfs_file * t) {
	if (t->type == TMPFS_TYPE_LINK) {
		printf("tmpfs: bad link free?\n");
		free(t->target);
	}
	for (size_t i = 0; i < t->block_count; ++i) {
		mmu_frame_clear((uintptr_t)t->blocks[i] * 0x1000);
	}
}

static void tmpfs_file_blocks_embiggen(struct tmpfs_file * t) {
	t->pointers *= 2;
	t->blocks = realloc(t->blocks, sizeof(char *) * t->pointers);
}

static char * tmpfs_file_getset_block(struct tmpfs_file * t, size_t blockid, int create) {
	spin_lock(tmpfs_page_lock);

	if (create) {
		spin_lock(tmpfs_lock);
		while (blockid >= t->pointers) {
			tmpfs_file_blocks_embiggen(t);
		}
		while (blockid >= t->block_count) {
			uintptr_t index = mmu_allocate_a_frame();
			t->blocks[t->block_count] = (char*)index;
			t->block_count += 1;
		}
		spin_unlock(tmpfs_lock);
	} else {
		if (blockid >= t->block_count) {
			printf("tmpfs: not enough blocks?\n");
			return NULL;
		}
	}

	union PML * page = mmu_get_page((uintptr_t)buf_space,0);
	/* This should be map_address? */
	page->bits.writable = 1;
	page->bits.user = 0;
	page->bits.page = (uintptr_t)t->blocks[blockid];
	page->bits.present = 1;
	mmu_invalidate((uintptr_t)buf_space);

	return (char *)buf_space;
}


static uint64_t read_tmpfs(fs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
	struct tmpfs_file * t = (struct tmpfs_file *)(node->device);

	t->atime = now();

	uint64_t end;
	if (offset + size > t->length) {
		end = t->length;
	} else {
		end = offset + size;
	}
	uint64_t start_block  = offset / BLOCKSIZE;
	uint64_t end_block    = end / BLOCKSIZE;
	uint64_t end_size     = end - end_block * BLOCKSIZE;
	uint64_t size_to_read = end - offset;
	if (start_block == end_block && offset == end) return 0;
	if (start_block == end_block) {
		void *buf = tmpfs_file_getset_block(t, start_block, 0);
		memcpy(buffer, (uint8_t *)(((uintptr_t)buf) + ((uintptr_t)offset % BLOCKSIZE)), size_to_read);
		spin_unlock(tmpfs_page_lock);
		return size_to_read;
	} else {
		uint64_t block_offset;
		uint64_t blocks_read = 0;
		for (block_offset = start_block; block_offset < end_block; block_offset++, blocks_read++) {
			if (block_offset == start_block) {
				void *buf = tmpfs_file_getset_block(t, block_offset, 0);
				memcpy(buffer, (uint8_t *)(((uint64_t)buf) + ((uintptr_t)offset % BLOCKSIZE)), BLOCKSIZE - (offset % BLOCKSIZE));
				spin_unlock(tmpfs_page_lock);
			} else {
				void *buf = tmpfs_file_getset_block(t, block_offset, 0);
				memcpy(buffer + BLOCKSIZE * blocks_read - (offset % BLOCKSIZE), buf, BLOCKSIZE);
				spin_unlock(tmpfs_page_lock);
			}
		}
		if (end_size) {
			void *buf = tmpfs_file_getset_block(t, end_block, 0);
			memcpy(buffer + BLOCKSIZE * blocks_read - (offset % BLOCKSIZE), buf, end_size);
			spin_unlock(tmpfs_page_lock);
		}
	}
	return size_to_read;
}

static uint64_t write_tmpfs(fs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
	struct tmpfs_file * t = (struct tmpfs_file *)(node->device);

	t->atime = now();
	t->mtime = t->atime;

	uint64_t end;
	if (offset + size > t->length) {
		t->length = offset + size;
	}
	end = offset + size;
	uint64_t start_block  = offset / BLOCKSIZE;
	uint64_t end_block    = end / BLOCKSIZE;
	uint64_t end_size     = end - end_block * BLOCKSIZE;
	uint64_t size_to_read = end - offset;
	if (start_block == end_block) {
		void *buf = tmpfs_file_getset_block(t, start_block, 1);
		memcpy((uint8_t *)(((uint64_t)buf) + ((uintptr_t)offset % BLOCKSIZE)), buffer, size_to_read);
		spin_unlock(tmpfs_page_lock);
		return size_to_read;
	} else {
		uint64_t block_offset;
		uint64_t blocks_read = 0;
		for (block_offset = start_block; block_offset < end_block; block_offset++, blocks_read++) {
			if (block_offset == start_block) {
				void *buf = tmpfs_file_getset_block(t, block_offset, 1);
				memcpy((uint8_t *)(((uint64_t)buf) + ((uintptr_t)offset % BLOCKSIZE)), buffer, BLOCKSIZE - (offset % BLOCKSIZE));
				spin_unlock(tmpfs_page_lock);
			} else {
				void *buf = tmpfs_file_getset_block(t, block_offset, 1);
				memcpy(buf, buffer + BLOCKSIZE * blocks_read - (offset % BLOCKSIZE), BLOCKSIZE);
				spin_unlock(tmpfs_page_lock);
			}
		}
		if (end_size) {
			void *buf = tmpfs_file_getset_block(t, end_block, 1);
			memcpy(buf, buffer + BLOCKSIZE * blocks_read - (offset % BLOCKSIZE), end_size);
			spin_unlock(tmpfs_page_lock);
		}
	}
	return size_to_read;
}

static int chmod_tmpfs(fs_node_t * node, int mode) {
	struct tmpfs_file * t = (struct tmpfs_file *)(node->device);

	/* XXX permissions */
	t->mask = mode;

	return 0;
}

static int chown_tmpfs(fs_node_t * node, int uid, int gid) {
	struct tmpfs_file * t = (struct tmpfs_file *)(node->device);

	t->uid = uid;
	t->gid = gid;

	return 0;
}

static void truncate_tmpfs(fs_node_t * node) {
	struct tmpfs_file * t = (struct tmpfs_file *)(node->device);
	for (size_t i = 0; i < t->block_count; ++i) {
		mmu_frame_clear((uintptr_t)t->blocks[i] * 0x1000);
		t->blocks[i] = 0;
	}
	t->block_count = 0;
	t->length = 0;
	t->mtime = node->atime;
}

static void open_tmpfs(fs_node_t * node, unsigned int flags) {
	struct tmpfs_file * t = (struct tmpfs_file *)(node->device);

	t->atime = now();
}

static fs_node_t * tmpfs_from_file(struct tmpfs_file * t) {
	fs_node_t * fnode = malloc(sizeof(fs_node_t));
	memset(fnode, 0x00, sizeof(fs_node_t));
	fnode->inode = 0;
	strcpy(fnode->name, t->name);
	fnode->device = t;
	fnode->mask = t->mask;
	fnode->uid = t->uid;
	fnode->gid = t->gid;
	fnode->atime = t->atime;
	fnode->ctime = t->ctime;
	fnode->mtime = t->mtime;
	fnode->flags   = FS_FILE;
	fnode->read    = read_tmpfs;
	fnode->write   = write_tmpfs;
	fnode->open    = open_tmpfs;
	fnode->close   = NULL;
	fnode->readdir = NULL;
	fnode->finddir = NULL;
	fnode->chmod   = chmod_tmpfs;
	fnode->chown   = chown_tmpfs;
	fnode->length  = t->length;
	fnode->truncate = truncate_tmpfs;
	fnode->nlink   = 1;
	return fnode;
}

static fs_node_t * tmpfs_from_link(struct tmpfs_file * t) {
	fs_node_t * fnode = tmpfs_from_file(t);
	fnode->flags   |= FS_SYMLINK;
	fnode->readlink = readlink_tmpfs;
	fnode->read     = NULL;
	fnode->write    = NULL;
	fnode->create   = NULL;
	fnode->mkdir    = NULL;
	fnode->readdir  = NULL;
	fnode->finddir  = NULL;
	return fnode;
}

static struct dirent * readdir_tmpfs(fs_node_t *node, uint64_t index) {
	struct tmpfs_dir * d = (struct tmpfs_dir *)node->device;
	uint64_t i = 0;

	if (index == 0) {
		struct dirent * out = malloc(sizeof(struct dirent));
		memset(out, 0x00, sizeof(struct dirent));
		out->ino = 0;
		strcpy(out->name, ".");
		return out;
	}

	if (index == 1) {
		struct dirent * out = malloc(sizeof(struct dirent));
		memset(out, 0x00, sizeof(struct dirent));
		out->ino = 0;
		strcpy(out->name, "..");
		return out;
	}

	index -= 2;

	if (index >= d->files->length) return NULL;

	foreach(f, d->files) {
		if (i == index) {
			struct tmpfs_file * t = (struct tmpfs_file *)f->value;
			struct dirent * out = malloc(sizeof(struct dirent));
			memset(out, 0x00, sizeof(struct dirent));
			out->ino = (uint64_t)t;
			strcpy(out->name, t->name);
			return out;
		} else {
			++i;
		}
	}
	return NULL;
}

static fs_node_t * finddir_tmpfs(fs_node_t * node, char * name) {
	if (!name) return NULL;

	struct tmpfs_dir * d = (struct tmpfs_dir *)node->device;

	spin_lock(tmpfs_lock);

	foreach(f, d->files) {
		struct tmpfs_file * t = (struct tmpfs_file *)f->value;
		if (!strcmp(name, t->name)) {
			spin_unlock(tmpfs_lock);
			switch (t->type) {
				case TMPFS_TYPE_FILE:
					return tmpfs_from_file(t);
				case TMPFS_TYPE_LINK:
					return tmpfs_from_link(t);
				case TMPFS_TYPE_DIR:
					return tmpfs_from_dir((struct tmpfs_dir *)t);
			}
			return NULL;
		}
	}

	spin_unlock(tmpfs_lock);

	return NULL;
}

static int unlink_tmpfs(fs_node_t * node, char * name) {
	struct tmpfs_dir * d = (struct tmpfs_dir *)node->device;
	int i = -1, j = 0;
	spin_lock(tmpfs_lock);

	foreach(f, d->files) {
		struct tmpfs_file * t = (struct tmpfs_file *)f->value;
		if (!strcmp(name, t->name)) {
			tmpfs_file_free(t);
			free(t);
			i = j;
			break;
		}
		j++;
	}

	if (i >= 0) {
		list_remove(d->files, i);
	} else {
		spin_unlock(tmpfs_lock);
		return -ENOENT;
	}

	spin_unlock(tmpfs_lock);
	return 0;
}

static int create_tmpfs(fs_node_t *parent, char *name, uint16_t permission) {
	if (!name) return -EINVAL;

	struct tmpfs_dir * d = (struct tmpfs_dir *)parent->device;

	spin_lock(tmpfs_lock);
	foreach(f, d->files) {
		struct tmpfs_file * t = (struct tmpfs_file *)f->value;
		if (!strcmp(name, t->name)) {
			spin_unlock(tmpfs_lock);
			return -EEXIST; /* Already exists */
		}
	}
	spin_unlock(tmpfs_lock);

	struct tmpfs_file * t = tmpfs_file_new(name);
	t->mask = permission;
	t->uid = current_process->user;
	t->gid = current_process->user;

	spin_lock(tmpfs_lock);
	list_insert(d->files, t);
	spin_unlock(tmpfs_lock);

	return 0;
}

static int mkdir_tmpfs(fs_node_t * parent, char * name, uint16_t permission) {
	if (!name) return -EINVAL;
	if (!strlen(name)) return -EINVAL;

	struct tmpfs_dir * d = (struct tmpfs_dir *)parent->device;

	spin_lock(tmpfs_lock);
	foreach(f, d->files) {
		struct tmpfs_file * t = (struct tmpfs_file *)f->value;
		if (!strcmp(name, t->name)) {
			spin_unlock(tmpfs_lock);
			return -EEXIST; /* Already exists */
		}
	}
	spin_unlock(tmpfs_lock);

	struct tmpfs_dir * out = tmpfs_dir_new(name, d);
	out->mask = permission;
	out->uid  = current_process->user;
	out->gid  = current_process->user;

	spin_lock(tmpfs_lock);
	list_insert(d->files, out);
	spin_unlock(tmpfs_lock);

	return 0;
}

static fs_node_t * tmpfs_from_dir(struct tmpfs_dir * d) {
	fs_node_t * fnode = malloc(sizeof(fs_node_t));
	memset(fnode, 0x00, sizeof(fs_node_t));
	fnode->inode = 0;
	strcpy(fnode->name, "tmp");
	fnode->mask = d->mask;
	fnode->uid  = d->uid;
	fnode->gid  = d->gid;
	fnode->device  = d;
	fnode->atime   = d->atime;
	fnode->mtime   = d->mtime;
	fnode->ctime   = d->ctime;
	fnode->flags   = FS_DIRECTORY;
	fnode->read    = NULL;
	fnode->write   = NULL;
	fnode->open    = NULL;
	fnode->close   = NULL;
	fnode->readdir = readdir_tmpfs;
	fnode->finddir = finddir_tmpfs;
	fnode->create  = create_tmpfs;
	fnode->unlink  = unlink_tmpfs;
	fnode->mkdir   = mkdir_tmpfs;
	fnode->nlink   = 1; /* should be "number of children that are directories + 1" */
	fnode->symlink = symlink_tmpfs;

	fnode->chown   = chown_tmpfs;
	fnode->chmod   = chmod_tmpfs;

	return fnode;
}

fs_node_t * tmpfs_create(char * name) {
	tmpfs_root = tmpfs_dir_new(name, NULL);
	tmpfs_root->mask = 0777;
	tmpfs_root->uid  = 0;
	tmpfs_root->gid  = 0;

	return tmpfs_from_dir(tmpfs_root);
}

fs_node_t * tmpfs_mount(const char * device, const char * mount_path) {
	char * arg = strdup(device);
	char * argv[10];
	int argc = tokenize(arg, ",", argv);

	fs_node_t * fs = tmpfs_create(argv[0]);

	if (argc > 1) {
		if (strlen(argv[1]) < 3) {
			printf("tmpfs: ignoring bad permission option for tmpfs\n");
		} else {
			int mode = ((argv[1][0] - '0') << 6) |
			           ((argv[1][1] - '0') << 3) |
			           ((argv[1][2] - '0') << 0);
			fs->mask = mode;
		}
	}

	free(arg);
	return fs;
}

void tmpfs_register_init(void) {
	buf_space = (void*)valloc(BLOCKSIZE);
	vfs_register("tmpfs", tmpfs_mount);
}


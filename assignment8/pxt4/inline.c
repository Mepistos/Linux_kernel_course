// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (c) 2012 Taobao.
 * Written by Tao Ma <boyu.mt@taobao.com>
 */

#include <linux/iomap.h>
#include <linux/fiemap.h>
#include <linux/iversion.h>

#include "pxt4_jbd3.h"
#include "pxt4.h"
#include "xattr.h"
#include "truncate.h"

#define PXT4_XATTR_SYSTEM_DATA	"data"
#define PXT4_MIN_INLINE_DATA_SIZE	((sizeof(__le32) * PXT4_N_BLOCKS))
#define PXT4_INLINE_DOTDOT_OFFSET	2
#define PXT4_INLINE_DOTDOT_SIZE		4

static int pxt4_get_inline_size(struct inode *inode)
{
	if (PXT4_I(inode)->i_inline_off)
		return PXT4_I(inode)->i_inline_size;

	return 0;
}

static int get_max_inline_xattr_value_size(struct inode *inode,
					   struct pxt4_iloc *iloc)
{
	struct pxt4_xattr_ibody_header *header;
	struct pxt4_xattr_entry *entry;
	struct pxt4_inode *raw_inode;
	void *end;
	int free, min_offs;

	if (!PXT4_INODE_HAS_XATTR_SPACE(inode))
		return 0;

	min_offs = PXT4_SB(inode->i_sb)->s_inode_size -
			PXT4_GOOD_OLD_INODE_SIZE -
			PXT4_I(inode)->i_extra_isize -
			sizeof(struct pxt4_xattr_ibody_header);

	/*
	 * We need to subtract another sizeof(__u32) since an in-inode xattr
	 * needs an empty 4 bytes to indicate the gap between the xattr entry
	 * and the name/value pair.
	 */
	if (!pxt4_test_inode_state(inode, PXT4_STATE_XATTR))
		return PXT4_XATTR_SIZE(min_offs -
			PXT4_XATTR_LEN(strlen(PXT4_XATTR_SYSTEM_DATA)) -
			PXT4_XATTR_ROUND - sizeof(__u32));

	raw_inode = pxt4_raw_inode(iloc);
	header = IHDR(inode, raw_inode);
	entry = IFIRST(header);
	end = (void *)raw_inode + PXT4_SB(inode->i_sb)->s_inode_size;

	/* Compute min_offs. */
	while (!IS_LAST_ENTRY(entry)) {
		void *next = PXT4_XATTR_NEXT(entry);

		if (next >= end) {
			PXT4_ERROR_INODE(inode,
					 "corrupt xattr in inline inode");
			return 0;
		}
		if (!entry->e_value_inum && entry->e_value_size) {
			size_t offs = le16_to_cpu(entry->e_value_offs);
			if (offs < min_offs)
				min_offs = offs;
		}
		entry = next;
	}
	free = min_offs -
		((void *)entry - (void *)IFIRST(header)) - sizeof(__u32);

	if (PXT4_I(inode)->i_inline_off) {
		entry = (struct pxt4_xattr_entry *)
			((void *)raw_inode + PXT4_I(inode)->i_inline_off);

		free += PXT4_XATTR_SIZE(le32_to_cpu(entry->e_value_size));
		goto out;
	}

	free -= PXT4_XATTR_LEN(strlen(PXT4_XATTR_SYSTEM_DATA));

	if (free > PXT4_XATTR_ROUND)
		free = PXT4_XATTR_SIZE(free - PXT4_XATTR_ROUND);
	else
		free = 0;

out:
	return free;
}

/*
 * Get the maximum size we now can store in an inode.
 * If we can't find the space for a xattr entry, don't use the space
 * of the extents since we have no space to indicate the inline data.
 */
int pxt4_get_max_inline_size(struct inode *inode)
{
	int error, max_inline_size;
	struct pxt4_iloc iloc;

	if (PXT4_I(inode)->i_extra_isize == 0)
		return 0;

	error = pxt4_get_inode_loc(inode, &iloc);
	if (error) {
		pxt4_error_inode(inode, __func__, __LINE__, 0,
				 "can't get inode location %lu",
				 inode->i_ino);
		return 0;
	}

	down_read(&PXT4_I(inode)->xattr_sem);
	max_inline_size = get_max_inline_xattr_value_size(inode, &iloc);
	up_read(&PXT4_I(inode)->xattr_sem);

	brelse(iloc.bh);

	if (!max_inline_size)
		return 0;

	return max_inline_size + PXT4_MIN_INLINE_DATA_SIZE;
}

/*
 * this function does not take xattr_sem, which is OK because it is
 * currently only used in a code path coming form pxt4_iget, before
 * the new inode has been unlocked
 */
int pxt4_find_inline_data_nolock(struct inode *inode)
{
	struct pxt4_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct pxt4_xattr_info i = {
		.name_index = PXT4_XATTR_INDEX_SYSTEM,
		.name = PXT4_XATTR_SYSTEM_DATA,
	};
	int error;

	if (PXT4_I(inode)->i_extra_isize == 0)
		return 0;

	error = pxt4_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	error = pxt4_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	if (!is.s.not_found) {
		if (is.s.here->e_value_inum) {
			PXT4_ERROR_INODE(inode, "inline data xattr refers "
					 "to an external xattr inode");
			error = -EFSCORRUPTED;
			goto out;
		}
		PXT4_I(inode)->i_inline_off = (u16)((void *)is.s.here -
					(void *)pxt4_raw_inode(&is.iloc));
		PXT4_I(inode)->i_inline_size = PXT4_MIN_INLINE_DATA_SIZE +
				le32_to_cpu(is.s.here->e_value_size);
	}
out:
	brelse(is.iloc.bh);
	return error;
}

static int pxt4_read_inline_data(struct inode *inode, void *buffer,
				 unsigned int len,
				 struct pxt4_iloc *iloc)
{
	struct pxt4_xattr_entry *entry;
	struct pxt4_xattr_ibody_header *header;
	int cp_len = 0;
	struct pxt4_inode *raw_inode;

	if (!len)
		return 0;

	BUG_ON(len > PXT4_I(inode)->i_inline_size);

	cp_len = len < PXT4_MIN_INLINE_DATA_SIZE ?
			len : PXT4_MIN_INLINE_DATA_SIZE;

	raw_inode = pxt4_raw_inode(iloc);
	memcpy(buffer, (void *)(raw_inode->i_block), cp_len);

	len -= cp_len;
	buffer += cp_len;

	if (!len)
		goto out;

	header = IHDR(inode, raw_inode);
	entry = (struct pxt4_xattr_entry *)((void *)raw_inode +
					    PXT4_I(inode)->i_inline_off);
	len = min_t(unsigned int, len,
		    (unsigned int)le32_to_cpu(entry->e_value_size));

	memcpy(buffer,
	       (void *)IFIRST(header) + le16_to_cpu(entry->e_value_offs), len);
	cp_len += len;

out:
	return cp_len;
}

/*
 * write the buffer to the inline inode.
 * If 'create' is set, we don't need to do the extra copy in the xattr
 * value since it is already handled by pxt4_xattr_ibody_set.
 * That saves us one memcpy.
 */
static void pxt4_write_inline_data(struct inode *inode, struct pxt4_iloc *iloc,
				   void *buffer, loff_t pos, unsigned int len)
{
	struct pxt4_xattr_entry *entry;
	struct pxt4_xattr_ibody_header *header;
	struct pxt4_inode *raw_inode;
	int cp_len = 0;

	if (unlikely(pxt4_forced_shutdown(PXT4_SB(inode->i_sb))))
		return;

	BUG_ON(!PXT4_I(inode)->i_inline_off);
	BUG_ON(pos + len > PXT4_I(inode)->i_inline_size);

	raw_inode = pxt4_raw_inode(iloc);
	buffer += pos;

	if (pos < PXT4_MIN_INLINE_DATA_SIZE) {
		cp_len = pos + len > PXT4_MIN_INLINE_DATA_SIZE ?
			 PXT4_MIN_INLINE_DATA_SIZE - pos : len;
		memcpy((void *)raw_inode->i_block + pos, buffer, cp_len);

		len -= cp_len;
		buffer += cp_len;
		pos += cp_len;
	}

	if (!len)
		return;

	pos -= PXT4_MIN_INLINE_DATA_SIZE;
	header = IHDR(inode, raw_inode);
	entry = (struct pxt4_xattr_entry *)((void *)raw_inode +
					    PXT4_I(inode)->i_inline_off);

	memcpy((void *)IFIRST(header) + le16_to_cpu(entry->e_value_offs) + pos,
	       buffer, len);
}

static int pxt4_create_inline_data(handle_t *handle,
				   struct inode *inode, unsigned len)
{
	int error;
	void *value = NULL;
	struct pxt4_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct pxt4_xattr_info i = {
		.name_index = PXT4_XATTR_INDEX_SYSTEM,
		.name = PXT4_XATTR_SYSTEM_DATA,
	};

	error = pxt4_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	BUFFER_TRACE(is.iloc.bh, "get_write_access");
	error = pxt4_journal_get_write_access(handle, is.iloc.bh);
	if (error)
		goto out;

	if (len > PXT4_MIN_INLINE_DATA_SIZE) {
		value = PXT4_ZERO_XATTR_VALUE;
		len -= PXT4_MIN_INLINE_DATA_SIZE;
	} else {
		value = "";
		len = 0;
	}

	/* Insert the the xttr entry. */
	i.value = value;
	i.value_len = len;

	error = pxt4_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	BUG_ON(!is.s.not_found);

	error = pxt4_xattr_ibody_set(handle, inode, &i, &is);
	if (error) {
		if (error == -ENOSPC)
			pxt4_clear_inode_state(inode,
					       PXT4_STATE_MAY_INLINE_DATA);
		goto out;
	}

	memset((void *)pxt4_raw_inode(&is.iloc)->i_block,
		0, PXT4_MIN_INLINE_DATA_SIZE);

	PXT4_I(inode)->i_inline_off = (u16)((void *)is.s.here -
				      (void *)pxt4_raw_inode(&is.iloc));
	PXT4_I(inode)->i_inline_size = len + PXT4_MIN_INLINE_DATA_SIZE;
	pxt4_clear_inode_flag(inode, PXT4_INODE_EXTENTS);
	pxt4_set_inode_flag(inode, PXT4_INODE_INLINE_DATA);
	get_bh(is.iloc.bh);
	error = pxt4_mark_iloc_dirty(handle, inode, &is.iloc);

out:
	brelse(is.iloc.bh);
	return error;
}

static int pxt4_update_inline_data(handle_t *handle, struct inode *inode,
				   unsigned int len)
{
	int error;
	void *value = NULL;
	struct pxt4_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct pxt4_xattr_info i = {
		.name_index = PXT4_XATTR_INDEX_SYSTEM,
		.name = PXT4_XATTR_SYSTEM_DATA,
	};

	/* If the old space is ok, write the data directly. */
	if (len <= PXT4_I(inode)->i_inline_size)
		return 0;

	error = pxt4_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	error = pxt4_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	BUG_ON(is.s.not_found);

	len -= PXT4_MIN_INLINE_DATA_SIZE;
	value = kzalloc(len, GFP_NOFS);
	if (!value) {
		error = -ENOMEM;
		goto out;
	}

	error = pxt4_xattr_ibody_get(inode, i.name_index, i.name,
				     value, len);
	if (error < 0)
		goto out;

	BUFFER_TRACE(is.iloc.bh, "get_write_access");
	error = pxt4_journal_get_write_access(handle, is.iloc.bh);
	if (error)
		goto out;

	/* Update the xttr entry. */
	i.value = value;
	i.value_len = len;

	error = pxt4_xattr_ibody_set(handle, inode, &i, &is);
	if (error)
		goto out;

	PXT4_I(inode)->i_inline_off = (u16)((void *)is.s.here -
				      (void *)pxt4_raw_inode(&is.iloc));
	PXT4_I(inode)->i_inline_size = PXT4_MIN_INLINE_DATA_SIZE +
				le32_to_cpu(is.s.here->e_value_size);
	pxt4_set_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA);
	get_bh(is.iloc.bh);
	error = pxt4_mark_iloc_dirty(handle, inode, &is.iloc);

out:
	kfree(value);
	brelse(is.iloc.bh);
	return error;
}

static int pxt4_prepare_inline_data(handle_t *handle, struct inode *inode,
				    unsigned int len)
{
	int ret, size, no_expand;
	struct pxt4_inode_info *ei = PXT4_I(inode);

	if (!pxt4_test_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA))
		return -ENOSPC;

	size = pxt4_get_max_inline_size(inode);
	if (size < len)
		return -ENOSPC;

	pxt4_write_lock_xattr(inode, &no_expand);

	if (ei->i_inline_off)
		ret = pxt4_update_inline_data(handle, inode, len);
	else
		ret = pxt4_create_inline_data(handle, inode, len);

	pxt4_write_unlock_xattr(inode, &no_expand);
	return ret;
}

static int pxt4_destroy_inline_data_nolock(handle_t *handle,
					   struct inode *inode)
{
	struct pxt4_inode_info *ei = PXT4_I(inode);
	struct pxt4_xattr_ibody_find is = {
		.s = { .not_found = 0, },
	};
	struct pxt4_xattr_info i = {
		.name_index = PXT4_XATTR_INDEX_SYSTEM,
		.name = PXT4_XATTR_SYSTEM_DATA,
		.value = NULL,
		.value_len = 0,
	};
	int error;

	if (!ei->i_inline_off)
		return 0;

	error = pxt4_get_inode_loc(inode, &is.iloc);
	if (error)
		return error;

	error = pxt4_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto out;

	BUFFER_TRACE(is.iloc.bh, "get_write_access");
	error = pxt4_journal_get_write_access(handle, is.iloc.bh);
	if (error)
		goto out;

	error = pxt4_xattr_ibody_set(handle, inode, &i, &is);
	if (error)
		goto out;

	memset((void *)pxt4_raw_inode(&is.iloc)->i_block,
		0, PXT4_MIN_INLINE_DATA_SIZE);
	memset(ei->i_data, 0, PXT4_MIN_INLINE_DATA_SIZE);

	if (pxt4_has_feature_extents(inode->i_sb)) {
		if (S_ISDIR(inode->i_mode) ||
		    S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode)) {
			pxt4_set_inode_flag(inode, PXT4_INODE_EXTENTS);
			pxt4_ext_tree_init(handle, inode);
		}
	}
	pxt4_clear_inode_flag(inode, PXT4_INODE_INLINE_DATA);

	get_bh(is.iloc.bh);
	error = pxt4_mark_iloc_dirty(handle, inode, &is.iloc);

	PXT4_I(inode)->i_inline_off = 0;
	PXT4_I(inode)->i_inline_size = 0;
	pxt4_clear_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA);
out:
	brelse(is.iloc.bh);
	if (error == -ENODATA)
		error = 0;
	return error;
}

static int pxt4_read_inline_page(struct inode *inode, struct page *page)
{
	void *kaddr;
	int ret = 0;
	size_t len;
	struct pxt4_iloc iloc;

	BUG_ON(!PageLocked(page));
	BUG_ON(!pxt4_has_inline_data(inode));
	BUG_ON(page->index);

	if (!PXT4_I(inode)->i_inline_off) {
		pxt4_warning(inode->i_sb, "inode %lu doesn't have inline data.",
			     inode->i_ino);
		goto out;
	}

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret)
		goto out;

	len = min_t(size_t, pxt4_get_inline_size(inode), i_size_read(inode));
	kaddr = kmap_atomic(page);
	ret = pxt4_read_inline_data(inode, kaddr, len, &iloc);
	flush_dcache_page(page);
	kunmap_atomic(kaddr);
	zero_user_segment(page, len, PAGE_SIZE);
	SetPageUptodate(page);
	brelse(iloc.bh);

out:
	return ret;
}

int pxt4_readpage_inline(struct inode *inode, struct page *page)
{
	int ret = 0;

	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode)) {
		up_read(&PXT4_I(inode)->xattr_sem);
		return -EAGAIN;
	}

	/*
	 * Current inline data can only exist in the 1st page,
	 * So for all the other pages, just set them uptodate.
	 */
	if (!page->index)
		ret = pxt4_read_inline_page(inode, page);
	else if (!PageUptodate(page)) {
		zero_user_segment(page, 0, PAGE_SIZE);
		SetPageUptodate(page);
	}

	up_read(&PXT4_I(inode)->xattr_sem);

	unlock_page(page);
	return ret >= 0 ? 0 : ret;
}

static int pxt4_convert_inline_data_to_extent(struct address_space *mapping,
					      struct inode *inode,
					      unsigned flags)
{
	int ret, needed_blocks, no_expand;
	handle_t *handle = NULL;
	int retries = 0, sem_held = 0;
	struct page *page = NULL;
	unsigned from, to;
	struct pxt4_iloc iloc;

	if (!pxt4_has_inline_data(inode)) {
		/*
		 * clear the flag so that no new write
		 * will trap here again.
		 */
		pxt4_clear_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA);
		return 0;
	}

	needed_blocks = pxt4_writepage_trans_blocks(inode);

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

retry:
	handle = pxt4_journal_start(inode, PXT4_HT_WRITE_PAGE, needed_blocks);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		handle = NULL;
		goto out;
	}

	/* We cannot recurse into the filesystem as the transaction is already
	 * started */
	flags |= AOP_FLAG_NOFS;

	page = grab_cache_page_write_begin(mapping, 0, flags);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	pxt4_write_lock_xattr(inode, &no_expand);
	sem_held = 1;
	/* If some one has already done this for us, just exit. */
	if (!pxt4_has_inline_data(inode)) {
		ret = 0;
		goto out;
	}

	from = 0;
	to = pxt4_get_inline_size(inode);
	if (!PageUptodate(page)) {
		ret = pxt4_read_inline_page(inode, page);
		if (ret < 0)
			goto out;
	}

	ret = pxt4_destroy_inline_data_nolock(handle, inode);
	if (ret)
		goto out;

	if (pxt4_should_dioread_nolock(inode)) {
		ret = __block_write_begin(page, from, to,
					  pxt4_get_block_unwritten);
	} else
		ret = __block_write_begin(page, from, to, pxt4_get_block);

	if (!ret && pxt4_should_journal_data(inode)) {
		ret = pxt4_walk_page_buffers(handle, page_buffers(page),
					     from, to, NULL,
					     do_journal_get_write_access);
	}

	if (ret) {
		unlock_page(page);
		put_page(page);
		page = NULL;
		pxt4_orphan_add(handle, inode);
		pxt4_write_unlock_xattr(inode, &no_expand);
		sem_held = 0;
		pxt4_journal_stop(handle);
		handle = NULL;
		pxt4_truncate_failed_write(inode);
		/*
		 * If truncate failed early the inode might
		 * still be on the orphan list; we need to
		 * make sure the inode is removed from the
		 * orphan list in that case.
		 */
		if (inode->i_nlink)
			pxt4_orphan_del(NULL, inode);
	}

	if (ret == -ENOSPC && pxt4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;

	if (page)
		block_commit_write(page, from, to);
out:
	if (page) {
		unlock_page(page);
		put_page(page);
	}
	if (sem_held)
		pxt4_write_unlock_xattr(inode, &no_expand);
	if (handle)
		pxt4_journal_stop(handle);
	brelse(iloc.bh);
	return ret;
}

/*
 * Try to write data in the inode.
 * If the inode has inline data, check whether the new write can be
 * in the inode also. If not, create the page the handle, move the data
 * to the page make it update and let the later codes create extent for it.
 */
int pxt4_try_to_write_inline_data(struct address_space *mapping,
				  struct inode *inode,
				  loff_t pos, unsigned len,
				  unsigned flags,
				  struct page **pagep)
{
	int ret;
	handle_t *handle;
	struct page *page;
	struct pxt4_iloc iloc;

	if (pos + len > pxt4_get_max_inline_size(inode))
		goto convert;

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	/*
	 * The possible write could happen in the inode,
	 * so try to reserve the space in inode first.
	 */
	handle = pxt4_journal_start(inode, PXT4_HT_INODE, 1);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		handle = NULL;
		goto out;
	}

	ret = pxt4_prepare_inline_data(handle, inode, pos + len);
	if (ret && ret != -ENOSPC)
		goto out;

	/* We don't have space in inline inode, so convert it to extent. */
	if (ret == -ENOSPC) {
		pxt4_journal_stop(handle);
		brelse(iloc.bh);
		goto convert;
	}

	ret = pxt4_journal_get_write_access(handle, iloc.bh);
	if (ret)
		goto out;

	flags |= AOP_FLAG_NOFS;

	page = grab_cache_page_write_begin(mapping, 0, flags);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	*pagep = page;
	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode)) {
		ret = 0;
		unlock_page(page);
		put_page(page);
		goto out_up_read;
	}

	if (!PageUptodate(page)) {
		ret = pxt4_read_inline_page(inode, page);
		if (ret < 0) {
			unlock_page(page);
			put_page(page);
			goto out_up_read;
		}
	}

	ret = 1;
	handle = NULL;
out_up_read:
	up_read(&PXT4_I(inode)->xattr_sem);
out:
	if (handle && (ret != 1))
		pxt4_journal_stop(handle);
	brelse(iloc.bh);
	return ret;
convert:
	return pxt4_convert_inline_data_to_extent(mapping,
						  inode, flags);
}

int pxt4_write_inline_data_end(struct inode *inode, loff_t pos, unsigned len,
			       unsigned copied, struct page *page)
{
	int ret, no_expand;
	void *kaddr;
	struct pxt4_iloc iloc;

	if (unlikely(copied < len) && !PageUptodate(page))
		return 0;

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret) {
		pxt4_std_error(inode->i_sb, ret);
		return ret;
	}

	pxt4_write_lock_xattr(inode, &no_expand);
	BUG_ON(!pxt4_has_inline_data(inode));

	/*
	 * ei->i_inline_off may have changed since pxt4_write_begin()
	 * called pxt4_try_to_write_inline_data()
	 */
	(void) pxt4_find_inline_data_nolock(inode);

	kaddr = kmap_atomic(page);
	pxt4_write_inline_data(inode, &iloc, kaddr, pos, copied);
	kunmap_atomic(kaddr);
	SetPageUptodate(page);
	/* clear page dirty so that writepages wouldn't work for us. */
	ClearPageDirty(page);

	pxt4_write_unlock_xattr(inode, &no_expand);
	brelse(iloc.bh);
	mark_inode_dirty(inode);

	return copied;
}

struct buffer_head *
pxt4_journalled_write_inline_data(struct inode *inode,
				  unsigned len,
				  struct page *page)
{
	int ret, no_expand;
	void *kaddr;
	struct pxt4_iloc iloc;

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret) {
		pxt4_std_error(inode->i_sb, ret);
		return NULL;
	}

	pxt4_write_lock_xattr(inode, &no_expand);
	kaddr = kmap_atomic(page);
	pxt4_write_inline_data(inode, &iloc, kaddr, 0, len);
	kunmap_atomic(kaddr);
	pxt4_write_unlock_xattr(inode, &no_expand);

	return iloc.bh;
}

/*
 * Try to make the page cache and handle ready for the inline data case.
 * We can call this function in 2 cases:
 * 1. The inode is created and the first write exceeds inline size. We can
 *    clear the inode state safely.
 * 2. The inode has inline data, then we need to read the data, make it
 *    update and dirty so that pxt4_da_writepages can handle it. We don't
 *    need to start the journal since the file's metatdata isn't changed now.
 */
static int pxt4_da_convert_inline_data_to_extent(struct address_space *mapping,
						 struct inode *inode,
						 unsigned flags,
						 void **fsdata)
{
	int ret = 0, inline_size;
	struct page *page;

	page = grab_cache_page_write_begin(mapping, 0, flags);
	if (!page)
		return -ENOMEM;

	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode)) {
		pxt4_clear_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA);
		goto out;
	}

	inline_size = pxt4_get_inline_size(inode);

	if (!PageUptodate(page)) {
		ret = pxt4_read_inline_page(inode, page);
		if (ret < 0)
			goto out;
	}

	ret = __block_write_begin(page, 0, inline_size,
				  pxt4_da_get_block_prep);
	if (ret) {
		up_read(&PXT4_I(inode)->xattr_sem);
		unlock_page(page);
		put_page(page);
		pxt4_truncate_failed_write(inode);
		return ret;
	}

	SetPageDirty(page);
	SetPageUptodate(page);
	pxt4_clear_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA);
	*fsdata = (void *)CONVERT_INLINE_DATA;

out:
	up_read(&PXT4_I(inode)->xattr_sem);
	if (page) {
		unlock_page(page);
		put_page(page);
	}
	return ret;
}

/*
 * Prepare the write for the inline data.
 * If the the data can be written into the inode, we just read
 * the page and make it uptodate, and start the journal.
 * Otherwise read the page, makes it dirty so that it can be
 * handle in writepages(the i_disksize update is left to the
 * normal pxt4_da_write_end).
 */
int pxt4_da_write_inline_data_begin(struct address_space *mapping,
				    struct inode *inode,
				    loff_t pos, unsigned len,
				    unsigned flags,
				    struct page **pagep,
				    void **fsdata)
{
	int ret, inline_size;
	handle_t *handle;
	struct page *page;
	struct pxt4_iloc iloc;
	int retries = 0;

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

retry_journal:
	handle = pxt4_journal_start(inode, PXT4_HT_INODE, 1);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto out;
	}

	inline_size = pxt4_get_max_inline_size(inode);

	ret = -ENOSPC;
	if (inline_size >= pos + len) {
		ret = pxt4_prepare_inline_data(handle, inode, pos + len);
		if (ret && ret != -ENOSPC)
			goto out_journal;
	}

	/*
	 * We cannot recurse into the filesystem as the transaction
	 * is already started.
	 */
	flags |= AOP_FLAG_NOFS;

	if (ret == -ENOSPC) {
		pxt4_journal_stop(handle);
		ret = pxt4_da_convert_inline_data_to_extent(mapping,
							    inode,
							    flags,
							    fsdata);
		if (ret == -ENOSPC &&
		    pxt4_should_retry_alloc(inode->i_sb, &retries))
			goto retry_journal;
		goto out;
	}

	page = grab_cache_page_write_begin(mapping, 0, flags);
	if (!page) {
		ret = -ENOMEM;
		goto out_journal;
	}

	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode)) {
		ret = 0;
		goto out_release_page;
	}

	if (!PageUptodate(page)) {
		ret = pxt4_read_inline_page(inode, page);
		if (ret < 0)
			goto out_release_page;
	}
	ret = pxt4_journal_get_write_access(handle, iloc.bh);
	if (ret)
		goto out_release_page;

	up_read(&PXT4_I(inode)->xattr_sem);
	*pagep = page;
	brelse(iloc.bh);
	return 1;
out_release_page:
	up_read(&PXT4_I(inode)->xattr_sem);
	unlock_page(page);
	put_page(page);
out_journal:
	pxt4_journal_stop(handle);
out:
	brelse(iloc.bh);
	return ret;
}

int pxt4_da_write_inline_data_end(struct inode *inode, loff_t pos,
				  unsigned len, unsigned copied,
				  struct page *page)
{
	int ret;

	ret = pxt4_write_inline_data_end(inode, pos, len, copied, page);
	if (ret < 0) {
		unlock_page(page);
		put_page(page);
		return ret;
	}
	copied = ret;

	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold i_mutex.
	 *
	 * But it's important to update i_size while still holding page lock:
	 * page writeout could otherwise come in and zero beyond i_size.
	 */
	if (pos+copied > inode->i_size)
		i_size_write(inode, pos+copied);
	unlock_page(page);
	put_page(page);

	/*
	 * Don't mark the inode dirty under page lock. First, it unnecessarily
	 * makes the holding time of page lock longer. Second, it forces lock
	 * ordering of page lock and transaction start for journaling
	 * filesystems.
	 */
	mark_inode_dirty(inode);

	return copied;
}

#ifdef INLINE_DIR_DEBUG
void pxt4_show_inline_dir(struct inode *dir, struct buffer_head *bh,
			  void *inline_start, int inline_size)
{
	int offset;
	unsigned short de_len;
	struct pxt4_dir_entry_2 *de = inline_start;
	void *dlimit = inline_start + inline_size;

	trace_printk("inode %lu\n", dir->i_ino);
	offset = 0;
	while ((void *)de < dlimit) {
		de_len = pxt4_rec_len_from_disk(de->rec_len, inline_size);
		trace_printk("de: off %u rlen %u name %.*s nlen %u ino %u\n",
			     offset, de_len, de->name_len, de->name,
			     de->name_len, le32_to_cpu(de->inode));
		if (pxt4_check_dir_entry(dir, NULL, de, bh,
					 inline_start, inline_size, offset))
			BUG();

		offset += de_len;
		de = (struct pxt4_dir_entry_2 *) ((char *) de + de_len);
	}
}
#else
#define pxt4_show_inline_dir(dir, bh, inline_start, inline_size)
#endif

/*
 * Add a new entry into a inline dir.
 * It will return -ENOSPC if no space is available, and -EIO
 * and -EEXIST if directory entry already exists.
 */
static int pxt4_add_dirent_to_inline(handle_t *handle,
				     struct pxt4_filename *fname,
				     struct inode *dir,
				     struct inode *inode,
				     struct pxt4_iloc *iloc,
				     void *inline_start, int inline_size)
{
	int		err;
	struct pxt4_dir_entry_2 *de;

	err = pxt4_find_dest_de(dir, inode, iloc->bh, inline_start,
				inline_size, fname, &de);
	if (err)
		return err;

	BUFFER_TRACE(iloc->bh, "get_write_access");
	err = pxt4_journal_get_write_access(handle, iloc->bh);
	if (err)
		return err;
	pxt4_insert_dentry(inode, de, inline_size, fname);

	pxt4_show_inline_dir(dir, iloc->bh, inline_start, inline_size);

	/*
	 * XXX shouldn't update any times until successful
	 * completion of syscall, but too many callers depend
	 * on this.
	 *
	 * XXX similarly, too many callers depend on
	 * pxt4_new_inode() setting the times, but error
	 * recovery deletes the inode, so the worst that can
	 * happen is that the times are slightly out of date
	 * and/or different from the directory change time.
	 */
	dir->i_mtime = dir->i_ctime = current_time(dir);
	pxt4_update_dx_flag(dir);
	inode_inc_iversion(dir);
	return 1;
}

static void *pxt4_get_inline_xattr_pos(struct inode *inode,
				       struct pxt4_iloc *iloc)
{
	struct pxt4_xattr_entry *entry;
	struct pxt4_xattr_ibody_header *header;

	BUG_ON(!PXT4_I(inode)->i_inline_off);

	header = IHDR(inode, pxt4_raw_inode(iloc));
	entry = (struct pxt4_xattr_entry *)((void *)pxt4_raw_inode(iloc) +
					    PXT4_I(inode)->i_inline_off);

	return (void *)IFIRST(header) + le16_to_cpu(entry->e_value_offs);
}

/* Set the final de to cover the whole block. */
static void pxt4_update_final_de(void *de_buf, int old_size, int new_size)
{
	struct pxt4_dir_entry_2 *de, *prev_de;
	void *limit;
	int de_len;

	de = (struct pxt4_dir_entry_2 *)de_buf;
	if (old_size) {
		limit = de_buf + old_size;
		do {
			prev_de = de;
			de_len = pxt4_rec_len_from_disk(de->rec_len, old_size);
			de_buf += de_len;
			de = (struct pxt4_dir_entry_2 *)de_buf;
		} while (de_buf < limit);

		prev_de->rec_len = pxt4_rec_len_to_disk(de_len + new_size -
							old_size, new_size);
	} else {
		/* this is just created, so create an empty entry. */
		de->inode = 0;
		de->rec_len = pxt4_rec_len_to_disk(new_size, new_size);
	}
}

static int pxt4_update_inline_dir(handle_t *handle, struct inode *dir,
				  struct pxt4_iloc *iloc)
{
	int ret;
	int old_size = PXT4_I(dir)->i_inline_size - PXT4_MIN_INLINE_DATA_SIZE;
	int new_size = get_max_inline_xattr_value_size(dir, iloc);

	if (new_size - old_size <= PXT4_DIR_REC_LEN(1))
		return -ENOSPC;

	ret = pxt4_update_inline_data(handle, dir,
				      new_size + PXT4_MIN_INLINE_DATA_SIZE);
	if (ret)
		return ret;

	pxt4_update_final_de(pxt4_get_inline_xattr_pos(dir, iloc), old_size,
			     PXT4_I(dir)->i_inline_size -
						PXT4_MIN_INLINE_DATA_SIZE);
	dir->i_size = PXT4_I(dir)->i_disksize = PXT4_I(dir)->i_inline_size;
	return 0;
}

static void pxt4_restore_inline_data(handle_t *handle, struct inode *inode,
				     struct pxt4_iloc *iloc,
				     void *buf, int inline_size)
{
	int ret;

	ret = pxt4_create_inline_data(handle, inode, inline_size);
	if (ret) {
		pxt4_msg(inode->i_sb, KERN_EMERG,
			"error restoring inline_data for inode -- potential data loss! (inode %lu, error %d)",
			inode->i_ino, ret);
		return;
	}
	pxt4_write_inline_data(inode, iloc, buf, 0, inline_size);
	pxt4_set_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA);
}

static int pxt4_finish_convert_inline_dir(handle_t *handle,
					  struct inode *inode,
					  struct buffer_head *dir_block,
					  void *buf,
					  int inline_size)
{
	int err, csum_size = 0, header_size = 0;
	struct pxt4_dir_entry_2 *de;
	void *target = dir_block->b_data;

	/*
	 * First create "." and ".." and then copy the dir information
	 * back to the block.
	 */
	de = (struct pxt4_dir_entry_2 *)target;
	de = pxt4_init_dot_dotdot(inode, de,
		inode->i_sb->s_blocksize, csum_size,
		le32_to_cpu(((struct pxt4_dir_entry_2 *)buf)->inode), 1);
	header_size = (void *)de - target;

	memcpy((void *)de, buf + PXT4_INLINE_DOTDOT_SIZE,
		inline_size - PXT4_INLINE_DOTDOT_SIZE);

	if (pxt4_has_metadata_csum(inode->i_sb))
		csum_size = sizeof(struct pxt4_dir_entry_tail);

	inode->i_size = inode->i_sb->s_blocksize;
	i_size_write(inode, inode->i_sb->s_blocksize);
	PXT4_I(inode)->i_disksize = inode->i_sb->s_blocksize;
	pxt4_update_final_de(dir_block->b_data,
			inline_size - PXT4_INLINE_DOTDOT_SIZE + header_size,
			inode->i_sb->s_blocksize - csum_size);

	if (csum_size)
		pxt4_initialize_dirent_tail(dir_block,
					    inode->i_sb->s_blocksize);
	set_buffer_uptodate(dir_block);
	unlock_buffer(dir_block);
	err = pxt4_handle_dirty_dirblock(handle, inode, dir_block);
	if (err)
		return err;
	set_buffer_verified(dir_block);
	return pxt4_mark_inode_dirty(handle, inode);
}

static int pxt4_convert_inline_data_nolock(handle_t *handle,
					   struct inode *inode,
					   struct pxt4_iloc *iloc)
{
	int error;
	void *buf = NULL;
	struct buffer_head *data_bh = NULL;
	struct pxt4_map_blocks map;
	int inline_size;

	inline_size = pxt4_get_inline_size(inode);
	buf = kmalloc(inline_size, GFP_NOFS);
	if (!buf) {
		error = -ENOMEM;
		goto out;
	}

	error = pxt4_read_inline_data(inode, buf, inline_size, iloc);
	if (error < 0)
		goto out;

	/*
	 * Make sure the inline directory entries pass checks before we try to
	 * convert them, so that we avoid touching stuff that needs fsck.
	 */
	if (S_ISDIR(inode->i_mode)) {
		error = pxt4_check_all_de(inode, iloc->bh,
					buf + PXT4_INLINE_DOTDOT_SIZE,
					inline_size - PXT4_INLINE_DOTDOT_SIZE);
		if (error)
			goto out;
	}

	error = pxt4_destroy_inline_data_nolock(handle, inode);
	if (error)
		goto out;

	map.m_lblk = 0;
	map.m_len = 1;
	map.m_flags = 0;
	error = pxt4_map_blocks(handle, inode, &map, PXT4_GET_BLOCKS_CREATE);
	if (error < 0)
		goto out_restore;
	if (!(map.m_flags & PXT4_MAP_MAPPED)) {
		error = -EIO;
		goto out_restore;
	}

	data_bh = sb_getblk(inode->i_sb, map.m_pblk);
	if (!data_bh) {
		error = -ENOMEM;
		goto out_restore;
	}

	lock_buffer(data_bh);
	error = pxt4_journal_get_create_access(handle, data_bh);
	if (error) {
		unlock_buffer(data_bh);
		error = -EIO;
		goto out_restore;
	}
	memset(data_bh->b_data, 0, inode->i_sb->s_blocksize);

	if (!S_ISDIR(inode->i_mode)) {
		memcpy(data_bh->b_data, buf, inline_size);
		set_buffer_uptodate(data_bh);
		unlock_buffer(data_bh);
		error = pxt4_handle_dirty_metadata(handle,
						   inode, data_bh);
	} else {
		error = pxt4_finish_convert_inline_dir(handle, inode, data_bh,
						       buf, inline_size);
	}

out_restore:
	if (error)
		pxt4_restore_inline_data(handle, inode, iloc, buf, inline_size);

out:
	brelse(data_bh);
	kfree(buf);
	return error;
}

/*
 * Try to add the new entry to the inline data.
 * If succeeds, return 0. If not, extended the inline dir and copied data to
 * the new created block.
 */
int pxt4_try_add_inline_entry(handle_t *handle, struct pxt4_filename *fname,
			      struct inode *dir, struct inode *inode)
{
	int ret, inline_size, no_expand;
	void *inline_start;
	struct pxt4_iloc iloc;

	ret = pxt4_get_inode_loc(dir, &iloc);
	if (ret)
		return ret;

	pxt4_write_lock_xattr(dir, &no_expand);
	if (!pxt4_has_inline_data(dir))
		goto out;

	inline_start = (void *)pxt4_raw_inode(&iloc)->i_block +
						 PXT4_INLINE_DOTDOT_SIZE;
	inline_size = PXT4_MIN_INLINE_DATA_SIZE - PXT4_INLINE_DOTDOT_SIZE;

	ret = pxt4_add_dirent_to_inline(handle, fname, dir, inode, &iloc,
					inline_start, inline_size);
	if (ret != -ENOSPC)
		goto out;

	/* check whether it can be inserted to inline xattr space. */
	inline_size = PXT4_I(dir)->i_inline_size -
			PXT4_MIN_INLINE_DATA_SIZE;
	if (!inline_size) {
		/* Try to use the xattr space.*/
		ret = pxt4_update_inline_dir(handle, dir, &iloc);
		if (ret && ret != -ENOSPC)
			goto out;

		inline_size = PXT4_I(dir)->i_inline_size -
				PXT4_MIN_INLINE_DATA_SIZE;
	}

	if (inline_size) {
		inline_start = pxt4_get_inline_xattr_pos(dir, &iloc);

		ret = pxt4_add_dirent_to_inline(handle, fname, dir,
						inode, &iloc, inline_start,
						inline_size);

		if (ret != -ENOSPC)
			goto out;
	}

	/*
	 * The inline space is filled up, so create a new block for it.
	 * As the extent tree will be created, we have to save the inline
	 * dir first.
	 */
	ret = pxt4_convert_inline_data_nolock(handle, dir, &iloc);

out:
	pxt4_write_unlock_xattr(dir, &no_expand);
	pxt4_mark_inode_dirty(handle, dir);
	brelse(iloc.bh);
	return ret;
}

/*
 * This function fills a red-black tree with information from an
 * inlined dir.  It returns the number directory entries loaded
 * into the tree.  If there is an error it is returned in err.
 */
int pxt4_inlinedir_to_tree(struct file *dir_file,
			   struct inode *dir, pxt4_lblk_t block,
			   struct dx_hash_info *hinfo,
			   __u32 start_hash, __u32 start_minor_hash,
			   int *has_inline_data)
{
	int err = 0, count = 0;
	unsigned int parent_ino;
	int pos;
	struct pxt4_dir_entry_2 *de;
	struct inode *inode = file_inode(dir_file);
	int ret, inline_size = 0;
	struct pxt4_iloc iloc;
	void *dir_buf = NULL;
	struct pxt4_dir_entry_2 fake;
	struct fscrypt_str tmp_str;

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode)) {
		up_read(&PXT4_I(inode)->xattr_sem);
		*has_inline_data = 0;
		goto out;
	}

	inline_size = pxt4_get_inline_size(inode);
	dir_buf = kmalloc(inline_size, GFP_NOFS);
	if (!dir_buf) {
		ret = -ENOMEM;
		up_read(&PXT4_I(inode)->xattr_sem);
		goto out;
	}

	ret = pxt4_read_inline_data(inode, dir_buf, inline_size, &iloc);
	up_read(&PXT4_I(inode)->xattr_sem);
	if (ret < 0)
		goto out;

	pos = 0;
	parent_ino = le32_to_cpu(((struct pxt4_dir_entry_2 *)dir_buf)->inode);
	while (pos < inline_size) {
		/*
		 * As inlined dir doesn't store any information about '.' and
		 * only the inode number of '..' is stored, we have to handle
		 * them differently.
		 */
		if (pos == 0) {
			fake.inode = cpu_to_le32(inode->i_ino);
			fake.name_len = 1;
			strcpy(fake.name, ".");
			fake.rec_len = pxt4_rec_len_to_disk(
						PXT4_DIR_REC_LEN(fake.name_len),
						inline_size);
			pxt4_set_de_type(inode->i_sb, &fake, S_IFDIR);
			de = &fake;
			pos = PXT4_INLINE_DOTDOT_OFFSET;
		} else if (pos == PXT4_INLINE_DOTDOT_OFFSET) {
			fake.inode = cpu_to_le32(parent_ino);
			fake.name_len = 2;
			strcpy(fake.name, "..");
			fake.rec_len = pxt4_rec_len_to_disk(
						PXT4_DIR_REC_LEN(fake.name_len),
						inline_size);
			pxt4_set_de_type(inode->i_sb, &fake, S_IFDIR);
			de = &fake;
			pos = PXT4_INLINE_DOTDOT_SIZE;
		} else {
			de = (struct pxt4_dir_entry_2 *)(dir_buf + pos);
			pos += pxt4_rec_len_from_disk(de->rec_len, inline_size);
			if (pxt4_check_dir_entry(inode, dir_file, de,
					 iloc.bh, dir_buf,
					 inline_size, pos)) {
				ret = count;
				goto out;
			}
		}

		pxt4fs_dirhash(dir, de->name, de->name_len, hinfo);
		if ((hinfo->hash < start_hash) ||
		    ((hinfo->hash == start_hash) &&
		     (hinfo->minor_hash < start_minor_hash)))
			continue;
		if (de->inode == 0)
			continue;
		tmp_str.name = de->name;
		tmp_str.len = de->name_len;
		err = pxt4_htree_store_dirent(dir_file, hinfo->hash,
					      hinfo->minor_hash, de, &tmp_str);
		if (err) {
			ret = err;
			goto out;
		}
		count++;
	}
	ret = count;
out:
	kfree(dir_buf);
	brelse(iloc.bh);
	return ret;
}

/*
 * So this function is called when the volume is mkfsed with
 * dir_index disabled. In order to keep f_pos persistent
 * after we convert from an inlined dir to a blocked based,
 * we just pretend that we are a normal dir and return the
 * offset as if '.' and '..' really take place.
 *
 */
int pxt4_read_inline_dir(struct file *file,
			 struct dir_context *ctx,
			 int *has_inline_data)
{
	unsigned int offset, parent_ino;
	int i;
	struct pxt4_dir_entry_2 *de;
	struct super_block *sb;
	struct inode *inode = file_inode(file);
	int ret, inline_size = 0;
	struct pxt4_iloc iloc;
	void *dir_buf = NULL;
	int dotdot_offset, dotdot_size, extra_offset, extra_size;

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode)) {
		up_read(&PXT4_I(inode)->xattr_sem);
		*has_inline_data = 0;
		goto out;
	}

	inline_size = pxt4_get_inline_size(inode);
	dir_buf = kmalloc(inline_size, GFP_NOFS);
	if (!dir_buf) {
		ret = -ENOMEM;
		up_read(&PXT4_I(inode)->xattr_sem);
		goto out;
	}

	ret = pxt4_read_inline_data(inode, dir_buf, inline_size, &iloc);
	up_read(&PXT4_I(inode)->xattr_sem);
	if (ret < 0)
		goto out;

	ret = 0;
	sb = inode->i_sb;
	parent_ino = le32_to_cpu(((struct pxt4_dir_entry_2 *)dir_buf)->inode);
	offset = ctx->pos;

	/*
	 * dotdot_offset and dotdot_size is the real offset and
	 * size for ".." and "." if the dir is block based while
	 * the real size for them are only PXT4_INLINE_DOTDOT_SIZE.
	 * So we will use extra_offset and extra_size to indicate them
	 * during the inline dir iteration.
	 */
	dotdot_offset = PXT4_DIR_REC_LEN(1);
	dotdot_size = dotdot_offset + PXT4_DIR_REC_LEN(2);
	extra_offset = dotdot_size - PXT4_INLINE_DOTDOT_SIZE;
	extra_size = extra_offset + inline_size;

	/*
	 * If the version has changed since the last call to
	 * readdir(2), then we might be pointing to an invalid
	 * dirent right now.  Scan from the start of the inline
	 * dir to make sure.
	 */
	if (!inode_eq_iversion(inode, file->f_version)) {
		for (i = 0; i < extra_size && i < offset;) {
			/*
			 * "." is with offset 0 and
			 * ".." is dotdot_offset.
			 */
			if (!i) {
				i = dotdot_offset;
				continue;
			} else if (i == dotdot_offset) {
				i = dotdot_size;
				continue;
			}
			/* for other entry, the real offset in
			 * the buf has to be tuned accordingly.
			 */
			de = (struct pxt4_dir_entry_2 *)
				(dir_buf + i - extra_offset);
			/* It's too expensive to do a full
			 * dirent test each time round this
			 * loop, but we do have to test at
			 * least that it is non-zero.  A
			 * failure will be detected in the
			 * dirent test below. */
			if (pxt4_rec_len_from_disk(de->rec_len, extra_size)
				< PXT4_DIR_REC_LEN(1))
				break;
			i += pxt4_rec_len_from_disk(de->rec_len,
						    extra_size);
		}
		offset = i;
		ctx->pos = offset;
		file->f_version = inode_query_iversion(inode);
	}

	while (ctx->pos < extra_size) {
		if (ctx->pos == 0) {
			if (!dir_emit(ctx, ".", 1, inode->i_ino, DT_DIR))
				goto out;
			ctx->pos = dotdot_offset;
			continue;
		}

		if (ctx->pos == dotdot_offset) {
			if (!dir_emit(ctx, "..", 2, parent_ino, DT_DIR))
				goto out;
			ctx->pos = dotdot_size;
			continue;
		}

		de = (struct pxt4_dir_entry_2 *)
			(dir_buf + ctx->pos - extra_offset);
		if (pxt4_check_dir_entry(inode, file, de, iloc.bh, dir_buf,
					 extra_size, ctx->pos))
			goto out;
		if (le32_to_cpu(de->inode)) {
			if (!dir_emit(ctx, de->name, de->name_len,
				      le32_to_cpu(de->inode),
				      get_dtype(sb, de->file_type)))
				goto out;
		}
		ctx->pos += pxt4_rec_len_from_disk(de->rec_len, extra_size);
	}
out:
	kfree(dir_buf);
	brelse(iloc.bh);
	return ret;
}

struct buffer_head *pxt4_get_first_inline_block(struct inode *inode,
					struct pxt4_dir_entry_2 **parent_de,
					int *retval)
{
	struct pxt4_iloc iloc;

	*retval = pxt4_get_inode_loc(inode, &iloc);
	if (*retval)
		return NULL;

	*parent_de = (struct pxt4_dir_entry_2 *)pxt4_raw_inode(&iloc)->i_block;

	return iloc.bh;
}

/*
 * Try to create the inline data for the new dir.
 * If it succeeds, return 0, otherwise return the error.
 * In case of ENOSPC, the caller should create the normal disk layout dir.
 */
int pxt4_try_create_inline_dir(handle_t *handle, struct inode *parent,
			       struct inode *inode)
{
	int ret, inline_size = PXT4_MIN_INLINE_DATA_SIZE;
	struct pxt4_iloc iloc;
	struct pxt4_dir_entry_2 *de;

	ret = pxt4_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	ret = pxt4_prepare_inline_data(handle, inode, inline_size);
	if (ret)
		goto out;

	/*
	 * For inline dir, we only save the inode information for the ".."
	 * and create a fake dentry to cover the left space.
	 */
	de = (struct pxt4_dir_entry_2 *)pxt4_raw_inode(&iloc)->i_block;
	de->inode = cpu_to_le32(parent->i_ino);
	de = (struct pxt4_dir_entry_2 *)((void *)de + PXT4_INLINE_DOTDOT_SIZE);
	de->inode = 0;
	de->rec_len = pxt4_rec_len_to_disk(
				inline_size - PXT4_INLINE_DOTDOT_SIZE,
				inline_size);
	set_nlink(inode, 2);
	inode->i_size = PXT4_I(inode)->i_disksize = inline_size;
out:
	brelse(iloc.bh);
	return ret;
}

struct buffer_head *pxt4_find_inline_entry(struct inode *dir,
					struct pxt4_filename *fname,
					struct pxt4_dir_entry_2 **res_dir,
					int *has_inline_data)
{
	int ret;
	struct pxt4_iloc iloc;
	void *inline_start;
	int inline_size;

	if (pxt4_get_inode_loc(dir, &iloc))
		return NULL;

	down_read(&PXT4_I(dir)->xattr_sem);
	if (!pxt4_has_inline_data(dir)) {
		*has_inline_data = 0;
		goto out;
	}

	inline_start = (void *)pxt4_raw_inode(&iloc)->i_block +
						PXT4_INLINE_DOTDOT_SIZE;
	inline_size = PXT4_MIN_INLINE_DATA_SIZE - PXT4_INLINE_DOTDOT_SIZE;
	ret = pxt4_search_dir(iloc.bh, inline_start, inline_size,
			      dir, fname, 0, res_dir);
	if (ret == 1)
		goto out_find;
	if (ret < 0)
		goto out;

	if (pxt4_get_inline_size(dir) == PXT4_MIN_INLINE_DATA_SIZE)
		goto out;

	inline_start = pxt4_get_inline_xattr_pos(dir, &iloc);
	inline_size = pxt4_get_inline_size(dir) - PXT4_MIN_INLINE_DATA_SIZE;

	ret = pxt4_search_dir(iloc.bh, inline_start, inline_size,
			      dir, fname, 0, res_dir);
	if (ret == 1)
		goto out_find;

out:
	brelse(iloc.bh);
	iloc.bh = NULL;
out_find:
	up_read(&PXT4_I(dir)->xattr_sem);
	return iloc.bh;
}

int pxt4_delete_inline_entry(handle_t *handle,
			     struct inode *dir,
			     struct pxt4_dir_entry_2 *de_del,
			     struct buffer_head *bh,
			     int *has_inline_data)
{
	int err, inline_size, no_expand;
	struct pxt4_iloc iloc;
	void *inline_start;

	err = pxt4_get_inode_loc(dir, &iloc);
	if (err)
		return err;

	pxt4_write_lock_xattr(dir, &no_expand);
	if (!pxt4_has_inline_data(dir)) {
		*has_inline_data = 0;
		goto out;
	}

	if ((void *)de_del - ((void *)pxt4_raw_inode(&iloc)->i_block) <
		PXT4_MIN_INLINE_DATA_SIZE) {
		inline_start = (void *)pxt4_raw_inode(&iloc)->i_block +
					PXT4_INLINE_DOTDOT_SIZE;
		inline_size = PXT4_MIN_INLINE_DATA_SIZE -
				PXT4_INLINE_DOTDOT_SIZE;
	} else {
		inline_start = pxt4_get_inline_xattr_pos(dir, &iloc);
		inline_size = pxt4_get_inline_size(dir) -
				PXT4_MIN_INLINE_DATA_SIZE;
	}

	BUFFER_TRACE(bh, "get_write_access");
	err = pxt4_journal_get_write_access(handle, bh);
	if (err)
		goto out;

	err = pxt4_generic_delete_entry(handle, dir, de_del, bh,
					inline_start, inline_size, 0);
	if (err)
		goto out;

	pxt4_show_inline_dir(dir, iloc.bh, inline_start, inline_size);
out:
	pxt4_write_unlock_xattr(dir, &no_expand);
	if (likely(err == 0))
		err = pxt4_mark_inode_dirty(handle, dir);
	brelse(iloc.bh);
	if (err != -ENOENT)
		pxt4_std_error(dir->i_sb, err);
	return err;
}

/*
 * Get the inline dentry at offset.
 */
static inline struct pxt4_dir_entry_2 *
pxt4_get_inline_entry(struct inode *inode,
		      struct pxt4_iloc *iloc,
		      unsigned int offset,
		      void **inline_start,
		      int *inline_size)
{
	void *inline_pos;

	BUG_ON(offset > pxt4_get_inline_size(inode));

	if (offset < PXT4_MIN_INLINE_DATA_SIZE) {
		inline_pos = (void *)pxt4_raw_inode(iloc)->i_block;
		*inline_size = PXT4_MIN_INLINE_DATA_SIZE;
	} else {
		inline_pos = pxt4_get_inline_xattr_pos(inode, iloc);
		offset -= PXT4_MIN_INLINE_DATA_SIZE;
		*inline_size = pxt4_get_inline_size(inode) -
				PXT4_MIN_INLINE_DATA_SIZE;
	}

	if (inline_start)
		*inline_start = inline_pos;
	return (struct pxt4_dir_entry_2 *)(inline_pos + offset);
}

bool empty_inline_dir(struct inode *dir, int *has_inline_data)
{
	int err, inline_size;
	struct pxt4_iloc iloc;
	size_t inline_len;
	void *inline_pos;
	unsigned int offset;
	struct pxt4_dir_entry_2 *de;
	bool ret = true;

	err = pxt4_get_inode_loc(dir, &iloc);
	if (err) {
		PXT4_ERROR_INODE(dir, "error %d getting inode %lu block",
				 err, dir->i_ino);
		return true;
	}

	down_read(&PXT4_I(dir)->xattr_sem);
	if (!pxt4_has_inline_data(dir)) {
		*has_inline_data = 0;
		goto out;
	}

	de = (struct pxt4_dir_entry_2 *)pxt4_raw_inode(&iloc)->i_block;
	if (!le32_to_cpu(de->inode)) {
		pxt4_warning(dir->i_sb,
			     "bad inline directory (dir #%lu) - no `..'",
			     dir->i_ino);
		ret = true;
		goto out;
	}

	inline_len = pxt4_get_inline_size(dir);
	offset = PXT4_INLINE_DOTDOT_SIZE;
	while (offset < inline_len) {
		de = pxt4_get_inline_entry(dir, &iloc, offset,
					   &inline_pos, &inline_size);
		if (pxt4_check_dir_entry(dir, NULL, de,
					 iloc.bh, inline_pos,
					 inline_size, offset)) {
			pxt4_warning(dir->i_sb,
				     "bad inline directory (dir #%lu) - "
				     "inode %u, rec_len %u, name_len %d"
				     "inline size %d",
				     dir->i_ino, le32_to_cpu(de->inode),
				     le16_to_cpu(de->rec_len), de->name_len,
				     inline_size);
			ret = true;
			goto out;
		}
		if (le32_to_cpu(de->inode)) {
			ret = false;
			goto out;
		}
		offset += pxt4_rec_len_from_disk(de->rec_len, inline_size);
	}

out:
	up_read(&PXT4_I(dir)->xattr_sem);
	brelse(iloc.bh);
	return ret;
}

int pxt4_destroy_inline_data(handle_t *handle, struct inode *inode)
{
	int ret, no_expand;

	pxt4_write_lock_xattr(inode, &no_expand);
	ret = pxt4_destroy_inline_data_nolock(handle, inode);
	pxt4_write_unlock_xattr(inode, &no_expand);

	return ret;
}

int pxt4_inline_data_iomap(struct inode *inode, struct iomap *iomap)
{
	__u64 addr;
	int error = -EAGAIN;
	struct pxt4_iloc iloc;

	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode))
		goto out;

	error = pxt4_get_inode_loc(inode, &iloc);
	if (error)
		goto out;

	addr = (__u64)iloc.bh->b_blocknr << inode->i_sb->s_blocksize_bits;
	addr += (char *)pxt4_raw_inode(&iloc) - iloc.bh->b_data;
	addr += offsetof(struct pxt4_inode, i_block);

	brelse(iloc.bh);

	iomap->addr = addr;
	iomap->offset = 0;
	iomap->length = min_t(loff_t, pxt4_get_inline_size(inode),
			      i_size_read(inode));
	iomap->type = IOMAP_INLINE;
	iomap->flags = 0;

out:
	up_read(&PXT4_I(inode)->xattr_sem);
	return error;
}

int pxt4_inline_data_fiemap(struct inode *inode,
			    struct fiemap_extent_info *fieinfo,
			    int *has_inline, __u64 start, __u64 len)
{
	__u64 physical = 0;
	__u64 inline_len;
	__u32 flags = FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_NOT_ALIGNED |
		FIEMAP_EXTENT_LAST;
	int error = 0;
	struct pxt4_iloc iloc;

	down_read(&PXT4_I(inode)->xattr_sem);
	if (!pxt4_has_inline_data(inode)) {
		*has_inline = 0;
		goto out;
	}
	inline_len = min_t(size_t, pxt4_get_inline_size(inode),
			   i_size_read(inode));
	if (start >= inline_len)
		goto out;
	if (start + len < inline_len)
		inline_len = start + len;
	inline_len -= start;

	error = pxt4_get_inode_loc(inode, &iloc);
	if (error)
		goto out;

	physical = (__u64)iloc.bh->b_blocknr << inode->i_sb->s_blocksize_bits;
	physical += (char *)pxt4_raw_inode(&iloc) - iloc.bh->b_data;
	physical += offsetof(struct pxt4_inode, i_block);

	brelse(iloc.bh);
out:
	up_read(&PXT4_I(inode)->xattr_sem);
	if (physical)
		error = fiemap_fill_next_extent(fieinfo, start, physical,
						inline_len, flags);
	return (error < 0 ? error : 0);
}

int pxt4_inline_data_truncate(struct inode *inode, int *has_inline)
{
	handle_t *handle;
	int inline_size, value_len, needed_blocks, no_expand, err = 0;
	size_t i_size;
	void *value = NULL;
	struct pxt4_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct pxt4_xattr_info i = {
		.name_index = PXT4_XATTR_INDEX_SYSTEM,
		.name = PXT4_XATTR_SYSTEM_DATA,
	};


	needed_blocks = pxt4_writepage_trans_blocks(inode);
	handle = pxt4_journal_start(inode, PXT4_HT_INODE, needed_blocks);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	pxt4_write_lock_xattr(inode, &no_expand);
	if (!pxt4_has_inline_data(inode)) {
		pxt4_write_unlock_xattr(inode, &no_expand);
		*has_inline = 0;
		pxt4_journal_stop(handle);
		return 0;
	}

	if ((err = pxt4_orphan_add(handle, inode)) != 0)
		goto out;

	if ((err = pxt4_get_inode_loc(inode, &is.iloc)) != 0)
		goto out;

	down_write(&PXT4_I(inode)->i_data_sem);
	i_size = inode->i_size;
	inline_size = pxt4_get_inline_size(inode);
	PXT4_I(inode)->i_disksize = i_size;

	if (i_size < inline_size) {
		/* Clear the content in the xattr space. */
		if (inline_size > PXT4_MIN_INLINE_DATA_SIZE) {
			if ((err = pxt4_xattr_ibody_find(inode, &i, &is)) != 0)
				goto out_error;

			BUG_ON(is.s.not_found);

			value_len = le32_to_cpu(is.s.here->e_value_size);
			value = kmalloc(value_len, GFP_NOFS);
			if (!value) {
				err = -ENOMEM;
				goto out_error;
			}

			err = pxt4_xattr_ibody_get(inode, i.name_index,
						   i.name, value, value_len);
			if (err <= 0)
				goto out_error;

			i.value = value;
			i.value_len = i_size > PXT4_MIN_INLINE_DATA_SIZE ?
					i_size - PXT4_MIN_INLINE_DATA_SIZE : 0;
			err = pxt4_xattr_ibody_set(handle, inode, &i, &is);
			if (err)
				goto out_error;
		}

		/* Clear the content within i_blocks. */
		if (i_size < PXT4_MIN_INLINE_DATA_SIZE) {
			void *p = (void *) pxt4_raw_inode(&is.iloc)->i_block;
			memset(p + i_size, 0,
			       PXT4_MIN_INLINE_DATA_SIZE - i_size);
		}

		PXT4_I(inode)->i_inline_size = i_size <
					PXT4_MIN_INLINE_DATA_SIZE ?
					PXT4_MIN_INLINE_DATA_SIZE : i_size;
	}

out_error:
	up_write(&PXT4_I(inode)->i_data_sem);
out:
	brelse(is.iloc.bh);
	pxt4_write_unlock_xattr(inode, &no_expand);
	kfree(value);
	if (inode->i_nlink)
		pxt4_orphan_del(handle, inode);

	if (err == 0) {
		inode->i_mtime = inode->i_ctime = current_time(inode);
		err = pxt4_mark_inode_dirty(handle, inode);
		if (IS_SYNC(inode))
			pxt4_handle_sync(handle);
	}
	pxt4_journal_stop(handle);
	return err;
}

int pxt4_convert_inline_data(struct inode *inode)
{
	int error, needed_blocks, no_expand;
	handle_t *handle;
	struct pxt4_iloc iloc;

	if (!pxt4_has_inline_data(inode)) {
		pxt4_clear_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA);
		return 0;
	} else if (!pxt4_test_inode_state(inode, PXT4_STATE_MAY_INLINE_DATA)) {
		/*
		 * Inode has inline data but PXT4_STATE_MAY_INLINE_DATA is
		 * cleared. This means we are in the middle of moving of
		 * inline data to delay allocated block. Just force writeout
		 * here to finish conversion.
		 */
		error = filemap_flush(inode->i_mapping);
		if (error)
			return error;
		if (!pxt4_has_inline_data(inode))
			return 0;
	}

	needed_blocks = pxt4_writepage_trans_blocks(inode);

	iloc.bh = NULL;
	error = pxt4_get_inode_loc(inode, &iloc);
	if (error)
		return error;

	handle = pxt4_journal_start(inode, PXT4_HT_WRITE_PAGE, needed_blocks);
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
		goto out_free;
	}

	pxt4_write_lock_xattr(inode, &no_expand);
	if (pxt4_has_inline_data(inode))
		error = pxt4_convert_inline_data_nolock(handle, inode, &iloc);
	pxt4_write_unlock_xattr(inode, &no_expand);
	pxt4_journal_stop(handle);
out_free:
	brelse(iloc.bh);
	return error;
}

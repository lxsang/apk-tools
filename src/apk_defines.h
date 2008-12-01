/* apk_defines.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation. See http://www.gnu.org/ for details.
 */

#ifndef APK_DEFINES_H
#define APK_DEFINES_H

#include <malloc.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define BIT(x) (1 << (x))

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef NULL
#define NULL 0L
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})
#endif

#if 1
#include "md5.h"

typedef unsigned char *csum_p;
typedef md5sum_t csum_t;
typedef struct md5_ctx csum_ctx_t;
extern csum_t bad_checksum;

#define csum_init(ctx)			md5_init(ctx)
#define csum_process(ctx, buf, len)	md5_process(ctx, buf, len)
#define csum_finish(ctx, buf)		md5_finish(ctx, buf)
#define csum_valid(buf)			memcmp(buf, bad_checksum, sizeof(csum_t))
#endif

extern int apk_cwd_fd, apk_quiet;

#define apk_error(args...)	apk_log("ERROR: ", args);
#define apk_warning(args...)	if (!apk_quiet) { apk_log("WARNING: ", args); }
#define apk_message(args...)	if (!apk_quiet) { apk_log(NULL, args); }

void apk_log(const char *prefix, const char *format, ...);

#define APK_ARRAY(array_type_name, elem_type_name)			\
	struct array_type_name {					\
		int num;						\
		elem_type_name item[];					\
	};								\
	static inline struct array_type_name *				\
	array_type_name##_resize(struct array_type_name *a, int size)	\
	{								\
		struct array_type_name *tmp;				\
		tmp = (struct array_type_name *)			\
			realloc(a, sizeof(struct array_type_name) +	\
				   size * sizeof(elem_type_name));	\
		tmp->num = size;					\
		return tmp;						\
	}								\
	static inline elem_type_name *					\
	array_type_name##_add(struct array_type_name **a)		\
	{								\
		int size = 1;						\
		if (*a != NULL) size += (*a)->num;			\
		*a = array_type_name##_resize(*a, size);		\
		return &(*a)->item[size-1];				\
	}

APK_ARRAY(apk_string_array, char *);

#define LIST_END (void *) 0xe01
#define LIST_POISON1 (void *) 0xdeadbeef
#define LIST_POISON2 (void *) 0xabbaabba

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next;
};

static inline int hlist_empty(const struct hlist_head *h)
{
	return !h->first;
}

static inline int hlist_hashed(const struct hlist_node *n)
{
	return n->next != NULL;
}

static inline void __hlist_del(struct hlist_node *n, struct hlist_node **pprev)
{
	*pprev = n->next;
	n->next = NULL;
}

static inline void hlist_del(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node **pp = &h->first;

	while (*pp != NULL && *pp != LIST_END && *pp != n)
		pp = &(*pp)->next;

	if (*pp == n)
		__hlist_del(n, pp);
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;

	n->next = first ? first : LIST_END;
	h->first = n;
}

static inline void hlist_add_after(struct hlist_node *n, struct hlist_node **prev)
{
	n->next = *prev ? *prev : LIST_END;
	*prev = n;
}

static inline struct hlist_node **hlist_tail_ptr(struct hlist_head *h)
{
	struct hlist_node *n = h->first;
	if (n == NULL || n == LIST_END)
		return &h->first;
	while (n->next != NULL && n->next != LIST_END)
		n = n->next;
	return &n->next;
}

#define hlist_entry(ptr, type, member) container_of(ptr,type,member)

#define hlist_for_each(pos, head) \
	for (pos = (head)->first; pos && pos != LIST_END; \
	     pos = pos->next)

#define hlist_for_each_safe(pos, n, head) \
	for (pos = (head)->first; pos && pos != LIST_END && \
		({ n = pos->next; 1; }); \
	     pos = n)

#define hlist_for_each_entry(tpos, pos, head, member)			 \
	for (pos = (head)->first;					 \
	     pos && pos != LIST_END  &&					 \
		({ tpos = hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = pos->next)

#define hlist_for_each_entry_safe(tpos, pos, n, head, member) 		 \
	for (pos = (head)->first;					 \
	     pos && pos != LIST_END && ({ n = pos->next; 1; }) && 	 \
		({ tpos = hlist_entry(pos, typeof(*tpos), member); 1;}); \
	     pos = n)


struct list_head {
	struct list_head *next, *prev;
};

static inline void list_init(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head * prev, struct list_head * next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

static inline int list_hashed(const struct list_head *n)
{
	return n->next != n->prev;
}

#define list_entry(ptr, type, member) container_of(ptr,type,member)

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member)				\
	for (pos = list_entry((head)->next, typeof(*pos), member);	\
	     &pos->member != (head); 					\
	     pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_entry((head)->next, typeof(*pos), member),	\
		n = list_entry(pos->member.next, typeof(*pos), member);	\
	     &pos->member != (head); 					\
	     pos = n, n = list_entry(n->member.next, typeof(*n), member))

#endif
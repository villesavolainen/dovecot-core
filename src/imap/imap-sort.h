#ifndef __IMAP_SORT_H
#define __IMAP_SORT_H

int imap_sort(struct client *client, const char *charset,
	      struct mail_search_arg *args,
	      const enum mail_sort_type *sort_program);

#endif

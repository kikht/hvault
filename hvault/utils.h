#ifndef _UTILS_H_
#define _UTILS_H_

#include "common.h"

#undef HASH_FUNCTION
#include "uthash.h"

/* undefine the uthash defaults */
#undef uthash_malloc
#undef uthash_free
#undef uthash_fatal

/* re-define, specifying alternate functions */
#define uthash_malloc(sz) palloc(sz)
#define uthash_free(ptr,sz) pfree(ptr)
#define uthash_fatal(msg) elog(FATAL, msg)

/* Appends value to List if it is not already there and returns inserted 
 * element's position or position of equal element */
int list_append_unique_pos (List ** list, void * item);

/* Checks whether any of the Relids in 'relids_list' is equal to 'relids' */
bool bms_equal_any(Relids relids, List *relids_list);

typedef struct {
    char * key;
    void * value;
    UT_hash_handle hh;
} HvaultHash;

#endif /* _UTILS_H_ */
#ifndef __DID_H_
#define __DID_H_

int free_entire_did_cache(struct afp_volume * volume) ;
int remove_did_entry(struct afp_volume * volume, const char * name) ;
unsigned char is_dir(struct afp_volume * volume,
        unsigned int parentdid, const char * path);
int get_dirid(struct afp_volume * volume, const char * path,
        char * basename, unsigned int * dirid);

/* getattr cache */
int getattr_cache_init(struct afp_volume * volume);
int getattr_cache_clear(struct afp_volume * volume);
int getattr_cache_lookup(struct afp_volume * volume,
        unsigned int parent_did, const char * basename,
        struct afp_file_info * out);
int getattr_cache_store(struct afp_volume * volume,
        unsigned int parent_did, const char * basename,
        const struct afp_file_info * info);
int getattr_cache_invalidate(struct afp_volume * volume,
        unsigned int parent_did);

#endif

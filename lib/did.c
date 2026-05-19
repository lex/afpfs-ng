/*


    Copyright (C) 2006 Alex deVries <alexthepuffin@gmail.com>

*/
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>

#include "afpfs-ng/afp.h"
#include "afpfs-ng/afp_protocol.h"

#undef DID_CACHE_DISABLE

static unsigned short timeout=10;

struct did_cache_entry {
                                 /* For the example /foo/bar/baz */
	char dirname[AFP_MAX_PATH];  /* full name, eg. /foo/bar/     */
	unsigned int did;            /*            eg  2323          */
	struct timeval time;
	struct did_cache_entry * next;
} ;

int free_entire_did_cache(struct afp_volume * volume) 
{

	struct did_cache_entry * d, * p, *p2;

	pthread_mutex_lock(&volume->did_cache_mutex);

	p=volume->did_cache_base;

	for (d=volume->did_cache_base;d;d=p)
	{
		p2=p;
		p=d->next;
		free(p2);
	}
	pthread_mutex_unlock(&volume->did_cache_mutex);

	return 0;
}

int remove_did_entry(struct afp_volume * volume, const char * name) 
{
	struct did_cache_entry * d, * p=NULL;

	pthread_mutex_lock(&volume->did_cache_mutex);

	for (d=volume->did_cache_base;d;d=d->next)
	{
		if (strcmp(d->dirname,name)==0) {
			if (p) 
				p->next=d->next;
			else 
				volume->did_cache_base=d->next;
			volume->did_cache_stats.force_removed++;
			free(d);
			break;
		} else 
			p=d;
	}
	pthread_mutex_unlock(&volume->did_cache_mutex);
	return 0;
}

	
static int add_did_cache_entry(struct afp_volume * volume, 
	unsigned int new_did, char * path)
{

	struct did_cache_entry * new, *old_base;

	#ifdef DID_CACHE_DISABLE
	return 0;
	#endif

	if ((new=malloc(sizeof(* new)))==NULL) return -1;


	memset(new,0,sizeof(*new));

	new->did=new_did;
	memcpy(new->dirname,path,AFP_MAX_PATH);
	gettimeofday(&new->time,NULL);

	pthread_mutex_lock(&volume->did_cache_mutex);
	old_base=volume->did_cache_base;
	volume->did_cache_base=new;
	new->next=old_base;
	pthread_mutex_unlock(&volume->did_cache_mutex);

	return 0;

}

unsigned char is_dir(struct afp_volume * volume, 
	unsigned int parentdid, const char * path)
{
	int ret;
	unsigned int filebitmap=0;
	unsigned int dirbitmap=0;
	struct afp_file_info fi;
#if 0
	struct did_cache_entry * p;

	if ((p=find_did_cache_entry(volume,parentdid,path,strlen(path)))) 
		return p->isdir;
#endif
	ret =afp_getfiledirparms(volume,parentdid,
		filebitmap,dirbitmap,path,&fi);

	if (ret) return 0;


	return fi.isdir;
}

static unsigned int find_dirid_by_fullname(struct afp_volume * volume,
	char * path)
{
	struct did_cache_entry * p, *prev=volume->did_cache_base;
	struct timeval time;
	unsigned int found_did=0;
	unsigned char breakearly=0;

	#ifdef DID_CACHE_DISABLE
	goto out;
	#endif

	gettimeofday(&time,NULL);

	pthread_mutex_lock(&volume->did_cache_mutex);
	for (p=volume->did_cache_base;p;p=p->next) {
		if (time.tv_sec > (p->time.tv_sec+timeout)) {
			volume->did_cache_stats.expired++;
			if (prev==volume->did_cache_base) {
				if (strcmp(p->dirname,path)==0) breakearly=1;
				volume->did_cache_base=p->next;
				free(p);
				if (breakearly) goto out;
				p=volume->did_cache_base;
				if (!p) goto out;
				prev=volume->did_cache_base;
				continue;
			} else {
				prev->next=p->next;
				free(p);
				p=prev;
			}
		}
		if (strcmp(p->dirname,path)==0) {
			found_did=p->did;
			volume->did_cache_stats.hits++;
			goto out;
		}
		prev=p;
	}
out:
	pthread_mutex_unlock(&volume->did_cache_mutex);
	return found_did;
}


/* This calculates the dirid and basename.  It *always* gets the parent did. */

int get_dirid(struct afp_volume * volume, const char * path, 
	char * basename, unsigned int * dirid)
{
	char * p, *p2;
	int ret;
	struct afp_file_info fi;
	unsigned int filebitmap,dirbitmap;
	unsigned int newdid;
	unsigned int parent_did;
	char copy[AFP_MAX_PATH];

	if (((p=strrchr(path,'/')))==NULL) return -1; 

	/* Calculate the basename, leave copy with just the parent */
	if (basename) {
		memset(basename,0,AFP_MAX_PATH);
		memcpy(basename,p+1,strlen(path)-(p-path)-1);
	}

	/* p now points to the last '/' */

	if (p-path==0) {
		*dirid=AFP_ROOT_DID;
		goto out;
	}

	memcpy(copy,path,p-path+1);

	if (copy[p-path]=='/') copy[p-path]='\0'; /* Lop off the last '/' */

	/* See if the parent's fullname is in the cache */

	if ((newdid=find_dirid_by_fullname(volume,copy))) {
		*dirid=newdid;
		goto out;
	}
	
	/* No?  Work your way back to the start from the end looking
	   for a parent */

	while ((p=strrchr(copy,'/'))) {
		if (p==copy) {
			/* Okay, we're done since we're at the start*/
			parent_did=AFP_ROOT_DID;
			break;
		}
		*p='\0';
		if ((parent_did=find_dirid_by_fullname(volume,copy))) {
			break;
		}
	}

	/* Okay, now we have the topmost cached parent */
	/* Move forward now from the last parentid */
	filebitmap=kFPNodeIDBit ;
	dirbitmap=kFPNodeIDBit ;


	/* Go to the end of last known entry */
	p=(char *)path+(p-copy);
	p2=p;

	while ((p=strchr(p+1,'/'))) {

		memset(copy,0,AFP_MAX_PATH);
		memcpy(copy,p2,p-p2);

		volume->did_cache_stats.misses++;

		ret =afp_getfiledirparms(volume,parent_did,
			filebitmap,dirbitmap,copy,&fi);

		if (fi.isdir) {
			/* Add it to the cache */
			memset(copy,0,AFP_MAX_PATH);
			memcpy(copy,path,p-path);
			add_did_cache_entry(volume, fi.fileid,copy);

		} else {
			break;
		}
		parent_did=fi.fileid;
		p2=p;
	}
	*dirid=parent_did;

out:
	return 0;
}

/* =====================================================================
 * getattr cache: caches file metadata (size, dates, permissions, etc.)
 * to eliminate redundant GetFileDirParms network round-trips.
 * ===================================================================== */

#define GETATTR_CACHE_TIMEOUT 30

struct getattr_cache_entry {
	unsigned int parent_did;
	char basename[AFP_MAX_PATH];
	struct afp_file_info info;
	struct timeval time;
	struct getattr_cache_entry * next;
};

static struct getattr_cache_entry * getattr_cache_base(struct afp_volume * volume)
{
	/* Stored in volume->priv as a simple pointer */
	return (struct getattr_cache_entry *)volume->priv;
}

static void setattr_cache_base(struct afp_volume * volume,
	struct getattr_cache_entry * base)
{
	volume->priv = (void *)base;
}

int getattr_cache_init(struct afp_volume * volume)
{
	setattr_cache_base(volume, NULL);
	return 0;
}

int getattr_cache_clear(struct afp_volume * volume)
{
	struct getattr_cache_entry * e, *next;
	struct getattr_cache_entry * base = getattr_cache_base(volume);
	for (e = base; e; e = next) {
		next = e->next;
		free(e);
	}
	setattr_cache_base(volume, NULL);
	return 0;
}

int getattr_cache_lookup(struct afp_volume * volume,
	unsigned int parent_did, const char * basename,
	struct afp_file_info * out)
{
	struct getattr_cache_entry * e;
	struct timeval now;
	gettimeofday(&now, NULL);

	for (e = getattr_cache_base(volume); e; e = e->next) {
		if (e->parent_did == parent_did &&
		    strcmp(e->basename, basename) == 0) {
			if (now.tv_sec < (e->time.tv_sec + GETATTR_CACHE_TIMEOUT)) {
				/* Copy relevant fields, skip pointer fields */
				memcpy(out, &e->info, sizeof(struct afp_file_info));
				out->next = NULL;
				out->largelist_next = NULL;
				out->icon = NULL;
				return 0; /* hit */
			}
			/* Expired — remove entry */
			break;
		}
	}
	return -1; /* miss or expired */
}

int getattr_cache_store(struct afp_volume * volume,
	unsigned int parent_did, const char * basename,
	const struct afp_file_info * info)
{
	struct getattr_cache_entry * e;
	if ((e = malloc(sizeof(*e))) == NULL) return -1;

	e->parent_did = parent_did;
	memset(e->basename, 0, AFP_MAX_PATH);
	strncpy(e->basename, basename, AFP_MAX_PATH - 1);
	memcpy(&e->info, info, sizeof(struct afp_file_info));
	e->info.next = NULL;
	e->info.largelist_next = NULL;
	e->info.icon = NULL;
	gettimeofday(&e->time, NULL);

	pthread_mutex_lock(&volume->did_cache_mutex);
	e->next = getattr_cache_base(volume);
	setattr_cache_base(volume, e);
	pthread_mutex_unlock(&volume->did_cache_mutex);
	return 0;
}

int getattr_cache_invalidate(struct afp_volume * volume,
	unsigned int parent_did)
{
	struct getattr_cache_entry * e, *prev, *next;
	pthread_mutex_lock(&volume->did_cache_mutex);
	prev = NULL;
	for (e = getattr_cache_base(volume); e; e = next) {
		next = e->next;
		if (e->parent_did == parent_did) {
			if (prev)
				prev->next = next;
			else
				setattr_cache_base(volume, next);
			free(e);
			continue;
		}
		prev = e;
	}
	pthread_mutex_unlock(&volume->did_cache_mutex);
	return 0;
}


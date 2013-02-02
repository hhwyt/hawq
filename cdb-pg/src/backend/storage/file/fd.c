/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  Virtual file descriptor code.
 *
 * Portions Copyright (c) 2007-2009, Greenplum inc
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/file/fd.c,v 1.131 2006/11/06 17:10:22 tgl Exp $
 *
 * NOTES:
 *
 * This code manages a cache of 'virtual' file descriptors (VFDs).
 * The server opens many file descriptors for a variety of reasons,
 * including base tables, scratch files (e.g., sort and hash spool
 * files), and random calls to C library routines like system(3); it
 * is quite easy to exceed system limits on the number of open files a
 * single process can have.  (This is around 256 on many modern
 * operating systems, but can be as low as 32 on others.)
 *
 * VFDs are managed as an LRU pool, with actual OS file descriptors
 * being opened and closed as needed.  Obviously, if a routine is
 * opened using these interfaces, all subsequent operations must also
 * be through these interfaces (the File type is not a real file
 * descriptor).
 *
 * For this scheme to work, most (if not all) routines throughout the
 * server should use these interfaces instead of calling the C library
 * routines (e.g., open(2) and fopen(3)) themselves.  Otherwise, we
 * may find ourselves short of real file descriptors anyway.
 *
 * This file used to contain a bunch of stuff to support RAID levels 0
 * (jbod), 1 (duplex) and 5 (xor parity).  That stuff is all gone
 * because the parallel query processing code that called it is all
 * gone.  If you really need it you could get it from the original
 * POSTGRES source.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "miscadmin.h"
#include "access/xact.h"
#include "cdb/cdbfilerep.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/filesystem.h"

#include "utils/debugbreak.h"
/* Debug_filerep_print guc temporaly added for troubleshooting */
#include "utils/guc.h"
#include "utils/faultinjector.h"

#include "utils/memutils.h"

// Provide some indirection here in case we have problems with lseek and
// 64 bits on some platforms
#define pg_lseek64(a,b,c) (int64)lseek(a,b,c)


/*
 * We must leave some file descriptors free for system(), the dynamic loader,
 * and other code that tries to open files without consulting fd.c.  This
 * is the number left free.  (While we can be pretty sure we won't get
 * EMFILE, there's never any guarantee that we won't get ENFILE due to
 * other processes chewing up FDs.	So it's a bad idea to try to open files
 * without consulting fd.c.  Nonetheless we cannot control all code.)
 *
 * Because this is just a fixed setting, we are effectively assuming that
 * no such code will leave FDs open over the long term; otherwise the slop
 * is likely to be insufficient.  Note in particular that we expect that
 * loading a shared library does not result in any permanent increase in
 * the number of open files.  (This appears to be true on most if not
 * all platforms as of Feb 2004.)
 */
#define NUM_RESERVED_FDS		10

/*
 * If we have fewer than this many usable FDs after allowing for the reserved
 * ones, choke.
 */
#define FD_MINFREE				10


/*
 * A number of platforms allow individual processes to open many more files
 * than they can really support when *many* processes do the same thing.
 * This GUC parameter lets the DBA limit max_safe_fds to something less than
 * what the postmaster's initial probe suggests will work.
 */
int			max_files_per_process = 1000;

/*
 * Maximum number of file descriptors to open for either VFD entries or
 * AllocateFile/AllocateDir operations.  This is initialized to a conservative
 * value, and remains that way indefinitely in bootstrap or standalone-backend
 * cases.  In normal postmaster operation, the postmaster calls
 * set_max_safe_fds() late in initialization to update the value, and that
 * value is then inherited by forked subprocesses.
 *
 * Note: the value of max_files_per_process is taken into account while
 * setting this variable, and so need not be tested separately.
 */
static int	max_safe_fds = 32;	/* default if not changed */


/* Debugging.... */
#ifdef FDDEBUG
#define DO_DB(A) A
#else
#define DO_DB(A)				/* A */
#endif

#define VFD_CLOSED (-1)

#define FileIsValid(file) \
	((file) > 0 && (file) < (int) SizeVfdCache && VfdCache[file].fileName != NULL)

#define FileIsNotOpen(file) (VfdCache[file].fd == VFD_CLOSED && \
		(VfdCache[file].hFS == NULL && VfdCache[file].hFile == NULL))

#define FileUnknownPos INT64CONST(-1)

/* these are the assigned bits in fdstate below: */
#define FD_TEMPORARY		(1 << 0)	/* T = delete when closed */
#define FD_CLOSE_AT_EOXACT	(1 << 1)	/* T = close at eoXact */

typedef struct vfd
{
	int fd;			/* current FD, or VFD_CLOSED if none */
	unsigned short fdstate;		/* bitflags for VFD's state */
	SubTransactionId create_subid;		/* for TEMPORARY fds, creating subxact */
	File		nextFree;		/* link to next free VFD, if in freelist */
	File		lruMoreRecently;	/* doubly linked recency-of-use list */
	File		lruLessRecently;
	int64		seekPos;		/* current logical file position */
	char	   *fileName;		/* name of file, or NULL for unused VFD */
	/* NB: fileName is malloc'd, and must be free'd when closing the VFD */
	int			fileFlags;		/* open(2) flags for (re)opening the file */
	int			fileMode;		/* mode to pass to open(2) */
	hdfsFS hFS; /* hdfs file descriptor if this is a hdfs file, else NULL */
	hdfsFile hFile; /* hdfs filesystem if this is a hdfs file, else NULL */
	char *hProtocol; /* protocol of hdfs filesystem if this is a hdfs file, else NULL */
} Vfd;

/*
 * Virtual File Descriptor array pointer and size.	This grows as
 * needed.	'File' values are indexes into this array.
 * Note that VfdCache[0] is not a usable VFD, just a list header.
 */
static Vfd *VfdCache;
static Size SizeVfdCache = 0;

/*
 * Number of file descriptors known to be in use by VFD entries.
 */
static int	nfile = 0;

/*
 * List of stdio FILEs and <dirent.h> DIRs opened with AllocateFile
 * and AllocateDir.
 *
 * Since we don't want to encourage heavy use of AllocateFile or AllocateDir,
 * it seems OK to put a pretty small maximum limit on the number of
 * simultaneously allocated descs.
 */
#define MAX_ALLOCATED_DESCS  32

typedef enum
{
	AllocateDescFile,
	AllocateDescDir,
	AllocateDescRemoteDir
} AllocateDescKind;

typedef struct
{
	AllocateDescKind kind;
	union
	{
		FILE	   *file;
		DIR		   *dir;
	}			desc;
	SubTransactionId create_subid;

	/* Remote storage information. */
	char		protocol[MAXPGPATH];
	void		*filelist;
	size_t		cur;
	size_t		num;
	struct	dirent ret;
} AllocateDesc;

static int	numAllocatedDescs = 0;
static AllocateDesc allocatedDescs[MAX_ALLOCATED_DESCS];
static int	RecentRemoteAllocatedDesc = -1;

/*
 * Number of temporary files opened during the current session;
 * this is used in generation of tempfile names.
 */
static long tempFileCounter = 0;


/*--------------------
 *
 * Private Routines
 *
 * Delete		   - delete a file from the Lru ring
 * LruDelete	   - remove a file from the Lru ring and close its FD
 * Insert		   - put a file at the front of the Lru ring
 * LruInsert	   - put a file at the front of the Lru ring and open it
 * ReleaseLruFile  - Release an fd by closing the last entry in the Lru ring
 * AllocateVfd	   - grab a free (or new) file record (from VfdArray)
 * FreeVfd		   - free a file record
 *
 * The Least Recently Used ring is a doubly linked list that begins and
 * ends on element zero.  Element zero is special -- it doesn't represent
 * a file and its "fd" field always == VFD_CLOSED.	Element zero is just an
 * anchor that shows us the beginning/end of the ring.
 * Only VFD elements that are currently really open (have an FD assigned) are
 * in the Lru ring.  Elements that are "virtually" open can be recognized
 * by having a non-null fileName field.
 *
 * example:
 *
 *	   /--less----\				   /---------\
 *	   v		   \			  v			  \
 *	 #0 --more---> LeastRecentlyUsed --more-\ \
 *	  ^\									| |
 *	   \\less--> MostRecentlyUsedFile	<---/ |
 *		\more---/					 \--less--/
 *
 *--------------------
 */
static void Delete(File file);
static void LruDelete(File file);
static void Insert(File file);
static int	LruInsert(File file);
static bool ReleaseLruFile(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int	FileAccess(File file);
static void AtProcExit_Files(int code, Datum arg);
static void CleanupTempFiles(bool isProcExit);
static void RemovePgTempFilesInDir(const char *tmpdirname);
static bool HasTempFilePrefix(char * fileName);

static hdfsFS HdfsGetConnection(const char * protocol, const char * path);
static bool HdfsBasicOpenFile(FileName fileName, int fileFlags, int fileMode,
							  char **hProtocol, hdfsFS *fs, hdfsFile *hFile);
static bool HdfsGetProtocol(const char *fileName, char *buf, size_t size);
static const char * ConvertToUnixPath(const char * fileName, char * buffer,
		int len);
static int IsLocalPath(const char * fileName);
static int IsLocalFile(File file);

/*
 * pg_fsync --- do fsync with or without writethrough
 */
int
pg_fsync(int fd)
{
#ifndef HAVE_FSYNC_WRITETHROUGH_ONLY
	if (sync_method != SYNC_METHOD_FSYNC_WRITETHROUGH)
		return pg_fsync_no_writethrough(fd);
	else
#endif
		return pg_fsync_writethrough(fd);
}


/*
 * pg_fsync_no_writethrough --- same as fsync except does nothing if
 *	enableFsync is off
 */
int
pg_fsync_no_writethrough(int fd)
{
	if (enableFsync)
		return fsync(fd);
	else
		return 0;
}

/*
 * pg_fsync_writethrough
 */
int
pg_fsync_writethrough(int fd)
{
	if (enableFsync)
	{
#ifdef WIN32
		return _commit(fd);
#elif defined(F_FULLFSYNC)
		return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
		return -1;
#endif
	}
	else
		return 0;
}

/*
 * pg_fdatasync --- same as fdatasync except does nothing if enableFsync is off
 *
 * Not all platforms have fdatasync; treat as fsync if not available.
 */
int
pg_fdatasync(int fd)
{
	if (enableFsync)
	{
#ifdef HAVE_FDATASYNC
		return fdatasync(fd);
#else
		return fsync(fd);
#endif
	}
	else
		return 0;
}

/*
 * Retrying close in case it gets interrupted. If that happens, it will cause
 * unlink to fail later.
 */
int
gp_retry_close(int fd) {
	int err = 0;
	do
	{
		err = close(fd);
	} while (err == -1 && errno == EINTR);
	return err;
}

/*
 * InitFileAccess --- initialize this module during backend startup
 *
 * This is called during either normal or standalone backend start.
 * It is *not* called in the postmaster.
 */
void
InitFileAccess(void)
{
	Assert(SizeVfdCache == 0);	/* call me only once */

	/* initialize cache header entry */
	VfdCache = (Vfd *) malloc(sizeof(Vfd));
	if (VfdCache == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	MemSet((char *) &(VfdCache[0]), 0, sizeof(Vfd));
	VfdCache->fd = VFD_CLOSED;

	SizeVfdCache = 1;

	/* register proc-exit hook to ensure temp files are dropped at exit */
	on_proc_exit(AtProcExit_Files, 0);
}

/*
 * count_usable_fds --- count how many FDs the system will let us open,
 *		and estimate how many are already open.
 *
 * We stop counting if usable_fds reaches max_to_probe.  Note: a small
 * value of max_to_probe might result in an underestimate of already_open;
 * we must fill in any "gaps" in the set of used FDs before the calculation
 * of already_open will give the right answer.	In practice, max_to_probe
 * of a couple of dozen should be enough to ensure good results.
 *
 * We assume stdin (FD 0) is available for dup'ing
 */
static void
count_usable_fds(int max_to_probe, int *usable_fds, int *already_open)
{
	int		   *fd;
	int			size;
	int			used = 0;
	int			highestfd = 0;
	int			j;

	size = 1024;
	fd = (int *) palloc(size * sizeof(int));

	/* dup until failure or probe limit reached */
	for (;;)
	{
		int			thisfd;

		thisfd = dup(0);
		if (thisfd < 0)
		{
			/* Expect EMFILE or ENFILE, else it's fishy */
			if (errno != EMFILE && errno != ENFILE)
			{
				insist_log(false, "dup(0) failed after %d successes: %m", used);
			}
			break;
		}

		if (used >= size)
		{
			size *= 2;
			fd = (int *) repalloc(fd, size * sizeof(int));
		}
		fd[used++] = thisfd;

		if (highestfd < thisfd)
			highestfd = thisfd;

		if (used >= max_to_probe)
			break;
	}

	/* release the files we opened */
	for (j = 0; j < used; j++)
		close(fd[j]);

	pfree(fd);

	/*
	 * Return results.	usable_fds is just the number of successful dups. We
	 * assume that the system limit is highestfd+1 (remember 0 is a legal FD
	 * number) and so already_open is highestfd+1 - usable_fds.
	 */
	*usable_fds = used;
	*already_open = highestfd + 1 - used;
}

/*
 * set_max_safe_fds
 *		Determine number of filedescriptors that fd.c is allowed to use
 */
void
set_max_safe_fds(void)
{
	int			usable_fds;
	int			already_open;

	/*----------
	 * We want to set max_safe_fds to
	 *			MIN(usable_fds, max_files_per_process - already_open)
	 * less the slop factor for files that are opened without consulting
	 * fd.c.  This ensures that we won't exceed either max_files_per_process
	 * or the experimentally-determined EMFILE limit.
	 *----------
	 */
	count_usable_fds(max_files_per_process,
					 &usable_fds, &already_open);

	max_safe_fds = Min(usable_fds, max_files_per_process - already_open);

	/*
	 * Take off the FDs reserved for system() etc.
	 */
	max_safe_fds -= NUM_RESERVED_FDS;

	/*
	 * Make sure we still have enough to get by.
	 */
	if (max_safe_fds < FD_MINFREE)
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient file handles available to start server process"),
				 errdetail("System allows %d, we need at least %d.",
						   max_safe_fds + NUM_RESERVED_FDS,
						   FD_MINFREE + NUM_RESERVED_FDS)));

	elog(DEBUG2, "max_safe_fds = %d, usable_fds = %d, already_open = %d",
		 max_safe_fds, usable_fds, already_open);
}

/*
 * BasicOpenFile --- same as open(2) except can free other FDs if needed
 *
 * This is exported for use by places that really want a plain kernel FD,
 * but need to be proof against running out of FDs.  Once an FD has been
 * successfully returned, it is the caller's responsibility to ensure that
 * it will not be leaked on ereport()!	Most users should *not* call this
 * routine directly, but instead use the VFD abstraction level, which
 * provides protection against descriptor leaks as well as management of
 * files that need to be open for more than a short period of time.
 *
 * Ideally this should be the *only* direct call of open() in the backend.
 * In practice, the postmaster calls open() directly, and there are some
 * direct open() calls done early in backend startup.  Those are OK since
 * this module wouldn't have any open files to close at that point anyway.
 */
int
BasicOpenFile(FileName fileName, int fileFlags, int fileMode)
{
	int			fd;

tryAgain:
	fd = open(fileName, fileFlags, fileMode);

	if (fd >= 0)
		return fd;				/* success! */

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file handles: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto tryAgain;
		errno = save_errno;
	}

	return -1;					/* failure */
}

#if defined(FDDEBUG)

static void
_dump_lru(void)
{
	int			mru = VfdCache[0].lruLessRecently;
	Vfd		   *vfdP = &VfdCache[mru];
	char		buf[2048];

	snprintf(buf, sizeof(buf), "LRU: MOST %d ", mru);
	while (mru != 0)
	{
		mru = vfdP->lruLessRecently;
		vfdP = &VfdCache[mru];
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d ", mru);
	}
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "LEAST");
	elog(LOG, "%s", buf);
}
#endif   /* FDDEBUG */

static void
Delete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Delete %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	VfdCache[vfdP->lruLessRecently].lruMoreRecently = vfdP->lruMoreRecently;
	VfdCache[vfdP->lruMoreRecently].lruLessRecently = vfdP->lruLessRecently;

	DO_DB(_dump_lru());
}

static void
LruDelete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruDelete %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	/* delete the vfd record from the LRU ring */
	Delete(file);

	/* save the seek position */
	if (IsLocalFile(file))
		vfdP->seekPos = pg_lseek64(VfdCache[file].fd, 0, SEEK_CUR);
	else
		vfdP->seekPos = (int64)HdfsTell(VfdCache[file].hProtocol, VfdCache[file].hFS,
				VfdCache[file].hFile);
	Insist(vfdP->seekPos != INT64CONST(-1));

	/* close the file */
	if (IsLocalFile(file))
	{
	if (close(vfdP->fd))
			elog(ERROR, "could not close file \"%s\": %m", vfdP->fileName);
	} else {
		if (HdfsCloseFile(VfdCache[file].hProtocol, VfdCache[file].hFS, VfdCache[file].hFile))
			elog(ERROR, "could not close file \"%s\": %m", vfdP->fileName);
	}

	--nfile;
	vfdP->fd = VFD_CLOSED;
	vfdP->hFS = NULL;
	vfdP->hFile = NULL;
	if(vfdP->hProtocol)
	{
		free(vfdP->hProtocol);
		vfdP->hProtocol = NULL;
}
}

static void
Insert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Insert %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	vfdP->lruMoreRecently = 0;
	vfdP->lruLessRecently = VfdCache[0].lruLessRecently;
	VfdCache[0].lruLessRecently = file;
	VfdCache[vfdP->lruLessRecently].lruMoreRecently = file;

	DO_DB(_dump_lru());
}

/* returns 0 on success, -1 on re-open failure (with errno set) */
static int
LruInsert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruInsert %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (FileIsNotOpen(file))
	{
	    elog(LOG, "reopen file %s with flag %o", vfdP->fileName, vfdP->fileFlags);

		/*
		 * The open could still fail for lack of file descriptors, eg due to
		 * overall system file table being full.  So, be prepared to release
		 * another FD if necessary...
		 */
		if (IsLocalFile(file))
		{
		while (nfile + numAllocatedDescs >= max_safe_fds)
		{
			if (!ReleaseLruFile())
				break;
		}

		vfdP->fd = BasicOpenFile(vfdP->fileName, vfdP->fileFlags,
								 vfdP->fileMode);
		if (vfdP->fd < 0)
		{
			DO_DB(elog(LOG, "RE_OPEN FAILED: %d", errno));
			return vfdP->fd;
			} else
			{
				DO_DB(elog(LOG, "RE_OPEN SUCCESS"));
				++nfile;
			}
		}
		else
		{
			if(!HdfsBasicOpenFile(vfdP->fileName, vfdP->fileFlags, vfdP->fileMode,
								 &vfdP->hProtocol, &vfdP->hFS, &vfdP->hFile))
			{
				DO_DB(elog(LOG, "RE_OPEN FAILED: %d", errno));
				return -1;
			} else {
			DO_DB(elog(LOG, "RE_OPEN SUCCESS"));
				//++nfile;
			}
		}

		/* seek to the right position */
		if (vfdP->seekPos != INT64CONST(0))
		{
			int64		returnValue;
            if (IsLocalFile(file))
            {
			returnValue = pg_lseek64(vfdP->fd, vfdP->seekPos, SEEK_SET);
                if (returnValue < 0)
                    return -1;
            } else
            {
                if (vfdP->fileFlags & O_WRONLY) {
                    /*
                     * open for write, only support append on hdfs
                     */
                    int64 len = (int64) HdfsTell(vfdP->hProtocol, vfdP->hFS, vfdP->hFile);
                    if (vfdP->fileFlags & O_APPEND) {
                        if (vfdP->seekPos != len) {
                            elog(WARNING, "hdfs file %s length "INT64_FORMAT
                                    " is not equal to logic file length "INT64_FORMAT,
                                    vfdP->fileName, len, vfdP->seekPos);
							return -1;
                        }
                    } else {
                        elog(WARNING, "hdfs file %s should be open as APPEND", vfdP->fileName);
						return -1;
                    }
                } else {
                    /*
                     * open for read
                     */
                    if (HdfsSeek(vfdP->hProtocol, vfdP->hFS, vfdP->hFile, vfdP->seekPos))
                        return -1;
                }
            }
		}
	}

	/*
	 * put it at the head of the Lru ring
	 */
	if (IsLocalFile(file))
	Insert(file);

	return 0;
}

static bool
ReleaseLruFile(void)
{
	DO_DB(elog(LOG, "ReleaseLruFile. Opened %d", nfile));

	if (nfile > 0)
	{
		/*
		 * There are opened files and so there should be at least one used vfd
		 * in the ring.
		 */
		Assert(VfdCache[0].lruMoreRecently != 0);
		LruDelete(VfdCache[0].lruMoreRecently);
		return true;			/* freed a file */
	}
	return false;				/* no files available to free */
}

static File
AllocateVfd(void)
{
	Index		i;
	File		file;

	DO_DB(elog(LOG, "AllocateVfd. Size %lu", SizeVfdCache));

	Assert(SizeVfdCache > 0);	/* InitFileAccess not called? */

	if (VfdCache[0].nextFree == 0)
	{
		/*
		 * The free list is empty so it is time to increase the size of the
		 * array.  We choose to double it each time this happens. However,
		 * there's not much point in starting *real* small.
		 */
		Size		newCacheSize = SizeVfdCache * 2;
		Vfd		   *newVfdCache;

		if (newCacheSize < 32)
			newCacheSize = 32;

		/*
		 * Be careful not to clobber VfdCache ptr if realloc fails.
		 */
		newVfdCache = (Vfd *) realloc(VfdCache, sizeof(Vfd) * newCacheSize);
		if (newVfdCache == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		VfdCache = newVfdCache;

		/*
		 * Initialize the new entries and link them into the free list.
		 */
		for (i = SizeVfdCache; i < newCacheSize; i++)
		{
			MemSet((char *) &(VfdCache[i]), 0, sizeof(Vfd));
			VfdCache[i].nextFree = i + 1;
			VfdCache[i].fd = VFD_CLOSED;
			VfdCache[i].hFS = NULL;
			VfdCache[i].hFile = NULL;
			VfdCache[i].hProtocol = NULL;
		}
		VfdCache[newCacheSize - 1].nextFree = 0;
		VfdCache[0].nextFree = SizeVfdCache;

		/*
		 * Record the new size
		 */
		SizeVfdCache = newCacheSize;
	}

	file = VfdCache[0].nextFree;

	VfdCache[0].nextFree = VfdCache[file].nextFree;

	return file;
}

static void
FreeVfd(File file)
{
	Vfd		   *vfdP = &VfdCache[file];

	DO_DB(elog(LOG, "FreeVfd: %d (%s)",
			   file, vfdP->fileName ? vfdP->fileName : ""));

	if (vfdP->fileName != NULL)
	{
		free(vfdP->fileName);
		vfdP->fileName = NULL;
	}
	vfdP->fdstate = 0x0;
	vfdP->hFS = NULL;
	vfdP->hFile = NULL;
	if(vfdP->hProtocol)
	{
		free(vfdP->hProtocol);
		vfdP->hProtocol = NULL;
	}

	vfdP->nextFree = VfdCache[0].nextFree;
	VfdCache[0].nextFree = file;
}

/* returns 0 on success, -1 on re-open failure (with errno set) */
static int
FileAccess(File file)
{
	int			returnValue;

	DO_DB(elog(LOG, "FileAccess %d (%s)",
			   file, VfdCache[file].fileName));

	/*
	 * Is the file open?  If not, open it and put it at the head of the LRU
	 * ring (possibly closing the least recently used file to get an FD).
	 */

	if (FileIsNotOpen(file))
	{
		returnValue = LruInsert(file);
		if (returnValue != 0)
			return returnValue;
	}
	else if (VfdCache[0].lruLessRecently != file && IsLocalFile(file))
	{
		/*
		 * We now know that the file is open and that it is not the last one
		 * accessed, so we need to move it to the head of the Lru ring.
		 * Don't put HDFS File into LRU list
		 */

		Delete(file);
		Insert(file);
	}

	return 0;
}

/*
 *	Called when we get a shared invalidation message on some relation.
 */
#ifdef NOT_USED
void
FileInvalidate(File file)
{
	Assert(FileIsValid(file));
	if (!FileIsNotOpen(file))
		LruDelete(file);
}
#endif


/*
 * open a file in an arbitrary directory
 *
 * NB: if the passed pathname is relative (which it usually is),
 * it will be interpreted relative to the process' working directory
 * (which should always be $PGDATA when this code is running).
 */
File LocalPathNameOpenFile(FileName fileName, int fileFlags, int fileMode) {
	char	   *fnamecopy;
	File		file;
	Vfd		   *vfdP;

	DO_DB(elog(LOG, "PathNameOpenFile: %s %x %o",
			   fileName, fileFlags, fileMode));

	/*
	 * We need a malloc'd copy of the file name; fail cleanly if no room.
	 */
	fnamecopy = strdup(fileName);
	if (fnamecopy == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	file = AllocateVfd();
	vfdP = &VfdCache[file];

	while (nfile + numAllocatedDescs >= max_safe_fds)
	{
		if (!ReleaseLruFile())
			break;
	}

	vfdP->fd = BasicOpenFile(fileName, fileFlags, fileMode);

	if (vfdP->fd < 0)
	{
		FreeVfd(file);
		free(fnamecopy);
		return -1;
	}
	++nfile;
	DO_DB(elog(LOG, "PathNameOpenFile: success %d",
			   vfdP->fd));

	Insert(file);

	vfdP->fileName = fnamecopy;
	/* Saved flags are adjusted to be OK for re-opening file */
	vfdP->fileFlags = fileFlags & ~(O_CREAT | O_TRUNC | O_EXCL);
	vfdP->fileMode = fileMode;
	vfdP->seekPos = INT64CONST(0);
	vfdP->fdstate = 0x0;

	return file;
}

/*
 * open a file in the database directory ($PGDATA/base/DIROID/)
 * if we are using the system default filespace. Otherwise open
 * the file in the filespace configured for temporary files.
 * The passed name MUST be a relative path.  Effectively, this
 * prepends DatabasePath or path of the filespace to it and then 
 * acts like PathNameOpenFile.
 */
File
FileNameOpenFile(FileName fileName, int fileFlags, int fileMode)
{
	File		fd;
	char	   *fname;

	Assert(!is_absolute_path(fileName));
	fname = (char*)palloc(PATH_MAX);
	if (snprintf(fname, PATH_MAX, "%s/%s", getCurrentTempFilePath, fileName) > PATH_MAX)
	{
		ereport(ERROR, (errmsg("cannot generate path %s/%s", getCurrentTempFilePath,
                        fileName)));
	}
	fd = PathNameOpenFile(fname, fileFlags, fileMode);
	pfree(fname);
	return fd;
}

/*
 * Open a temporary file that will (optionally) disappear when we close it.
 *
 * If 'makenameunique' is true, this function generates a file name which
 * should be unique to this particular OpenTemporaryFile() request and
 * distinct from any others in concurrent use on the same host.  As a
 * convenience for monitoring and debugging, the given 'fileName' string
 * and 'extentseqnum' are embedded in the file name.
 *
 * If 'makenameunique' is false, then 'fileName' and 'extentseqnum' identify a
 * new or existing temporary file which other processes also could open and
 * share.
 *
 * If 'create' is true, a new file is created.  If successful, a valid vfd
 * index (>0) is returned; otherwise an error is thrown.
 *
 * If 'create' is false, an existing file is opened.  If successful, a valid
 * vfd index (>0) is returned.  If the file does not exist or cannot be
 * opened, an invalid vfd index (<= 0) is returned.
 *
 * If 'delOnClose' is true, then the file is removed when you call
 * FileClose(); or when the process exits; or (provided 'closeAtEOXact' is
 * true) when the transaction ends.
 *
 * If 'closeAtEOXact' is true, the vfd is closed automatically at end of
 * transaction unless you have called FileClose() to close it before then.
 * If 'closeAtEOXact' is false, the vfd state is not changed at end of
 * transaction.
 *
 * In most cases, you don't want temporary files to outlive the transaction
 * that created them, so you should specify 'true' for both 'delOnClose' and
 * 'closeAtEOXact'.
 */
File
OpenTemporaryFile(const char   *fileName,
                  int           extentseqnum,
                  bool          makenameunique,
                  bool          create,
                  bool          delOnClose,
                  bool          closeAtEOXact)
{
	File    file;
    int     fileFlags;
	char	tempfilepath[MAXPGPATH];

	Assert(fileName);
    AssertImply(makenameunique, create && delOnClose);

	/*
	 * File flags when open the file.  Note: we don't use O_EXCL, in case there is an orphaned
	 * temp file that can be reused.
	 */
	fileFlags = O_RDWR | PG_BINARY;
    if (create)
        fileFlags |= O_TRUNC | O_CREAT;

    char tempfileprefix[MAXPGPATH];

    int len = GetTempFilePrefix(tempfileprefix, MAXPGPATH, fileName);
    insist_log(len <= MAXPGPATH - 1, "could not generate temporary file name");

    if (makenameunique)
	{
		/*
		 * Generate a tempfile name that should be unique within the current
		 * database instance.
		 */
		snprintf(tempfilepath, sizeof(tempfilepath),
				 "%s_%d_%04d.%ld",
				 tempfileprefix,
				 MyProcPid,
                 extentseqnum,
                 tempFileCounter++);
	}
	else
	{
        snprintf(tempfilepath, sizeof(tempfilepath),
				 "%s.%04d",
				 tempfileprefix,
                 extentseqnum);
	}

	file = FileNameOpenFile(tempfilepath, fileFlags, 0600);

	if (file <= 0)
	{
		char	   *dirpath;

		if (!create)
			return file;

		/*
		 * We might need to create the pg_tempfiles subdirectory, if no one
		 * has yet done so.
		 *
		 * Don't check for error from mkdir; it could fail if someone else
		 * just did the same thing.  If it doesn't work then we'll bomb out on
		 * the second create attempt, instead.
		 */
		dirpath = (char*)palloc(PATH_MAX);
		snprintf(dirpath, PATH_MAX, "%s/%s", getCurrentTempFilePath, PG_TEMP_FILES_DIR);
		mkdir(dirpath, S_IRWXU);
		pfree(dirpath);

		file = FileNameOpenFile(tempfilepath, fileFlags, 0600);
		if (file <= 0)
			elog(ERROR, "could not create temporary file \"%s\": %m",
			     tempfilepath);
	}

	/* Mark it for deletion at close */
	if(delOnClose)
		VfdCache[file].fdstate |= FD_TEMPORARY;

	/* Mark it to be closed at end of transaction. */
	if (closeAtEOXact)
	{
		VfdCache[file].fdstate |= FD_CLOSE_AT_EOXACT;
		VfdCache[file].create_subid = GetCurrentSubTransactionId();
	}

	return file;
}                               /* OpenTemporaryFile */

/*
 * close a file when done with it
 */
void LocalFileClose(File file) {
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileClose: %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (!FileIsNotOpen(file))
	{
		/* remove the file from the lru ring */
		Delete(file);

		/* close the file */
		if (gp_retry_close(vfdP->fd))
			elog(ERROR, "could not close file \"%s\": %m",
				 vfdP->fileName);

		--nfile;
		vfdP->fd = VFD_CLOSED;
	}

	/*
	 * Delete the file if it was temporary
	 */
	if (vfdP->fdstate & FD_TEMPORARY)
	{
		/* reset flag so that die() interrupt won't cause problems */
		vfdP->fdstate &= ~FD_TEMPORARY;
		if (unlink(vfdP->fileName))
			elog(DEBUG1, "failed to unlink \"%s\": %m",
				 vfdP->fileName);
	}

	/*
	 * Return the Vfd slot to the free list
	 */
	FreeVfd(file);
}

/*
 * close a file and forcibly delete the underlying Unix file
 */
void
FileUnlink(File file)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileUnlink: %d (%s)",
	     file, VfdCache[file].fileName));

	/* force FileClose to delete it */
	VfdCache[file].fdstate |= FD_TEMPORARY;

	FileClose(file);
}

int LocalFileRead(File file, char *buffer, int amount) {
	return FileReadIntr(file, buffer, amount, true);
}

int
FileReadIntr(File file, char *buffer, int amount, bool fRetryIntr)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileRead: %d (%s) " INT64_FORMAT " %d %p",
			   file, VfdCache[file].fileName,
			   VfdCache[file].seekPos, amount, buffer));

	if (Debug_filerep_print)
		(elog(LOG, "FileRead: %d (%s) " INT64_FORMAT " %d %p",
			  file, VfdCache[file].fileName,
			  VfdCache[file].seekPos, amount, buffer));
	
	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

retry:
	returnCode = read(VfdCache[file].fd, buffer, amount);

	if (returnCode >= 0)
		VfdCache[file].seekPos += returnCode;
	else
	{
		/*
		 * Windows may run out of kernel buffers and return "Insufficient
		 * system resources" error.  Wait a bit and retry to solve it.
		 *
		 * It is rumored that EINTR is also possible on some Unix filesystems,
		 * in which case immediate retry is indicated.
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* OK to retry if interrupted */
		if (errno == EINTR && fRetryIntr)
			goto retry;

		/* Trouble, so assume we don't know the file position anymore */
		VfdCache[file].seekPos = FileUnknownPos;
	}

	return returnCode;
}

int LocalFileWrite(File file, const char *buffer, int amount) {
	int			returnCode;
	FileRepGpmonRecord_s gpmonRecord;
	FileRepGpmonStatType_e whichStat =0;

	if (fileRepRole == FileRepPrimaryRole)
	{
			whichStat = FileRepGpmonStatType_PrimaryWriteSyscall;
			FileRepGpmonStat_OpenRecord(whichStat, &gpmonRecord);
			gpmonRecord.size = amount;

	} else if (fileRepRole == FileRepMirrorRole)
	{
			whichStat = FileRepGpmonStatType_MirrorWriteSyscall;
			FileRepGpmonStat_OpenRecord(whichStat, &gpmonRecord);
			gpmonRecord.size = amount;

	}

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWrite: %d (%s) " INT64_FORMAT " %d %p",
			   file, VfdCache[file].fileName,
			   VfdCache[file].seekPos, amount, buffer));
	
	/* Added temporary for troubleshooting */
	if (Debug_filerep_print)
		elog(LOG, "FileWrite: %d (%s) " INT64_FORMAT " %d %p",
		 file, VfdCache[file].fileName,
		 VfdCache[file].seekPos, amount, buffer);
	else
		FileRep_InsertLogEntry(
							   "FileWrite",
							   FileRep_GetFlatFileIdentifier(VfdCache[file].fileName, ""),
							   FileRepRelationTypeFlatFile,
							   FileRepOperationWrite,
							   FILEREP_UNDEFINED,
							   FILEREP_UNDEFINED,
							   FileRepAckStateNotInitialized,
							   VfdCache[file].seekPos,
							   amount);		
	
	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;
	
#ifdef FAULT_INJECTOR	
	if (! strcmp(VfdCache[file].fileName, "global/pg_control"))
	{
		if (FaultInjector_InjectFaultIfSet(
										   PgControl, 
										   DDLNotSpecified,
										   "" /* databaseName */,
										   "" /* tableName */) == FaultInjectorTypeDataCorruption)
		{
			MemSet(buffer, 0, amount);
		}
	}
	
	if (strstr(VfdCache[file].fileName, "pg_xlog/"))
	{
		if (FaultInjector_InjectFaultIfSet(
										   PgXlog, 
										   DDLNotSpecified,
										   "" /* databaseName */,
										   "" /* tableName */) == FaultInjectorTypeDataCorruption)
		{
			MemSet(buffer, 0, amount);
		}
	}
	
	
#endif	
	
retry:
	errno = 0;
	returnCode = write(VfdCache[file].fd, buffer, amount);

	/* if write didn't set errno, assume problem is no disk space */
	if (returnCode != amount && errno == 0)
		errno = ENOSPC;

	if (returnCode >= 0)
		VfdCache[file].seekPos += returnCode;
	else
	{
		/*
		 * See comments in FileRead()
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* OK to retry if interrupted */
		if (errno == EINTR)
			goto retry;

		/* Trouble, so assume we don't know the file position anymore */
		VfdCache[file].seekPos = FileUnknownPos;
	}

	if (returnCode >=0 )
	{
			//only include stat if successful
			if ((fileRepRole == FileRepPrimaryRole) || 
				(fileRepRole == FileRepMirrorRole))
			{
					FileRepGpmonStat_CloseRecord(whichStat, &gpmonRecord);
			}
	}
	return returnCode;
}

int LocalFileSync(File file) {
	int			returnCode;
	FileRepGpmonRecord_s gpmonRecord;
	FileRepGpmonStatType_e whichStat;

	if (fileRepRole == FileRepPrimaryRole)
	{
			whichStat = FileRepGpmonStatType_PrimaryFsyncSyscall;
			FileRepGpmonStat_OpenRecord(whichStat, &gpmonRecord);
	} else 
	{
			whichStat = FileRepGpmonStatType_MirrorFsyncSyscall;
			FileRepGpmonStat_OpenRecord(whichStat, &gpmonRecord);
	}
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSync: %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

#ifdef FAULT_INJECTOR
	FaultInjector_InjectFaultIfSet(
								   FileRepFlush,
								   DDLNotSpecified,
								   "",	//databaseName
								   ""); // tableName
#endif								
	
	returnCode =  pg_fsync(VfdCache[file].fd);
	
	if (returnCode >= 0)
	{
			//only include stats if successful
			if ((fileRepRole == FileRepPrimaryRole) || 
				(fileRepRole == FileRepMirrorRole))
			{
					FileRepGpmonStat_CloseRecord(whichStat, &gpmonRecord);
			}
	}
	return returnCode;
}

int64 LocalFileSeek(File file, int64 offset, int whence) {
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSeek: %d (%s) " INT64_FORMAT " " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   VfdCache[file].seekPos, offset, whence));

	if (FileIsNotOpen(file)) {
		switch (whence) {
			case SEEK_SET:
				Assert(offset >= INT64CONST(0));
				VfdCache[file].seekPos = offset;
				break;
			case SEEK_CUR:
				VfdCache[file].seekPos += offset;
				break;
			case SEEK_END:
				returnCode = FileAccess(file);
				if (returnCode < 0)
					return returnCode;
				VfdCache[file].seekPos = pg_lseek64(VfdCache[file].fd,
											   offset, whence);
				break;
			default:
				Assert(!"invalid whence");
				break;
		}
	} else {
		switch (whence) {
			case SEEK_SET:
				Assert(offset >= INT64CONST(0));
				if (VfdCache[file].seekPos != offset)
					VfdCache[file].seekPos = pg_lseek64(VfdCache[file].fd,
												   offset, whence);
				break;
			case SEEK_CUR:
				if (offset != 0 || VfdCache[file].seekPos == FileUnknownPos)
					VfdCache[file].seekPos = pg_lseek64(VfdCache[file].fd,
												   offset, whence);
				break;
			case SEEK_END:
				VfdCache[file].seekPos = pg_lseek64(VfdCache[file].fd,
											   offset, whence);
				break;
			default:
				Assert(!"invalid whence");
				break;
		}
	}
	return VfdCache[file].seekPos;
}

int64 FileNonVirtualTell(File file) {
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileNonVirtualTell: %d (%s) virtual position " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   VfdCache[file].seekPos));
	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	if (IsLocalFile(file))
	return pg_lseek64(VfdCache[file].fd, 0, SEEK_CUR);
	else
		return HdfsFileTell(file);
}

/*
 * XXX not actually used but here for completeness
 */
#ifdef NOT_USED
int64
FileTell(File file)
{
	Assert(FileIsValid(file));
	DO_DB(elog(LOG, "FileTell %d (%s)",
			   file, VfdCache[file].fileName));
	return VfdCache[file].seekPos;
}
#endif

/*
 * remove a local path
 *
 * return 0 on failure, non-zero on success
 */
int LocalRemovePath(FileName fileName, int recursive) {
	if (!recursive)
		return !unlink(fileName);
	else
		return rmtree(fileName, true);
}

int LocalFileTruncate(File file, int64 offset) {
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileTruncate %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	/*
	 * Call ftruncate with a int64 value.
	 *
	 * WARNING:DO NOT typecast this down to a 32-bit long or
	 * append-only vacuum full adjustment of the eof will erroneously remove
	 * table data.
	 */
	returnCode = ftruncate(VfdCache[file].fd, offset);
	
	/* Assume we don't know the file position anymore */
	VfdCache[file].seekPos = FileUnknownPos;
		
	return returnCode;
}


/*
 * Routines that want to use stdio (ie, FILE*) should use AllocateFile
 * rather than plain fopen().  This lets fd.c deal with freeing FDs if
 * necessary to open the file.	When done, call FreeFile rather than fclose.
 *
 * Note that files that will be open for any significant length of time
 * should NOT be handled this way, since they cannot share kernel file
 * descriptors with other files; there is grave risk of running out of FDs
 * if anyone locks down too many FDs.  Most callers of this routine are
 * simply reading a config file that they will read and close immediately.
 *
 * fd.c will automatically close all files opened with AllocateFile at
 * transaction commit or abort; this prevents FD leakage if a routine
 * that calls AllocateFile is terminated prematurely by ereport(ERROR).
 *
 * Ideally this should be the *only* direct call of fopen() in the backend.
 */
FILE *
AllocateFile(const char *name, const char *mode)
{
	FILE	   *file;

	DO_DB(elog(LOG, "AllocateFile: Allocated %d (%s)",
			   numAllocatedDescs, name));

	/*
	 * The test against MAX_ALLOCATED_DESCS prevents us from overflowing
	 * allocatedFiles[]; the test against max_safe_fds prevents AllocateFile
	 * from hogging every one of the available FDs, which'd lead to infinite
	 * looping.
	 */
	if (numAllocatedDescs >= MAX_ALLOCATED_DESCS ||
		numAllocatedDescs >= max_safe_fds - 1)
		elog(ERROR, "could not allocate file: out of file handles");

TryAgain:
	if ((file = fopen(name, mode)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescFile;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file handles: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * Free an AllocateDesc of either type.
 *
 * The argument *must* point into the allocatedDescs[] array.
 */
static int
FreeDesc(AllocateDesc *desc)
{
	int			result = 0;

	/* Close the underlying object */
	switch (desc->kind)
	{
		case AllocateDescFile:
			result = fclose(desc->desc.file);
			break;
		case AllocateDescDir:
			result = closedir(desc->desc.dir);
			break;
		case AllocateDescRemoteDir:
			HdfsFreeFileInfo(desc->protocol, (HdfsFileInfo *) desc->filelist, desc->num);
			free(desc->desc.dir);
			result = true;
			break;
		default:
			Assert(false);
			break;
	}

	/* Compact storage in the allocatedDescs array */
	numAllocatedDescs--;
	*desc = allocatedDescs[numAllocatedDescs];

	return result;
}

/*
 * Close a file returned by AllocateFile.
 *
 * Note we do not check fclose's return value --- it is up to the caller
 * to handle close errors.
 */
int
FreeFile(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "FreeFile: Allocated %d", numAllocatedDescs));

	/* Remove file from list of allocated files, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescFile && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a file not in allocatedDescs */
	elog(LOG, "file to be closed was not opened through the virtual file descriptor system");
	Assert(false);

	return fclose(file);
}


/*
 * Routines that want to use <dirent.h> (ie, DIR*) should use AllocateDir
 * rather than plain opendir().  This lets fd.c deal with freeing FDs if
 * necessary to open the directory, and with closing it after an elog.
 * When done, call FreeDir rather than closedir.
 *
 * Ideally this should be the *only* direct call of opendir() in the backend.
 */
DIR *
AllocateDir(const char *dirname)
{
	DIR		   *dir;

	DO_DB(elog(LOG, "AllocateDir: Allocated %d (%s)",
			   numAllocatedDescs, dirname));

	/*
	 * The test against MAX_ALLOCATED_DESCS prevents us from overflowing
	 * allocatedDescs[]; the test against max_safe_fds prevents AllocateDir
	 * from hogging every one of the available FDs, which'd lead to infinite
	 * looping.
	 */
	if (numAllocatedDescs >= MAX_ALLOCATED_DESCS ||
		numAllocatedDescs >= max_safe_fds - 1)
		elog(ERROR, "could not allocate directory: out of file handles");

	if (!IsLocalPath(dirname))
	{
		int				num;
		HdfsFileInfo	*info;
		char			protocol[MAXPGPATH];
		char			unixpath[MAXPGPATH];
		hdfsFS			fs;
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		/* Remote storage */
		if (!HdfsGetProtocol(dirname, protocol, sizeof(protocol)))
			return NULL;
		if (ConvertToUnixPath(dirname, unixpath, sizeof(unixpath)) == NULL)
			return NULL;
		if ((fs = HdfsGetConnection(protocol, dirname)) == NULL)
			return NULL;
		/* TODO: add to filesystem! */
		if ((info = hdfsListDirectory(fs, unixpath, &num)) == NULL)
			return NULL;

		/* FIXME: we just need to return something. */
		dir = (DIR *) malloc(1);
		if (dir == NULL)			
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
		desc->kind = AllocateDescRemoteDir;
		strncpy(desc->protocol, protocol, sizeof(desc->protocol));
		desc->desc.dir = dir;
		desc->create_subid = GetCurrentSubTransactionId();
		desc->filelist = (void *) info;
		desc->num = num;
		desc->cur = 0;

		numAllocatedDescs++;
		return desc->desc.dir;
	}

TryAgain:
	if ((dir = opendir(dirname)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescDir;
		desc->desc.dir = dir;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.dir;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file handles: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * Read a directory opened with AllocateDir, ereport'ing any error.
 *
 * This is easier to use than raw readdir() since it takes care of some
 * otherwise rather tedious and error-prone manipulation of errno.	Also,
 * if you are happy with a generic error message for AllocateDir failure,
 * you can just do
 *
 *		dir = AllocateDir(path);
 *		while ((dirent = ReadDir(dir, path)) != NULL)
 *			process dirent;
 *		FreeDir(dir);
 *
 * since a NULL dir parameter is taken as indicating AllocateDir failed.
 * (Make sure errno hasn't been changed since AllocateDir if you use this
 * shortcut.)
 *
 * The pathname passed to AllocateDir must be passed to this routine too,
 * but it is only used for error reporting.
 */
struct dirent *
ReadDir(DIR *dir, const char *dirname)
{
	struct dirent *dent;

	/* Give a generic message for AllocateDir failure, if caller didn't */
	if (dir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						dirname)));

	if (!IsLocalPath(dirname))
	{
		/* This cache value may be changed. But most of time it is correct! */
		int		cache_id = RecentRemoteAllocatedDesc;
		char	*fullname;
		char	*filename;
		AllocateDesc *desc;

		/*
		 * If the cache_id is not of range, not a remote dir, or not equal to
		 * dir pointer, we need to scan the desc array.
		 */
		if (cache_id < 0 ||
			cache_id >= numAllocatedDescs ||
			allocatedDescs[cache_id].kind != AllocateDescRemoteDir ||
			allocatedDescs[cache_id].desc.dir != dir)
		{
			for (cache_id = 0; cache_id < numAllocatedDescs; cache_id++)
			{
				desc = &allocatedDescs[cache_id];
				if (desc->kind == AllocateDescRemoteDir &&
					desc->desc.dir == dir)
					break;
			}

			/* Should not happen! */
			if (cache_id >= numAllocatedDescs)
				Assert(false);

			/* cache this result. */
			RecentRemoteAllocatedDesc = cache_id;
		}

		desc = &allocatedDescs[cache_id];
		dent = &desc->ret;
		/* No more element. */
		if (desc->cur >= desc->num)
			return NULL;

		fullname = ((HdfsFileInfo *) desc->filelist)[(desc->cur)++].mName;
		/* Get the file name instead of the absolute path. */
		filename = fullname + strlen(fullname);
		while (filename > fullname &&
			*(filename - 1) != '/')
			filename--;
		if (sizeof(dent->d_name) < (strlen(filename) + 1))
			elog(ERROR, "file name is too long \"%s\"", filename);

		snprintf(dent->d_name, sizeof(dent->d_name), "%s", filename);
		dent->d_name[strlen(filename)] = '\0';
		return dent;
	}

	errno = 0;
	if ((dent = readdir(dir)) != NULL)
		return dent;

#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but not in
	 * released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif

	if (errno)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m",
						dirname)));
	return NULL;
}

/*
 * Close a directory opened with AllocateDir.
 *
 * Note we do not check closedir's return value --- it is up to the caller
 * to handle close errors.
 */
int
FreeDir(DIR *dir)
{
	int			i;

	DO_DB(elog(LOG, "FreeDir: Allocated %d", numAllocatedDescs));

	/* Remove dir from list of allocated dirs, if it's present */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind != AllocateDescFile && desc->desc.dir == dir)
			return FreeDesc(desc);
	}

	/* Only get here if someone passes us a dir not in allocatedDescs */
	elog(LOG, "directory to be closed was not opened through the virtual file descriptor system");
	Assert(false);

	return closedir(dir);
}


/*
 * closeAllVfds
 *
 * Force all VFDs into the physically-closed state, so that the fewest
 * possible number of kernel file descriptors are in use.  There is no
 * change in the logical state of the VFDs.
 */
void
closeAllVfds(void)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));		/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if (!FileIsNotOpen(i))
				LruDelete(i);
		}
	}
}

/*
 * AtEOSubXact_Files
 *
 * Take care of subtransaction commit/abort.  At abort, we close temp files
 * that the subtransaction may have opened.  At commit, we reassign the
 * files that were opened to the parent subtransaction.
 */
void
AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
				  SubTransactionId parentSubid)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));		/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			unsigned short fdstate = VfdCache[i].fdstate;

			if ((fdstate & FD_CLOSE_AT_EOXACT) &&
				VfdCache[i].create_subid == mySubid)
			{
				if (isCommit)
					VfdCache[i].create_subid = parentSubid;
				else if (VfdCache[i].fileName != NULL)
					FileClose(i);
			}
		}
	}

	for (i = 0; i < numAllocatedDescs; i++)
	{
		if (allocatedDescs[i].create_subid == mySubid)
		{
			if (isCommit)
				allocatedDescs[i].create_subid = parentSubid;
			else
			{
				/* have to recheck the item after FreeDesc (ugly) */
				FreeDesc(&allocatedDescs[i--]);
			}
		}
	}
}

/*
 * AtEOXact_Files
 *
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All still-open per-transaction temporary file
 * VFDs are closed, which also causes the underlying files to be
 * deleted. Furthermore, all "allocated" stdio files are closed.
 */
void
AtEOXact_Files(void)
{
	CleanupTempFiles(false);
}

/*
 * AtProcExit_Files
 *
 * on_proc_exit hook to clean up temp files during backend shutdown.
 * Here, we want to clean up *all* temp files including interXact ones.
 */
static void
AtProcExit_Files(int code, Datum arg)
{
	CleanupTempFiles(true);
}

/*
 * Close temporary files and delete their underlying files.
 *
 * isProcExit: if true, this is being called as the backend process is
 * exiting. If that's the case, we should remove all temporary files; if
 * that's not the case, we are being called for transaction commit/abort
 * and should only remove transaction-local temp files.  In either case,
 * also clean up "allocated" stdio files and dirs.
 */
static void
CleanupTempFiles(bool isProcExit)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));		/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			unsigned short fdstate = VfdCache[i].fdstate;

			/*
			 * If we're in the process of exiting a backend process, close
			 * all temporary files. Otherwise, only close temporary files
			 * local to the current transaction.
			 */
			if((fdstate & FD_CLOSE_AT_EOXACT)
			  	||
			   (isProcExit && (fdstate & FD_TEMPORARY))
			  )
			{
				AssertImply( (fdstate & FD_TEMPORARY), VfdCache[i].fileName != NULL);
				FileClose(i);
			}
		}
	}

	while (numAllocatedDescs > 0)
		FreeDesc(&allocatedDescs[0]);
}

/*
 * Close all HDFS files
 */
static void
CloseAllHdfsFiles(void)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));		/* Make sure ring not corrupted */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if(FileIsNotOpen(i) || IsLocalFile(i))
				continue;

			DO_DB(elog(LOG, "CloseAllHdfsFiles: close %d. filename: %s", i, VfdCache[i].fileName));

			/* we are in transaction abort, cannot report error while close file */
			HdfsFileClose(i, false);
		}
	}
}

/*
 * Called while transaction was aborted
 */
void
AtXactCancle_Files(void)
{
	CloseAllHdfsFiles();
}

/*
 * Remove temporary files left over from a prior postmaster session
 *
 * This should be called during postmaster startup.  It will forcibly
 * remove any leftover files created by OpenTemporaryFile.
 *
 * NOTE: we could, but don't, call this during a post-backend-crash restart
 * cycle.  The argument for not doing it is that someone might want to examine
 * the temp files for debugging purposes.  This does however mean that
 * OpenTemporaryFile had better allow for collision with an existing temp
 * file name.
 */
void
RemovePgTempFiles(void)
{
	char		temp_path[MAXPGPATH];
	DIR		   *db_dir;
	struct dirent *db_de;

	/*
	 * Cycle through pgsql_tmp directories for all databases and remove old
	 * temp files.
	 */
	db_dir = AllocateDir("base");

	while ((db_de = ReadDir(db_dir, "base")) != NULL)
	{
		if (strcmp(db_de->d_name, ".") == 0 ||
			strcmp(db_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "base/%s/%s",
				 db_de->d_name, PG_TEMP_FILES_DIR);
		RemovePgTempFilesInDir(temp_path);
	}

	FreeDir(db_dir);

	/*
	 * In EXEC_BACKEND case there is a pgsql_tmp directory at the top level of
	 * DataDir as well.
	 */
#ifdef EXEC_BACKEND
	RemovePgTempFilesInDir(PG_TEMP_FILES_DIR);
#endif
}

/* Process one pgsql_tmp directory for RemovePgTempFiles */
static void
RemovePgTempFilesInDir(const char *tmpdirname)
{
	DIR		   *temp_dir;
	struct dirent *temp_de;
	char		rm_path[MAXPGPATH];

	temp_dir = AllocateDir(tmpdirname);
	if (temp_dir == NULL)
	{
		/* anything except ENOENT is fishy */
		if (errno != ENOENT)
			elog(LOG,
				 "could not open temporary-files directory \"%s\": %m",
				 tmpdirname);
		return;
	}

	while ((temp_de = ReadDir(temp_dir, tmpdirname)) != NULL)
	{
		if (strcmp(temp_de->d_name, ".") == 0 ||
			strcmp(temp_de->d_name, "..") == 0)
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 tmpdirname, temp_de->d_name);

		if (HasTempFilePrefix(temp_de->d_name))
		{
			unlink(rm_path);	/* note we ignore any error */
		}
		else
		{
			elog(LOG,
				 "unexpected file found in temporary-files directory: \"%s\"",
				 rm_path);
	}
	}

	FreeDir(temp_dir);
}

/*
 * Generate the prefix for a new temp file name. This will be checked
 * before cleaning up, to make sure we only delete what we created.
 * Caller is responsible for allocating the input buffer large enough
 * for the fileName and prefix.
 * Returns needlen > buflen if buffer is not large enough to store prefix.
 */
size_t
GetTempFilePrefix(char * buf, size_t buflen, const char * fileName)
{
	size_t needlen = strlen(PG_TEMP_FILES_DIR)
			+ strlen(PG_TEMP_FILE_PREFIX)
			+ strlen(fileName)
			+ 2; /* enough for a slash and a _ */

	if(buflen < needlen)
		return needlen;

	snprintf(buf, buflen, "%s/%s_%s",
			PG_TEMP_FILES_DIR,
			PG_TEMP_FILE_PREFIX,
			fileName);

	return needlen;
}
	
/*
 * Check if a temporary file matches the expected prefix. This
 * is done before deleting it as a sanity check.
 */
static bool HasTempFilePrefix(char * fileName)
{
	return (strncmp(fileName,
						PG_TEMP_FILE_PREFIX,
						strlen(PG_TEMP_FILE_PREFIX)) == 0);
}

/*
 * hash table of hdfs file systems, key = hdfs:/<host>:<port>, value = hdfsFS
 */
static HTAB * HdfsFsTable = NULL;
static MemoryContext HdfsGlobalContext = NULL;
#define EXPECTED_MAX_HDFS_CONNECTIONS 10

struct FsEntry {
	char host[MAXPGPATH + 1];
	hdfsFS fs;
};

//static const char * hdfs_prefix = "hdfs://";
static const char * fsys_protocol_sep = "://";
static const char * local_prefix = "local://";

/*
 * get/create a hdfs file system from path
 *
 * hdfs path schema :
 * 		hdfs:/<host>:<port>/...
 */
static hdfsFS HdfsGetConnection(const char * protocol, const char * path) {
	struct FsEntry * entry;
	HASHCTL hash_ctl;
	bool found;
	char * host = NULL, *p, *location = NULL;

	int port = 0;
	hdfsFS retval = NULL;

	do {
		p = strstr(path, fsys_protocol_sep);
		if (NULL == p) {
			elog(WARNING, "no filesystem protocol found: %s", path);
			errno = EINVAL;
			break;
		}

		/* skip option field. liugd TODO : ugly */
		p += strlen(fsys_protocol_sep);
		if (*p == '{') {
			p = strstr(p+1, "}") + 1;
		}

		host = palloc0(strlen(path));
		strcpy(host, p);
		p = strstr(host, ":");
		if (NULL == p) {
			elog(WARNING, "cannot find hdfs port in path: %s", path);
			errno = EINVAL;
			break;
		}
		*p++ = 0;
		errno = 0;
		port = (int) strtol(p, NULL, 0);
		if (EINVAL == errno || ERANGE == errno || !(0 < port && port < 65536)) {
			elog(WARNING, "cannot find hdfs port in path: %s", path);
			errno = EINVAL;
			break;
		}

		location = palloc0(strlen(host) + 10);
		sprintf(location, "%s:%d", host, port);

		if (NULL == HdfsFsTable) {
			if (NULL == HdfsGlobalContext) {
				Assert(NULL != TopMemoryContext);
				HdfsGlobalContext = AllocSetContextCreate(TopMemoryContext,
						"HDFS Global Context", ALLOCSET_DEFAULT_MINSIZE,
						ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
			}

			MemSet(&hash_ctl, 0, sizeof(hash_ctl));
			hash_ctl.keysize = MAXPGPATH;
			hash_ctl.entrysize = sizeof(struct FsEntry);
			hash_ctl.hash = string_hash;
			hash_ctl.hcxt = HdfsGlobalContext;

			HdfsFsTable = hash_create("hdfs connections hash table",
					EXPECTED_MAX_HDFS_CONNECTIONS, &hash_ctl,
					HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

			if (HdfsFsTable == NULL) {
				elog(WARNING, "failed to create hash table.");
				errno = EIO;
				break;
			}
		}

		entry = (struct FsEntry *) hash_search(HdfsFsTable, location,
				HASH_ENTER, &found);

		if (!found) {
			Assert(NULL != entry);
			DO_DB(elog(LOG, "connect webhdfs host: %s, port: %d", host, port));

			entry->fs = HdfsConnect(protocol, host, port);
			if (NULL == entry->fs) {
				hash_search(HdfsFsTable, location, HASH_REMOVE, &found);
				elog(WARNING, "fail to connect hdfs at %s, errno = %d", location,
						errno);
				break;
			}
		}

		Assert(NULL != entry && NULL != entry->fs);

		retval = entry->fs;

	} while (0);

	if (host)
		pfree(host);
	if (location)
		pfree(location);

	return retval;
}

/*
 * return non-zero if fileName is a well formated hdfs path
 */
static int IsLocalPath(const char * fileName) {
	if (0 == strncmp(fileName, local_prefix, strlen(local_prefix))) {
		return 1;
	}
	if (NULL == strstr(fileName, fsys_protocol_sep)) {
		return 1;
	}
	return 0;
}

/*
 * return non-zero if file is a hdfs file
 */
static int IsLocalFile(File file) {
	Vfd *vfdP;
	Assert(FileIsValid(file));
	vfdP = &VfdCache[file];
	if (NULL == vfdP->hFile && NULL == vfdP->hFS)
		return 1;
	return 0;
}

/*
 * get protocol from filepath
 */
static bool
HdfsGetProtocol(const char *fileName, char *buf, size_t size)
{
	const char	*p;

	p = strstr(fileName, fsys_protocol_sep);
	if (NULL == p) {
		elog(WARNING, "internal error: no filesystem protocol found in path \"%s\"",
				fileName);
		errno = EINVAL;
		return false;
	}

	if (size < ((p - fileName + 1) * sizeof(char))) {
		elog(WARNING, "internal error: protocol buf is too small for protocl \"%s\"",
				fileName);
		errno = ENOMEM;
		return false;
	}
	
	strncpy(buf, fileName, p - fileName);
	buf[p - fileName] = '\0';
	return true;
}

/*
 * get options from fileName, get rep number
 * liugd TODO : this function is VERY BAD
 */
static int
HdfsParseOptions(const char *fileName, short *rep)
{
	const char *pb = NULL;
	const char *p = NULL;
	const char *pe = NULL;

	p = strstr(fileName, fsys_protocol_sep);
	if (NULL == p) {
	    errno = EINVAL;
		elog(WARNING, "internal error HdfsParseOptions: no filesystem protocol found in path \"%s\"",
			 fileName);
		return -1;
	}
	p += strlen(fsys_protocol_sep);
	if (*p != '{') /* no options found, return */
		return 0;

	pb = p + 1;
	pe = strstr(pb, "}");
	if (NULL == pe) {
	    errno = EINVAL;
		elog(WARNING, "internal error HdfsParseOptions: options format error in path \"%s\"",
			 fileName);
		return -1;
	}

	if(strncmp(pb, "replica=", strlen("replica=")) == 0) {
		pb = pb + strlen("replica=");
		*rep = atoi(pb);
	}
	return 0;
}

/*
 * convert a hdfs well formated file path to unix file path
 * 		eg: http:/localhost:50070/example -> /example
 *
 * return buffer on success, return NULL on failure
 */
static const char *
ConvertToUnixPath(const char * fileName, char * buffer, int len) {
	char * p;
	p = strstr(fileName, fsys_protocol_sep);
	if (NULL == p) {
		elog(WARNING, "internal error: no filesystem protocol found in path \"%s\"",
				fileName);
		errno = EINVAL;
		return NULL;
	}
	p = strstr(p + strlen(fsys_protocol_sep), "/");
	if (NULL == p) {
		elog(WARNING, "internal error: cannot convert path \"%s\" into unix format",
				fileName);
		errno = EINVAL;
		return NULL;
	}
	strncpy(buffer, p, len);
	return buffer;
}

/*
 * open a hdfs file for read/write
 *
 * fileName: well formated hdfs file path,
 * 		hdfs file path schema hdfs://<host>:<port>/abspath
 */
static bool HdfsBasicOpenFile(FileName fileName, int fileFlags, int fileMode,
							  char **hProtocol, hdfsFS *fs, hdfsFile *hFile) {
	char path[MAXPGPATH + 1];
	char protocol[MAXPGPATH];
	short rep = FS_DEFAULT_REPLICA_NUM;

	DO_DB(elog(LOG, "HdfsBasicOpenFile, path: %s, fileFlags: %x, fileMode: %o",
	        fileName, fileFlags, fileMode));

	if (!HdfsGetProtocol(fileName, protocol, sizeof(protocol))) {
		elog(WARNING, "cannot get protocol for path: %s", fileName);
		return FALSE;
	}

	HdfsParseOptions(fileName, &rep);

	*hProtocol = strdup(protocol);
	*fs = HdfsGetConnection(protocol, fileName);
	if (*fs == NULL) {
		return FALSE;
	}

	if (NULL == ConvertToUnixPath(fileName, path, sizeof(path))) {
		return FALSE;
	}

    if (!(fileFlags & O_APPEND) && (fileFlags & O_WRONLY)) {
        *hFile = HdfsOpenFile(protocol, *fs, path, fileFlags, 0, rep, 0);
    } else {
        *hFile = HdfsOpenFile(protocol, *fs, path, fileFlags, 0, 0, 0);
    }

	//  do not check errno, checked in caller.
	if (*hFile != NULL) {
		if (O_CREAT & fileFlags) {
		    if (HdfsSync(protocol, *fs, *hFile)) {
		        elog(WARNING, "cannot reopen hdfs file errno %d", errno);
		        return FALSE;
		    }
			if (HdfsChmod(protocol, *fs, path, fileMode)) {
                elog(WARNING, "cannot set file : %s mode : %o, errno %d", fileName,
                        fileMode, errno);
                return FALSE;
            }
		}
		return TRUE; /* success! */
	}

	elog(WARNING, "hdfs error, cannot open %s, errno = %d", fileName, errno);
	return FALSE; /* failure */
}

/*
 * open a hdfs file.
 *
 * fileName: a well formated hdfs path
 */
File HdfsPathNameOpenFile(FileName fileName, int fileFlags, int fileMode) {
	File file;
	Vfd *vfdP;
	char * protocol;
	hdfsFS fs;
	hdfsFile hfile;
	char *pathname;

	DO_DB(elog(LOG, "HdfsPathNameOpenFile, path: %s, flag: %x, mode %o",
			   fileName, fileFlags, fileMode));
    /*
	 * We need a malloc'd copy of the file name; fail cleanly if no room.
	 */
	pathname = strdup(fileName);
	if (pathname == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));

	DO_DB(elog(LOG, "HdfsPathNameOpenFile: %s", fileNane));

	bool result = HdfsBasicOpenFile(pathname, fileFlags, fileMode, &protocol,
			&fs, &hfile);

	if (result == FALSE) {
		free(pathname);
		elog(WARNING, "HdfsPathNameOpenFile: fail %s, errno %d", fileName,
				errno);
		return -1;
	}

	/*
	 * create virtual file after open hdfs file,
	 * since open hdfs file may need to open metedata and create virtual file too,
	 * it may cause VfdCache realloc.
	 */
	file = AllocateVfd();
	vfdP = &VfdCache[file];

	vfdP->fileName = pathname;
	vfdP->hFS = fs;
	vfdP->hFile = hfile;
	vfdP->hProtocol = protocol;
	/*
	 * we always set O_APPEND since it will be ignored if
	 * the file is opened for read
	 */
	vfdP->fileFlags = (fileFlags & ~O_CREAT) | O_APPEND;
	vfdP->fileMode = fileMode;
	vfdP->seekPos = INT64CONST(0);
	vfdP->fdstate = 0x0;

	DO_DB(elog(LOG, "HdfsPathNameOpenFile: file: %d, success %s", file, fileName));

	return file;
}

/*
 * close a hdfs file
 */
void HdfsFileClose(File file, bool canReportError) {
	int retval = 0;
	Vfd *vfdP;

	char fileName[MAXPGPATH + 1];
	Assert(FileIsValid(file));

	vfdP = &VfdCache[file];

	DO_DB(elog(LOG, "HdfsFileClose: %d (%s)", file, vfdP->fileName));
	//elog(LOG, "HdfsFileClose: %d (%s)", file, vfdP->fileName);

	if (!FileIsNotOpen(file)) //file is open
	{
		//remove the file from the LRU ring
		//Delete(file);
		//close file

		//no matter the return code, remove vfd, file cannot be closed twice
		retval = HdfsCloseFile(vfdP->hProtocol, vfdP->hFS, vfdP->hFile);

		//used for log
		if (retval == -1) {
			strncpy(fileName, vfdP->fileName, MAXPGPATH);
		}

		//--nfile;
		vfdP->fd = VFD_CLOSED;
		vfdP->hFS = NULL;
		vfdP->hFile = NULL;
		if(vfdP->hProtocol)
		{
			free(vfdP->hProtocol);
			vfdP->hProtocol = NULL;
		}
	}

	//return the Vfd slot to the free list
	FreeVfd(file);

	if (retval == -1) {
		/* do not disconnect. */
		if (canReportError)
			elog(ERROR, "could not close file %d : (%s) errno %d", file,
			fileName, errno);
		else
			elog(WARNING, "could not close file %d : (%s) errno %d", file,
			fileName, errno);
	}
}

/*
 * read from hdfs file
 */
int HdfsFileRead(File file, char *buffer, int amount) {
	int returnCode;

	Assert(FileIsValid(file));
	DO_DB(elog(LOG, "HdfsFileRead: %d (%s) " INT64_FORMAT " %d %p",
					file, VfdCache[file].fileName,
					VfdCache[file].seekPos, amount, buffer));

	//ensure the file is open before FileAccess
    Vfd *vfdP;
    vfdP = &VfdCache[file];

	returnCode = FileAccess(file);
	if (returnCode < 0){
		elog(WARNING, "cannot reopen file %s for read, errno %d",
				vfdP->fileName, errno);
		return returnCode;
	}

	DO_DB(elog(LOG, "HdfsFileRead  para %p %p %p %d", VfdCache[file].hFS,
			VfdCache[file].hFile, buffer, amount));
	returnCode = HdfsRead(VfdCache[file].hProtocol, VfdCache[file].hFS, VfdCache[file].hFile,
			buffer, amount);
	DO_DB(elog(LOG, "HdfsFileRead  return %d, errno %d", returnCode, errno));
	if (returnCode >= 0)
		VfdCache[file].seekPos += returnCode;
	else {
		/* Trouble, so assume we don't know the file position anymore */
		VfdCache[file].seekPos = FileUnknownPos;
		elog(WARNING, "cannot read from file %s, errno %d",
		        VfdCache[file].fileName, errno);
	}

	return returnCode;
}

/*
 * write into hdfs file
 */
int HdfsFileWrite(File file, const char *buffer, int amount) {
	int returnCode;
	Vfd *vfdP;
	vfdP = &VfdCache[file];

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "HdfsFileWrite: %d (%s) " INT64_FORMAT " %d %p",
                            file,vfdP->fileName,
                            vfdP->seekPos, amount, buffer));

	returnCode = FileAccess(file);
	if (returnCode < 0){
	    elog(WARNING, "cannot reopen file %s for write, errno %d",
	    		vfdP->fileName, errno);
	    return returnCode;
	}

	DO_DB(elog(LOG, "HdfsFileWrite: file %d filename (%s)  amount%d buffer%p", file,
                            vfdP->fileName, amount, buffer));

	returnCode = HdfsWrite(vfdP->hProtocol, vfdP->hFS, vfdP->hFile, buffer, amount);

	if (returnCode >= 0) {
		vfdP->seekPos += returnCode;
	} else {
		/* Trouble, so assume we don't know the file position anymore */
	    elog(WARNING, "cannot write into file %s, errno %d",
	    		vfdP->fileName, errno);
	    vfdP->seekPos = FileUnknownPos;
	}
	return returnCode;
}


/*
 * tell the position of file point
 *
 * NB: file should be already opened
 */
int64 HdfsFileTell(File file) {
	int returnCode;
	Assert(FileIsValid(file));
	DO_DB(elog(LOG, "HfdsFileTell, file %s", VfdCache[file].fileName));
	returnCode = FileAccess(file);
	if (returnCode < 0){
	    elog(WARNING, "cannot reopen file %s for file tell, errno %d",
	            VfdCache[file].fileName, errno);
		return returnCode;
	}
	return (int64) HdfsTell(VfdCache[file].hProtocol, VfdCache[file].hFS, VfdCache[file].hFile);
}

/*
 * seek file point to given position
 *
 * NB: only hdfs file that is opend for read can be seek
 */
int64 HdfsFileSeek(File file, int64 offset, int whence) {
	int returnCode;
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "HdfsFileSeek: %d (%s) " INT64_FORMAT " " INT64_FORMAT " %d",
					file, VfdCache[file].fileName,
					VfdCache[file].seekPos, offset, whence));

	int64 desiredPos = 0;
	if (VfdCache[file].seekPos != FileUnknownPos) {
		desiredPos = VfdCache[file].seekPos;
	}

	switch (whence) {
		case SEEK_SET:
			Assert(offset >= INT64CONST(0));
			desiredPos = offset;
			break;
		case SEEK_CUR:
			desiredPos += offset;
			break;
		case SEEK_END:
		{
			HdfsFileInfo	*info;
			char path[MAXPGPATH + 1];

			ConvertToUnixPath(VfdCache[file].fileName, path, sizeof(path));

			info = HdfsGetPathInfo(VfdCache[file].hProtocol, VfdCache[file].hFS, path);
			// liugd: do we need to return false instead of elog ERROR?
			if (!info)
				elog(ERROR, "hdfsGetPathInfo: failed");

			desiredPos = info->mSize;
			HdfsFreeFileInfo(VfdCache[file].hProtocol, info, 1);
			break;
		}
		default:
			Assert(!"invalid whence");
			break;
	}

	returnCode = FileAccess(file);
	if (returnCode < 0){
	    elog(WARNING, "cannot open file: %s for seek, errno %d",
	            VfdCache[file].fileName, errno);
		return returnCode;
	}
	if (0 != HdfsSeek(VfdCache[file].hProtocol, VfdCache[file].hFS, VfdCache[file].hFile,
					desiredPos)) {
	    elog(WARNING, "cannot to seek file %s to "INT64_FORMAT" errno %d",
	            VfdCache[file].fileName, desiredPos, errno);
		VfdCache[file].seekPos = -1;
	} else {
		VfdCache[file].seekPos = desiredPos;
	}

	return VfdCache[file].seekPos;
}

/*
 * flush hdfs file
 *
 * NB: hdfs flush do NOT promise that data has been writen on disk
 * after flush, data can be read by others
 */
int HdfsFileSync(File file) {
	Assert(FileIsValid(file));
	Assert(!FileIsNotOpen(file));
	DO_DB(elog(LOG, "HdfsFileSync: %d (%s)", file, VfdCache[file].fileName));

	if(HdfsSync(VfdCache[file].hProtocol, VfdCache[file].hFS, VfdCache[file].hFile)) {
	    elog(WARNING, "cannot flush hdfs file %s://%s, errno %d",
			 VfdCache[file].hProtocol, VfdCache[file].fileName, errno);
	    return -1;
	}
	return 0;
}

/*
 * remove a hdfs path
 *
 * fileName : a well formated hdfs path
 *
 * return 0 on success, non-zero on failure
 */
int HdfsRemovePath(FileName fileName, int recursive) {
	char path[MAXPGPATH + 1];
	char protocol[MAXPGPATH];

	DO_DB(elog(LOG, "HdfsRemovePath, path: %s, recursive: %d", fileName, recursive));

	if (!HdfsGetProtocol(fileName, protocol, sizeof(protocol))) {
		elog(WARNING, "cannot get protocol for path: %s", fileName);
		return -1;
	}

	hdfsFS fs = HdfsGetConnection(protocol, fileName);
	if (NULL == fs) {
		return -1;
	}
	if (NULL == ConvertToUnixPath(fileName, path, sizeof(path))) {
		return -1;
	}

	if(HdfsDelete(protocol, fs, path, recursive)) {
	    elog(WARNING, "cannot delete path: %s, recursive: %d, errno %d",
	            fileName, recursive, errno);
	    return -1;
	}
	return 0;
}

int HdfsMakeDirectory(const char * path, mode_t mode) {
	char p[MAXPGPATH + 1];
	char protocol[MAXPGPATH];

	DO_DB(elog(LOG, "HdfsMakeDirectory: %s, mode: %o", path, mode));

	if (!HdfsGetProtocol(path, protocol, sizeof(protocol))) {
		elog(WARNING, "cannot get protocol for path: %s", path);
		return -1;
	}

	hdfsFS fs = HdfsGetConnection(protocol, path);
	if (NULL == fs) {
		return -1;
	}
	if (NULL == ConvertToUnixPath(path, p, sizeof(p))) {
		return -1;
	}

	if (0 == HdfsCreateDirectory(protocol, fs, p)) {
		if(HdfsChmod(protocol, fs, p, mode)) {
		    elog(WARNING, "cannot set path %s, mode %o, errno %d", path, mode, errno);
		    return -1;
		}
		return 0;
	}
	return -1;
}

/*
 * truncate a hdfs file to a defined length
 */
int HdfsFileTruncate(File file, int64 offset) {
	Vfd *vfdP;
	char protocol [MAXPGPATH + 1];
	hdfsFS fs;
	char p[MAXPGPATH + 1];

	Assert(FileIsValid(file));

	vfdP = &VfdCache[file];

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "HdfsFileTruncate %d (%s)",
					file, VfdCache[file].fileName));

	strncpy(protocol, vfdP->hProtocol, MAXPGPATH);

	fs = vfdP->hFS;

	/*
	 * HDFS files not in LRU list, so no need to call LruDelete
	 * but we need to close HDFS file first
	 */
	if (!FileIsNotOpen(file)) //file is open
	{
		//LruDelete(file);
		Vfd		   *vfdP;

		DO_DB(elog(LOG, "HdfsFileTruncate: close file %d (%s)",
				   file, VfdCache[file].fileName));

		vfdP = &VfdCache[file];

		/* close the file */
		if (HdfsCloseFile(VfdCache[file].hProtocol, VfdCache[file].hFS, VfdCache[file].hFile))
		{
			elog(WARNING, "could not close file \"%s\": %m", vfdP->fileName);
			return -1;
		}

		//--nfile;
		vfdP->fd = VFD_CLOSED;
		vfdP->hFS = NULL;
		vfdP->hFile = NULL;
		if(vfdP->hProtocol)
		{
			free(vfdP->hProtocol);
			vfdP->hProtocol = NULL;
		}
	}

	if (NULL == ConvertToUnixPath(vfdP->fileName, p, sizeof(p))) {
		return -1;
	}

	if (0 != HdfsTruncate(protocol, fs, p, offset))
	    return -1;

	/*
	 * reopen file after truncate
	 */

	Insist((vfdP->fileFlags & O_WRONLY) && (vfdP->fileFlags & O_APPEND));
	if (FALSE == HdfsBasicOpenFile(vfdP->fileName, vfdP->fileFlags, vfdP->fileMode,
	        &vfdP->hProtocol, &vfdP->hFS, &vfdP->hFile))
	    return -1;

	/*
	 * check logic position.
	 * since we use hdfsTruncate to implement fHdfsTruncate,
	 * and close file, truncate and reopen file is not atomic.
	 * others may append data after truncate but before reopen.
	 * we just simply check the file length after reopen,
	 * we assume that there is no concurrent appending and truncating.
	 */
	vfdP->seekPos = (int64) HdfsTell(vfdP->hProtocol, vfdP->hFS, vfdP->hFile);
	if (offset != vfdP->seekPos)
	{
		errno = EIO;
		return -1;
	}

	return 0;
}

File PathNameOpenFile(FileName fileName, int fileFlags, int fileMode) {
	if (IsLocalPath(fileName))
		return LocalPathNameOpenFile(fileName, fileFlags, fileMode);
	else
		return HdfsPathNameOpenFile(fileName, fileFlags, fileMode);
}

void FileClose(File file) {
	if (IsLocalFile(file))
		LocalFileClose(file);
	else
		HdfsFileClose(file, true);
}

int FileRead(File file, char *buffer, int amount) {
	if (IsLocalFile(file))
		return LocalFileRead(file, buffer, amount);
	else
		return HdfsFileRead(file, buffer, amount);
}

int FileWrite(File file, const char *buffer, int amount) {
	if (IsLocalFile(file))
		return LocalFileWrite(file, buffer, amount);
	else
		return HdfsFileWrite(file, buffer, amount);
}

/*
 * seek file point to given position
 *
 * return the position of file point after seek
 */
int64 FileSeek(File file, int64 offset, int whence) {
	if (IsLocalFile(file))
		return LocalFileSeek(file, offset, whence);
	else
		return HdfsFileSeek(file, offset, whence);
}

/*
 * sync file
 *
 * return 0 on success, non-zero on failure
 */
int FileSync(File file) {
	if (IsLocalFile(file))
		return LocalFileSync(file);
	else
		return HdfsFileSync(file);
}

/*
 * remove a path
 *
 * return 0 on failure, non-zero on success
 */
int RemovePath(FileName fileName, int recursive) {
	if (IsLocalPath(fileName))
		return LocalRemovePath(fileName, recursive);
	else
		return !HdfsRemovePath(fileName, recursive);
}

int FileTruncate(File file, int64 offset) {
	if (IsLocalFile(file))
		return LocalFileTruncate(file, offset);
	else
		return HdfsFileTruncate(file, offset);
}

/*
 * make a directory on given file system
 *
 * return o on success, non-zero on failure
 */
int MakeDirectory(const char * path, mode_t mode) {
	if (IsLocalPath(path))
		return mkdir(path, mode);
	else
		return HdfsMakeDirectory(path, mode);
}

bool
TestFileValid(File file)
{
    return FileIsValid(file);
}

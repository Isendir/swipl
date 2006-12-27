/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        wielemak@science.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2006, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define O_DEBUG 1

#define _GNU_SOURCE 1			/* get recursive mutex stuff to */
					/* compile clean with glibc.  Can */
					/* this do any harm? */
#include "pl-incl.h"
#include <stdio.h>
#ifdef O_PLMT

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
			   MULTITHREADING SUPPORT

APPROACH
========

    * Prolog threads are C-threads
      Prolog multi-threading is based upon a C thread library.  At first,
      we will concentrate on the Posix pthread standard, due to its wide
      availability especially on Unix systems.

      This approach has some clear advantages: clean mixing of thread-based
      foreign code and portability.

    * Data
      All global data that cannot be removed is split into three large
      structures:

	+ PL_code_data
	  This structure contains `code' data: data that is set up at
	  initialisation time and never changed afterwards.
	  PL_initialise() initialises this and no further precautions
	  are needed.

	+ PL_global_data
	  This structure contains all global data required for the
	  Prolog `heap'.  This data is shared between threads and
	  access should be properly synchronised.

	+ PL_local_data
	  This structure contains the thread-local data.  If a new
	  Prolog engine is initialised in a thread, a new copy of this
	  structure is allocated and initialised.

	  For compatibility reasons, we cannot pass this pointer around
	  as an argument between all functions in the system.  We will
	  locate it through the thread-id using a function.  Any function
	  requiring frequent access can fetch this pointer once at
	  start-up.  Cooperating functions can pass this pointer.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <errno.h>
#if defined(__linux__) && defined(HAVE_GETTID)
#include <linux/unistd.h>
_syscall0(pid_t,gettid)
#endif

#ifdef HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif

#ifdef HAVE_SEMA_INIT			/* Solaris */
#include <synch.h>

typedef sema_t sem_t;
#define sem_trywait(s)	sema_trywait(s)
#define sem_destroy(s)	sema_destroy(s)
#define sem_post(s)	sema_post(s)
#define sem_init(s, type, cnt) sema_init(s, cnt, type, NULL)

#else /*HAVE_SEMA_INIT*/
#include <semaphore.h>

#ifndef USYNC_THREAD
#define USYNC_THREAD 0
#endif

#endif /*HAVE_SEMA_INIT*/

#ifdef USE_SEM_OPEN			/* see below */
static sem_t *sem_canceled_ptr;
#else
static sem_t sem_canceled;		/* used on halt */
#define sem_canceled_ptr (&sem_canceled)
#endif

#ifndef WIN32
#include <signal.h>

#ifdef USE_SEM_OPEN

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Apple Darwin (6.6) only contains the sem_init()   function as a stub. It
only provides named semaphores  through   sem_open().  These defines and
my_sem_open() try to hide the details of   this as much as possible from
the rest of the code. Note  that   we  unlink  the semaphore right after
creating it, using the common Unix trick to keep access to it as long as
we do not close it. We assume  the   OS  will close the semaphore as the
application terminates. All this is highly   undesirable, but it will do
for now. The USE_SEM_OPEN define  is  set   by  configure  based  on the
substring "darwin" in the architecture identifier.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static sem_t *sem_mark_ptr;

#define sem_init(ptr, flags, val) my_sem_open(&ptr, val)
#define sem_destroy(ptr)	  ((void)0)

static int
my_sem_open(sem_t **ptr, unsigned int val)
{ if ( !*ptr )
  { sem_t *sem = sem_open("pl", O_CREAT|O_EXCL, 0600, val);

    DEBUG(1, Sdprintf("sem = %p\n", sem));

    if ( sem == NULL )
    { perror("sem_open");
      exit(1);
    }

    *ptr = sem;

    sem_unlink("pl");
  }

  return 0;
}

#else /*USE_SEM_OPEN*/

static sem_t sem_mark;			/* used for atom-gc */
#define sem_mark_ptr (&sem_mark)

#endif /*USE_SEM_OPEN*/

#ifndef SA_RESTART
#define SA_RESTART 0
#endif

#if defined(__APPLE__)
#define SIG_FORALL SIGUSR1
#else
#define SIG_FORALL SIGHUP
#endif
#define SIG_RESUME SIG_FORALL
#endif /*!WIN32*/


		 /*******************************
		 *	    GLOBAL DATA		*
		 *******************************/

static Table threadTable;		/* name --> integer-id */
static PL_thread_info_t threads[MAX_THREADS];
static int threads_ready = FALSE;	/* Prolog threads available */
static Table queueTable;		/* name --> queue */
static int queue_id;			/* next generated id */

TLD_KEY PL_ldata;			/* key for thread PL_local_data */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The global mutexes. Most are using  within   a  module and their name is
simply the module-name. The idea is that   a module holds a coherent bit
of data that needs a mutex for all operations.

Some remarks:

    L_MISC
	General-purpose mutex.  Should only be used for simple, very
	local tasks and may not be used to lock anything significant.

    WIN32
	We use native windows CRITICAL_SECTIONS for mutexes here to
	get the best performance, notably on single-processor hardware.
	This is selected in pl-mutex.h based on the macro
	USE_CRITICAL_SECTIONS

	Unfortunately critical sections have no static initialiser,
	so we need something called before anything else happens.  This
	can only be DllMain().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef USE_CRITICAL_SECTIONS
#undef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER {0}
#endif

#define COUNT_MUTEX_INITIALIZER(name) \
 { PTHREAD_MUTEX_INITIALIZER, \
   name, \
   0L \
 }

counting_mutex _PL_mutexes[] =
{ COUNT_MUTEX_INITIALIZER("L_MISC"),
  COUNT_MUTEX_INITIALIZER("L_ALLOC"),
  COUNT_MUTEX_INITIALIZER("L_ATOM"),
  COUNT_MUTEX_INITIALIZER("L_FLAG"),
  COUNT_MUTEX_INITIALIZER("L_FUNCTOR"),
  COUNT_MUTEX_INITIALIZER("L_RECORD"),
  COUNT_MUTEX_INITIALIZER("L_THREAD"),
  COUNT_MUTEX_INITIALIZER("L_PREDICATE"),
  COUNT_MUTEX_INITIALIZER("L_MODULE"),
  COUNT_MUTEX_INITIALIZER("L_TABLE"),
  COUNT_MUTEX_INITIALIZER("L_BREAK"),
  COUNT_MUTEX_INITIALIZER("L_FILE"),
  COUNT_MUTEX_INITIALIZER("L_FEATURE"),
  COUNT_MUTEX_INITIALIZER("L_OP"),
  COUNT_MUTEX_INITIALIZER("L_INIT"),
  COUNT_MUTEX_INITIALIZER("L_TERM"),
  COUNT_MUTEX_INITIALIZER("L_GC"),
  COUNT_MUTEX_INITIALIZER("L_FOREIGN"),
  COUNT_MUTEX_INITIALIZER("L_OS")
};


static void
link_mutexes()
{ counting_mutex *m;
  int n = sizeof(_PL_mutexes)/sizeof(*m);
  int i;

  GD->thread.mutexes = _PL_mutexes;
  for(i=0, m=_PL_mutexes; i<n-1; i++, m++)
    m->next = m+1;
}


#ifdef USE_CRITICAL_SECTIONS

static void
initMutexes()
{ counting_mutex *m;
  int n = sizeof(_PL_mutexes)/sizeof(*m);
  int i;

  for(i=0, m=_PL_mutexes; i<n; i++, m++)
    simpleMutexInit(&m->mutex);
}

static void
deleteMutexes()
{ counting_mutex *m;
  int n = sizeof(_PL_mutexes)/sizeof(*m);
  int i;

  for(i=0, m=_PL_mutexes; i<n; i++, m++)
    simpleMutexDelete(&m->mutex);
}


BOOL WINAPI
DllMain(HINSTANCE hinstDll, DWORD fdwReason, LPVOID lpvReserved)
{ BOOL result = TRUE;

  switch(fdwReason)
  { case DLL_PROCESS_ATTACH:
      GD->thread.instance = hinstDll;
      initMutexes();
      break;
    case DLL_PROCESS_DETACH:
      deleteMutexes();
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
  }

  return result;
}

#endif /*USE_CRITICAL_SECTIONS*/

static
PRED_IMPL("mutex_statistics", 0, mutex_statistics, 0)
{ counting_mutex *cm;

#ifdef O_CONTENTION_STATISTICS
  Sdprintf("Name                               locked collisions\n"
	   "----------------------------------------------------\n");
#else
  Sdprintf("Name                               locked\n"
	   "-----------------------------------------\n");
#endif
  PL_LOCK(L_THREAD);
  for(cm = GD->thread.mutexes; cm; cm = cm->next)
  { if ( cm->count == 0 )
      continue;

    Sdprintf("%-32Us %8d", cm->name, cm->count); /* %Us: UTF-8 string */
#ifdef O_CONTENTION_STATISTICS
    Sdprintf(" %8d", cm->collisions);
#endif
    if ( cm == &_PL_mutexes[L_THREAD] )
    { if ( cm->count - cm->unlocked != 1 )
	Sdprintf(" LOCKS: %d\n", cm->count - cm->unlocked - 1);
      else
	Sdprintf("\n");
    } else
    { if ( cm->unlocked != cm->count )
	Sdprintf(" LOCKS: %d\n", cm->count - cm->unlocked);
      else
	Sdprintf("\n");
    }
  }
  PL_UNLOCK(L_THREAD);

  succeed;
}


#if defined(PTHREAD_CPUCLOCKS) || defined(LINUX_CPU_CLOCKS)
#define ThreadCPUTime(info, which)	ThreadCPUTime(info)
#endif

		 /*******************************
		 *	  LOCAL PROTOTYPES	*
		 *******************************/

static PL_thread_info_t *alloc_thread(void);
static void	destroy_message_queue(message_queue *queue);
static void	init_message_queue(message_queue *queue);
static void	freeThreadSignals(PL_local_data_t *ld);
static void	unaliasThread(atom_t name);
static void	run_thread_exit_hooks();
static void	free_thread_info(PL_thread_info_t *info);
static void	set_system_thread_id(PL_thread_info_t *info);
static int	get_message_queue(term_t t, message_queue **queue);
static void	cleanupLocalDefinitions(PL_local_data_t *ld);
static pl_mutex *mutexCreate(atom_t name);
static double   ThreadCPUTime(PL_thread_info_t *info, int which);


		 /*******************************
		 *	       ERRORS		*
		 *******************************/

static char *
ThError(int e)
{ return strerror(e);
}


		 /*******************************
		 *	LOCK ON L_THREAD	*
		 *******************************/

#define LOCK()   PL_LOCK(L_THREAD)
#define UNLOCK() PL_UNLOCK(L_THREAD)


		 /*******************************
		 *     RUNTIME ENABLE/DISABLE	*
		 *******************************/

int
enableThreads(int enable)
{ if ( enable )
  { GD->thread.enabled = TRUE;		/* print system message? */
  } else
  { LOCK();
    if ( GD->statistics.threads_created -
	 GD->statistics.threads_finished == 1 ) /* I am alone :-( */
    { GD->thread.enabled = FALSE;
    } else
    { term_t key = PL_new_term_ref();

      PL_put_atom(key, ATOM_threads);

      UNLOCK();
      return PL_error(NULL, 0, "Active threads",
		      ERR_PERMISSION,
		      ATOM_modify, ATOM_flag, key);
    }
    UNLOCK();
  }

  succeed;
}


		 /*******************************
		 *	 THREAD ALLOCATION	*
		 *******************************/

static int
initialise_thread(PL_thread_info_t *info)
{ assert(info->thread_data);

  LOCK();
  GD->statistics.threads_created++;
  UNLOCK();

  TLD_set(PL_ldata, info->thread_data);

  if ( !info->local_size    ) info->local_size    = GD->options.localSize;
  if ( !info->global_size   ) info->global_size   = GD->options.globalSize;
  if ( !info->trail_size    ) info->trail_size    = GD->options.trailSize;
  if ( !info->argument_size ) info->argument_size = GD->options.argumentSize;

  if ( !initPrologStacks(info->local_size,
			 info->global_size,
			 info->trail_size,
			 info->argument_size) )
  { PL_local_data_t *ld = info->thread_data;

    memset(&ld->stacks, 0, sizeof(ld->stacks));

    fail;
  }

  initPrologLocalData();
  info->thread_data->magic = LD_MAGIC;

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
free_prolog_thread()
    Called from a cleanup-handler to release all resources associated
    with a thread.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
free_prolog_thread(void *data)
{ PL_local_data_t *ld = data;
  PL_thread_info_t *info;
  int acknowlege;
  double time;

  if ( !threads_ready )
    return;				/* Post-mortem */

  info = ld->thread.info;

  LOCK();
  if ( info->status == PL_THREAD_RUNNING )
    info->status = PL_THREAD_EXITED;	/* foreign pthread_exit() */
  acknowlege = (info->status == PL_THREAD_CANCELED);
  UNLOCK();
  DEBUG(1, Sdprintf("Freeing prolog thread %d\n", info-threads));

#if O_DEBUGGER
  callEventHook(PL_EV_THREADFINISHED, info);
#endif
  run_thread_exit_hooks();
  cleanupLocalDefinitions(ld);
  
  DEBUG(2, Sdprintf("Destroying data\n"));
  ld->magic = 0;
  if ( ld->stacks.global.base )		/* otherwise assume they are not */
    freeStacks(ld);			/* initialised */
  freeLocalData(ld);

  if ( ld->feature.table )
    destroyHTable(ld->feature.table);
  /*PL_unregister_atom(ld->prompt.current);*/

  freeThreadSignals(ld);
  time = ThreadCPUTime(info, CPU_USER);

  LOCK();
  destroy_message_queue(&ld->thread.messages);
  GD->statistics.threads_finished++;
  GD->statistics.thread_cputime += time;

  info->thread_data = NULL;
  ld->thread.info = NULL;		/* avoid a loop */
  UNLOCK();

  if ( info->detached )
    free_thread_info(info);

  mergeAllocPool(&GD->alloc_pool, &ld->alloc_pool);
  freeHeap(ld, sizeof(*ld));

  if ( acknowlege )
    sem_post(sem_canceled_ptr);
}


void
initPrologThreads()
{ PL_thread_info_t *info;
  static int init_ldata_key = FALSE;

  LOCK();
  if ( threads_ready )
  { UNLOCK();
    return;
  }

  if ( !init_ldata_key )
  { TLD_alloc(&PL_ldata);		/* see also alloc_thread() */
    init_ldata_key = TRUE;
  }
  TLD_set(PL_ldata, &PL_local_data);
  PL_local_data.magic = LD_MAGIC;
  info = &threads[1];
  info->tid = pthread_self();
  info->pl_tid = 1;
  info->thread_data = &PL_local_data;
  info->status = PL_THREAD_RUNNING;
  PL_local_data.thread.info = info;
  PL_local_data.thread.magic = PL_THREAD_MAGIC;
#ifdef WIN32
  info->w32id = GetCurrentThreadId();
#endif
  set_system_thread_id(info);
  init_message_queue(&PL_local_data.thread.messages);

  GD->statistics.thread_cputime = 0.0;
  GD->statistics.threads_created = 1;
  GD->thread.mutexTable = newHTable(16);
  GD->thread.MUTEX_load = mutexCreate(ATOM_dload);
  link_mutexes();
  threads_ready = TRUE;
  UNLOCK();
}


void
cleanupThreads()
{ /*TLD_free(PL_ldata);*/		/* this causes crashes */
  threadTable = NULL;
  queueTable = NULL;
  memset(&threads, 0, sizeof(threads));
  threads_ready = FALSE;
  queue_id = 0;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A first step towards clean destruction of the system.  Ideally, we would
like the following to happen:

    * Close-down all threads except for the main one
	+ Have all thread_at_exit/1 hooks called
    * Run the at_halt/1 hooks in the main thread
    * Exit from the main thread.

There are a lot of problems however.

    * Somehow Halt() should always be called from the main thread
      to have the process working properly.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
exitPrologThreads()
{ PL_thread_info_t *t;
  int i;
  int me = PL_thread_self();
  int canceled = 0;

  DEBUG(1, Sdprintf("exitPrologThreads(): me = %d\n", me));

  sem_init(sem_canceled_ptr, USYNC_THREAD, 0);

  for(t=&threads[1], i=1; i<MAX_THREADS; i++, t++)
  { if ( t->thread_data && i != me )
    { switch(t->status)
      { case PL_THREAD_FAILED:
	case PL_THREAD_EXITED:
	case PL_THREAD_EXCEPTION:
	{ void *r;
	  int rc;

	  if ( (rc=pthread_join(t->tid, &r)) )
	    Sdprintf("Failed to join thread %d: %s\n", i, ThError(rc));

	  break;
	}
	case PL_THREAD_RUNNING:
	{ t->thread_data->exit_requested = TRUE;

	  if ( t->cancel )
	  { if ( (*t->cancel)(i) == TRUE )
	      break;			/* done so */
	  }

#ifdef WIN32
	  raiseSignal(t->thread_data, SIGINT);
	  PostThreadMessage(t->w32id, WM_QUIT, 0, 0);
	  DEBUG(1, Sdprintf("Cancelled %d\n", i));
	  canceled++;
#else
  	  if ( t->tid )
	  { int rc;
	    
	    if ( (rc=pthread_cancel(t->tid)) == 0 )
	    { t->status = PL_THREAD_CANCELED;
	      canceled++;
	    } else
	    { Sdprintf("Failed to cancel thread %d: %s\n", i, ThError(rc));
	    }
	  } else
	  { DEBUG(1, Sdprintf("Destroying engine %d\n", i));
	    PL_destroy_engine(t->thread_data);
	  }
#endif
	  break;
	}
      }
    }
  }

  DEBUG(1, Sdprintf("Waiting for %d threads ...", canceled));
  for(i=canceled; i-- > 0;)
  { int maxwait = 10;

    while(maxwait--)
    { if ( sem_trywait(sem_canceled_ptr) == 0 )
      { DEBUG(1, Sdprintf(" (ok)"));
	canceled--;
	break;
      }
      Pause(0.1);
    }
  }
  if ( canceled )
  { printMessage(ATOM_informational,
		 PL_FUNCTOR_CHARS, "threads_not_died", 1,
		   PL_INT, canceled);
  } else
  { DEBUG(1, Sdprintf("done\n"));
  }

  if ( canceled == 0 )			/* safe */
    sem_destroy(sem_canceled_ptr);

  threads_ready = FALSE;
}


		 /*******************************
		 *	    ALIAS NAME		*
		 *******************************/

bool
aliasThread(int tid, atom_t name)
{ LOCK();
  if ( !threadTable )
    threadTable = newHTable(16);

  if ( (threadTable && lookupHTable(threadTable, (void *)name)) ||
       (queueTable  && lookupHTable(queueTable,  (void *)name)) )
  { term_t obj = PL_new_term_ref();

    UNLOCK();
    PL_put_atom(obj, name);
    return PL_error("thread_create", 1, "Alias name already taken",
		    ERR_PERMISSION, ATOM_thread, ATOM_create, obj);
  }

  addHTable(threadTable, (void *)name, (void *)(long)tid);
  PL_register_atom(name);
  threads[tid].name = name;

  UNLOCK();

  succeed;
}

static void
unaliasThread(atom_t name)
{ if ( threadTable )
  { Symbol s;

    LOCK();
    if ( (s = lookupHTable(threadTable, (void *)name)) )
    { PL_unregister_atom(name);
      deleteSymbolHTable(threadTable, s);
    }
    UNLOCK();
  }
}

		 /*******************************
		 *	 PROLOG BINDING		*
		 *******************************/

static PL_thread_info_t *
alloc_thread()
{ int i;

  for(i=1; i<MAX_THREADS; i++)
  { if ( threads[i].status == PL_THREAD_UNUSED )
    { PL_local_data_t *ld = allocHeap(sizeof(PL_local_data_t));

      memset(ld, 0, sizeof(PL_local_data_t));

      threads[i].pl_tid = i;
      threads[i].thread_data = ld;
      threads[i].status = PL_THREAD_CREATED;
      ld->thread.info = &threads[i];
      ld->thread.magic = PL_THREAD_MAGIC;

      return &threads[i];
    }
  }

  return NULL;				/* out of threads */
}


int
PL_thread_self()
{ PL_local_data_t *ld = LD;

  if ( ld )
    return ld->thread.info->pl_tid;

  return -1;				/* thread has no Prolog thread */
}


#ifdef WIN32

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
PL_w32thread_raise(DWORD id, int sig)
    Sets the signalled mask for a specific Win32 thread. This is a
    partial work-around for the lack of proper asynchronous signal
    handling in the Win32 platform.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
PL_w32thread_raise(DWORD id, int sig)
{ PL_thread_info_t *info;
  int i;

  if ( sig < 0 || sig > MAXSIGNAL )
    return FALSE;			/* illegal signal */

  LOCK();
  for(i = 0, info = threads; i < MAX_THREADS; i++, info++)
  { if ( info->w32id == id && info->thread_data )
    { raiseSignal(info->thread_data, sig);
#ifdef WIN32
      if ( info->w32id )
	PostThreadMessage(info->w32id, WM_SIGNALLED, 0, 0L);
#endif
      UNLOCK();
      DEBUG(1, Sdprintf("Signalled %d to thread %d\n", sig, i));
      return TRUE;
    }
  }
  UNLOCK();

  return FALSE;				/* can't find thread */
}

#endif /*WIN32*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
PL_thread_raise() is used  for  re-routing   interrupts  in  the Windows
version, where the signal handler is running  from a different thread as
Prolog.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
PL_thread_raise(int tid, int sig)
{ PL_thread_info_t *info;

  LOCK();
  if ( tid < 0 || tid >= MAX_THREADS )
  { error:
    UNLOCK();
    return FALSE;
  }
  info = &threads[tid];
  if ( info->status == PL_THREAD_UNUSED )
    goto error;

  if ( !raiseSignal(info->thread_data, sig) )
    goto error;
#ifdef WIN32
  if ( info->w32id )
    PostThreadMessage(info->w32id, WM_SIGNALLED, 0, 0L);
#endif
  UNLOCK();

  return TRUE;
}


const char *
threadName(int id)
{ PL_thread_info_t *info;
  char tmp[16];
    
  if ( id == 0 )
    id = PL_thread_self();
  if ( id < 0 )
    return "[Not a prolog thread]";

  info = &threads[id];

  if ( info->name )
    return PL_atom_chars(info->name);

  sprintf(tmp, "%d", id);
  return buffer_string(tmp, BUF_RING);
}


long
system_thread_id(PL_thread_info_t *info)
{ if ( !info )
  { if ( LD )
      info = LD->thread.info;
    else
      return -1;
  }
#ifdef __linux__
  return info->pid;
#else
#ifdef WIN32
  return info->w32id;
#else
  return (long)info->tid;
#endif
#endif
}

static void
set_system_thread_id(PL_thread_info_t *info)
{ 
#ifdef HAVE_GETTID
  info->pid = gettid();
#else
#ifdef WIN32
  info->w32id = GetCurrentThreadId();
#endif
#endif
}


static const opt_spec make_thread_options[] = 
{ { ATOM_local,		OPT_LONG|OPT_INF },
  { ATOM_global,	OPT_LONG|OPT_INF },
  { ATOM_trail,	        OPT_LONG|OPT_INF },
  { ATOM_argument,	OPT_LONG|OPT_INF },
  { ATOM_alias,		OPT_ATOM },
  { ATOM_detached,	OPT_BOOL },
  { ATOM_stack,		OPT_LONG },
  { NULL_ATOM,		0 }
};


static void *
start_thread(void *closure)
{ PL_thread_info_t *info = closure;
  term_t ex, goal;
  int rval;

  blockSignal(SIGINT);			/* only the main thread processes */
					/* Control-C */
  set_system_thread_id(info);		/* early to get exit code ok */

  if ( !initialise_thread(info) )
  { info->status = PL_THREAD_NOMEM;
    return (void *)TRUE;
  }
    
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

  pthread_cleanup_push(free_prolog_thread, info->thread_data);

  LOCK();
  info->status = PL_THREAD_RUNNING;
  UNLOCK();

  goal = PL_new_term_ref();
  PL_put_atom(goal, ATOM_dthread_init);

  rval = callProlog(MODULE_system, goal, PL_Q_CATCH_EXCEPTION, &ex);

  if ( rval )
  { PL_recorded(info->goal, goal);
    rval  = callProlog(info->module, goal, PL_Q_CATCH_EXCEPTION, &ex);
  }

  if ( !rval && info->detached )
  { if ( ex )
    { printMessage(ATOM_warning,
		   PL_FUNCTOR_CHARS, "abnormal_thread_completion", 2,
		     PL_TERM, goal,
		     PL_FUNCTOR, FUNCTOR_exception1,
		       PL_TERM, ex);
    } else
    { printMessage(ATOM_warning,
		   PL_FUNCTOR_CHARS, "abnormal_thread_completion", 2,
		     PL_TERM, goal,
		     PL_ATOM, ATOM_fail);
    } 
  }

  LOCK();
  if ( rval )
  { info->status = PL_THREAD_SUCCEEDED;
  } else
  { if ( ex )
    { info->status = PL_THREAD_EXCEPTION;
      info->return_value = PL_record(ex);
    } else
    { info->status = PL_THREAD_FAILED;
    }
  }    
  UNLOCK();

  pthread_cleanup_pop(1);

  return (void *)TRUE;
}


word
pl_thread_create(term_t goal, term_t id, term_t options)
{ PL_thread_info_t *info;
  PL_local_data_t *ldnew;
  atom_t alias = NULL_ATOM;
  pthread_attr_t attr;
  long stack = 0;
  int rc;

  if ( !(PL_is_compound(goal) || PL_is_atom(goal)) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_callable, goal);

  LOCK();

  if ( !GD->thread.enabled )
  { UNLOCK();
    return PL_error(NULL, 0, "threading disabled",
		      ERR_PERMISSION,
		      ATOM_create, ATOM_thread, goal);
  }

  info = alloc_thread();
  UNLOCK();
  if ( !info )
    return PL_error(NULL, 0, NULL, ERR_RESOURCE, ATOM_threads);

  ldnew = info->thread_data;

  if ( !scan_options(options, 0, /*OPT_ALL,*/
		     ATOM_thread_option, make_thread_options,
		     &info->local_size,
		     &info->global_size,
		     &info->trail_size,
		     &info->argument_size,
		     &alias,
		     &info->detached,
		     &stack) )
  { free_thread_info(info);
    fail;
  }

#define MK_KBYTES(v) if ( v < (LONG_MAX/1024) ) v *= 1024

  MK_KBYTES(info->local_size);
  MK_KBYTES(info->global_size);
  MK_KBYTES(info->trail_size);
  MK_KBYTES(info->argument_size);
  MK_KBYTES(stack);

  info->goal = PL_record(goal);
  info->module = PL_context();

  if ( alias )
  { if ( !aliasThread(info->pl_tid, alias) )
    { free_thread_info(info);
      fail;
    }
  }
					/* copy settings */

  PL_register_atom(LD->prompt.current);
  ldnew->prompt			 = LD->prompt;
  if ( LD->prompt.first )
  { ldnew->prompt.first		 = LD->prompt.first;
    PL_register_atom(ldnew->prompt.first);
  }
  ldnew->modules		 = LD->modules;
  ldnew->IO			 = LD->IO;
  ldnew->_fileerrors		 = LD->_fileerrors;
  ldnew->float_format		 = LD->float_format;
  ldnew->encoding		 = LD->encoding;
  ldnew->_debugstatus		 = LD->_debugstatus;
  ldnew->_debugstatus.retryFrame = NULL;
  ldnew->feature.mask		 = LD->feature.mask;
  if ( LD->feature.table )
  { PL_LOCK(L_FEATURE);
    ldnew->feature.table	 = copyHTable(LD->feature.table);
    PL_UNLOCK(L_FEATURE);
  }
  init_message_queue(&info->thread_data->thread.messages);

  pthread_attr_init(&attr);
  if ( info->detached )
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if ( stack )
    pthread_attr_setstacksize(&attr, stack);
  LOCK();
  rc = pthread_create(&info->tid, &attr, start_thread, info);
  UNLOCK();
  pthread_attr_destroy(&attr);
  if ( rc != 0 )
  { free_thread_info(info);
    return PL_error(NULL, 0, ThError(rc),
		    ERR_SYSCALL, "pthread_create");
  }

  return unify_thread_id(id, info);
}


static int
get_thread(term_t t, PL_thread_info_t **info, int warn)
{ int i = -1;

  if ( !PL_get_integer(t, &i) )
  { atom_t name;

    if ( !PL_get_atom(t, &name) )
      return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_thread, t);
    if ( threadTable )
    { Symbol s;

      if ( (s = lookupHTable(threadTable, (void *)name)) )
	i = (int)(long)s->value;
    }
  }

  if ( i < 0 || i >= MAX_THREADS || threads[i].status == PL_THREAD_UNUSED )
  { if ( warn )
      return PL_error(NULL, 0, "no info record",
		      ERR_EXISTENCE, ATOM_thread, t);
    else
      return FALSE;
  }
  
  *info = &threads[i];

  return TRUE;
}


int
unify_thread_id(term_t id, PL_thread_info_t *info)
{ if ( info->name )
    return PL_unify_atom(id, info->name);

  return PL_unify_integer(id, info->pl_tid);
}


static int
unify_thread_status(term_t status, PL_thread_info_t *info)
{ switch(info->status)
  { case PL_THREAD_CREATED:
    case PL_THREAD_RUNNING:
      return PL_unify_atom(status, ATOM_running);
    case PL_THREAD_EXITED:
    { term_t tmp = PL_new_term_ref();

      if ( info->return_value )
	PL_recorded(info->return_value, tmp);

      return PL_unify_term(status,
			   PL_FUNCTOR, FUNCTOR_exited1,
			     PL_TERM, tmp);
    }
    case PL_THREAD_SUCCEEDED:
      return PL_unify_atom(status, ATOM_true);
    case PL_THREAD_FAILED:
      return PL_unify_atom(status, ATOM_false);
    case PL_THREAD_EXCEPTION:
    { term_t tmp = PL_new_term_ref();

      PL_recorded(info->return_value, tmp);
      return PL_unify_term(status,
			   PL_FUNCTOR, FUNCTOR_exception1,
			     PL_TERM, tmp);
    }
    case PL_THREAD_NOMEM:
    { return PL_unify_term(status,
			   PL_FUNCTOR, FUNCTOR_exception1,
			     PL_FUNCTOR, FUNCTOR_error2,
			       PL_FUNCTOR, FUNCTOR_resource_error1,
			         PL_CHARS, "virtual_memory",
			       PL_VARIABLE);
    }
    case PL_THREAD_CANCELED:
      return PL_unify_atom(status, ATOM_canceled);
    default:
      DEBUG(1, Sdprintf("info->status = %d\n", info->status));
      fail;				/* can happen in current_thread/2 */
  }
}


word
pl_thread_self(term_t self)
{ return unify_thread_id(self, LD->thread.info);
}


static void
free_thread_info(PL_thread_info_t *info)
{ if ( info->thread_data )
    free_prolog_thread(info->thread_data);
  if ( info->return_value )
    PL_erase(info->return_value);
  if ( info->goal )
    PL_erase(info->goal);

  if ( info->name )
    unaliasThread(info->name);

  memset(info, 0, sizeof(*info));	/* sets status to PL_THREAD_UNUSED */
}


word
pl_thread_join(term_t thread, term_t retcode)
{ PL_thread_info_t *info;
  void *r;
  word rval;
  int rc;

  if ( !get_thread(thread, &info, TRUE) )
    fail;
  if ( info == LD->thread.info || info->detached )
  { return PL_error("thread_join", 2,
		    info->detached ? "Cannot join detached thread"
				   : "Cannot join self",
		    ERR_PERMISSION, ATOM_join, ATOM_thread, thread);
  }

  while( (rc=pthread_join(info->tid, &r)) == EINTR )
  { if ( PL_handle_signals() < 0 )
      fail;
  }
  switch(rc)
  { case 0:
      break;
    case ESRCH:
      Sdprintf("ESRCH from %d\n", info->tid);
      return PL_error("thread_join", 2, NULL,
		      ERR_EXISTENCE, ATOM_thread, thread);
    default:
      return PL_error("thread_join", 2, ThError(rc),
		      ERR_SYSCALL, "pthread_join");
  }
  
  rval = unify_thread_status(retcode, info);
   
  free_thread_info(info);

  return rval;
}


word
pl_thread_exit(term_t retcode)
{ PL_thread_info_t *info = LD->thread.info;

  LOCK();
  if ( LD->exit_requested )
    info->status = PL_THREAD_CANCELED;
  else
    info->status = PL_THREAD_EXITED;
  info->return_value = PL_record(retcode);
  UNLOCK();

  DEBUG(1, Sdprintf("thread_exit(%d)\n", info-threads));

  pthread_exit(NULL);
  assert(0);
  fail;
}


word
pl_thread_kill(term_t t, term_t sig)
{
#ifdef HAVE_PTHREAD_KILL
  PL_thread_info_t *info;
  int s, rc;

  
  if ( !get_thread(t, &info, TRUE) )
    fail;
  if ( !_PL_get_signum(sig, &s) )
    return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_signal, sig);

  if ( (rc=pthread_kill(info->tid, s)) )
  { assert(rc == ESRCH);

    return PL_error("thread_kill", 2, NULL, ERR_EXISTENCE, ATOM_thread, t);
  }

  succeed;
#else
  return notImplemented("thread_kill", 2);
#endif
}


static
PRED_IMPL("thread_detach", 1, thread_detach, 0)
{ PL_thread_info_t *info;

  LOCK();
  if ( !get_thread(A1, &info, TRUE) )
  { UNLOCK();
    fail;
  }

  if ( !info->detached )
  { int rc;

    if ( (rc=pthread_detach(info->tid)) )
    { assert(rc == ESRCH);

      free_thread_info(info);
    } else
      info->detached = TRUE;
  }

  UNLOCK();
  succeed;
}


word
pl_current_thread(term_t id, term_t status, control_t h)
{ int current;

  switch(ForeignControl(h))
  { case FRG_FIRST_CALL:
    { PL_thread_info_t *info;

      if ( PL_is_variable(id) )
      { current = 1;
	goto redo;
      }
      if ( !get_thread(id, &info, FALSE) )
        fail;

      return unify_thread_status(status, info);
    }
    case FRG_REDO:
      current = ForeignContextInt(h);
    redo:
      for( ; current < MAX_THREADS; current++ )
      { mark m;

	if ( !threads[current].tid )
	   continue;

	Mark(m);
	if ( unify_thread_id(id, &threads[current]) &&
	     unify_thread_status(status, &threads[current]) )
	  ForeignRedoInt(current+1);
	Undo(m);
      }
      fail;
    case FRG_CUTTED:
    default:
      succeed;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Sum the amount of heap allocated through all threads allocation-pools.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

long
threadLocalHeapUsed(void)
{ int i;
  PL_thread_info_t *info;
  long heap = 0;

  LOCK();
  for(i=0, info=threads; i<MAX_THREADS; i++, info++)
  { PL_local_data_t *ld;

    if ( (ld = info->thread_data) )
    { heap += ld->alloc_pool.allocated;
    }
  }
  UNLOCK();

  return heap;
}


static
PRED_IMPL("thread_setconcurrency", 2, thread_setconcurrency, 0)
{
#ifdef HAVE_PTHREAD_SETCONCURRENCY
  int val = pthread_getconcurrency();
  int rc;

  if ( PL_unify_integer(A1, val) )
  { if ( PL_compare(A1, A2) != 0  )
    { if ( PL_get_integer_ex(A2, &val) )
      { if ( (rc=pthread_setconcurrency(val)) != 0 )
	  return PL_error(NULL, 0, ThError(rc),
			  ERR_SYSCALL, "pthread_setconcurrency");
      }
    }
  }

  succeed;
#else
  return PL_unify_integer(A1, 0);
#endif
}


		 /*******************************
		 *	     CLEANUP		*
		 *******************************/

typedef enum { EXIT_PROLOG, EXIT_C } exit_type;

typedef struct _at_exit_goal
{ struct _at_exit_goal *next;		/* Next in queue */
  exit_type type;			/* Prolog or C */
  union
  { struct
    { Module   module;			/* Module for running goal */
      record_t goal;			/* Goal to run */
    } prolog;
    struct
    { void (*function)(void *);		/* called function */
      void *closure;			/* client data */
    } c;
  } goal;
} at_exit_goal;


foreign_t
pl_thread_at_exit(term_t goal)
{ Module m = NULL;
  at_exit_goal *eg = allocHeap(sizeof(*eg));

  PL_strip_module(goal, &m, goal);
  eg->next = NULL;
  eg->type = EXIT_PROLOG;
  eg->goal.prolog.module = m;
  eg->goal.prolog.goal   = PL_record(goal);

  eg->next = LD->thread.exit_goals;
  LD->thread.exit_goals = eg;

  succeed;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Request a function to run when the Prolog thread is about to detach, but
still capable of running Prolog queries.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
PL_thread_at_exit(void (*function)(void *), void *closure, int global)
{ at_exit_goal *eg = allocHeap(sizeof(*eg));

  eg->next = NULL;
  eg->type = EXIT_C;
  eg->goal.c.function = function;
  eg->goal.c.closure  = closure;

  if ( global )
  { LOCK();
    eg->next = GD->thread.exit_goals;
    GD->thread.exit_goals = eg;
    UNLOCK();
  } else
  { eg->next = LD->thread.exit_goals;
    LD->thread.exit_goals = eg;
  }

  succeed;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Newly pushed hooks are executed  after   all  currently registered hooks
have finished. 

Q: What to do with exceptions?
Q: Should we limit the passes?
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
run_exit_hooks(at_exit_goal *eg, int free)
{ at_exit_goal *next;
  term_t goal = PL_new_term_ref();
  fid_t fid = PL_open_foreign_frame();

  for( ; eg; eg = next)
  { next = eg->next;
  
    switch(eg->type)
    { case EXIT_PROLOG:
	PL_recorded(eg->goal.prolog.goal, goal);
        if ( free )
	  PL_erase(eg->goal.prolog.goal);
	callProlog(eg->goal.prolog.module, goal, PL_Q_NODEBUG, NULL);
	PL_rewind_foreign_frame(fid);
	break;
      case EXIT_C:
	(*eg->goal.c.function)(eg->goal.c.closure);
        break;
      default:
	assert(0);
    }
      
    if ( free )
      freeHeap(eg, sizeof(*eg));
  }

  PL_discard_foreign_frame(fid);
  PL_reset_term_refs(goal);
}



static void
run_thread_exit_hooks()
{ at_exit_goal *eg;

  while( (eg = LD->thread.exit_goals) )
  { LD->thread.exit_goals = NULL;	/* empty these */

    run_exit_hooks(eg, TRUE);
  }

  run_exit_hooks(GD->thread.exit_goals, FALSE);
}


		 /*******************************
		 *	   THREAD SIGNALS	*
		 *******************************/

typedef struct _thread_sig
{ struct _thread_sig *next;		/* Next in queue */
  Module   module;			/* Module for running goal */
  record_t goal;			/* Goal to run */
} thread_sig;


static int
is_alive(int status)
{ switch(status)
  { case PL_THREAD_CREATED:
    case PL_THREAD_RUNNING:
    case PL_THREAD_SUSPENDED:
    case PL_THREAD_RESUMING:
      succeed;
    default:
      fail;
  }
}


foreign_t
pl_thread_signal(term_t thread, term_t goal)
{ Module m = NULL;
  thread_sig *sg;
  PL_thread_info_t *info;
  PL_local_data_t *ld;

  PL_strip_module(goal, &m, goal);

  LOCK();
  if ( !get_thread(thread, &info, TRUE) )
  { UNLOCK();
    fail;
  }
  if ( !is_alive(info->status) )
  { PL_error(NULL, 0, NULL, ERR_EXISTENCE, ATOM_thread, thread);
    UNLOCK();
    fail;
  }       

  sg = allocHeap(sizeof(*sg));
  sg->next = NULL;
  sg->module = m;
  sg->goal = PL_record(goal);

  ld = info->thread_data;
  if ( !ld->thread.sig_head )
    ld->thread.sig_head = ld->thread.sig_tail = sg;
  else
  { ld->thread.sig_tail->next = sg;
    ld->thread.sig_tail = sg;
  }
  raiseSignal(ld, SIG_THREAD_SIGNAL);

#ifdef WIN32
  if ( ld->thread.info->w32id )
    PostThreadMessage(ld->thread.info->w32id, WM_SIGNALLED, 0, 0L);
#endif

  UNLOCK();

  succeed;
}


void
executeThreadSignals(int sig)
{ thread_sig *sg, *next;
  fid_t fid = PL_open_foreign_frame();
  term_t goal = PL_new_term_ref();

  LOCK();
  sg = LD->thread.sig_head;
  LD->thread.sig_head = LD->thread.sig_tail = NULL;
  UNLOCK();

  for( ; sg; sg = next)
  { term_t ex;
    int rval;
  
    next = sg->next;
    PL_recorded(sg->goal, goal);
    PL_erase(sg->goal);
    rval = callProlog(sg->module, goal, PL_Q_CATCH_EXCEPTION, &ex);
    freeHeap(sg, sizeof(*sg));

    if ( !rval && ex )
    { PL_close_foreign_frame(fid);
      PL_raise_exception(ex);

      for(sg = next; sg; sg=next)
      { next = sg->next;
	PL_erase(sg->goal);
	freeHeap(sg, sizeof(*sg));
      }

      return;
    }

    PL_rewind_foreign_frame(fid);
  }

  PL_discard_foreign_frame(fid);
}


static void
freeThreadSignals(PL_local_data_t *ld)
{ thread_sig *sg;
  thread_sig *next;

  for( sg = ld->thread.sig_head; sg; sg = next )
  { next = sg->next;

    PL_erase(sg->goal);
    freeHeap(sg, sizeof(*sg));
  }
}


		 /*******************************
		 *	  MESSAGE QUEUES	*
		 *******************************/

#ifdef WIN32
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Earlier implementations used pthread-win32 condition   variables.  As we
need to dispatch messages while waiting for a condition variable we need
to use pthread_cond_timedwait() which is   really  complicated and SLOW.
Below is an  alternative  emulation   of  pthread_cond_wait()  that does
dispatch messages. It is not fair  nor   correct,  but  neither of these
problems bothers us considering the promises we make about Win32 message
queues. This implentation is about 250 times faster, providing about the
same performance as on native  pthread   implementations  such as Linux.
This work was sponsored by SSS, http://www.sss.co.nz

This implementation is based on   the following, summarizing discussions
on comp.lang.thread.

Strategies for Implementing POSIX Condition Variables on Win32
Douglas C. Schmidt and Irfan Pyarali
Department of Computer Science
Washington University, St. Louis, Missouri
http://www.cs.wustl.edu/~schmidt/win32-cv-1.html

It uses the second alternative, avoiding   the extra critical section as
we assume the condition  variable  is   always  associated  to  the same
critical section (associated to the same SWI-Prolog message queue).

The resulting implementation suffers from the following problems:

  * Unfairness
    If two threads are waiting and two messages arrive on the queue it
    is possible for one thread to consume both of them. We never
    anticipated on `fair' behaviour in this sense in SWI-Prolog, so
    we should not be bothered.  Nevertheless existing application may
    have assumed fairness.

  * Incorrectness
    If two threads are waiting, a broadcast happens and a third thread
    kicks in it is possible one of the threads does not get a wakeup.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */


static int 
win32_cond_init(win32_cond_t *cv)
{ cv->events[SIGNAL]    = CreateEvent(NULL, FALSE, FALSE, NULL);
  cv->events[BROADCAST] = CreateEvent(NULL, TRUE,  FALSE, NULL);
  cv->waiters = 0;

  return 0;
}


static int
win32_cond_destroy(win32_cond_t *cv)
{ CloseHandle(cv->events[SIGNAL]);
  CloseHandle(cv->events[BROADCAST]);

  return 0;
}


static int 
win32_cond_wait(win32_cond_t *cv,
		CRITICAL_SECTION *external_mutex)
{ int rc, last;

  cv->waiters++;

  LeaveCriticalSection(external_mutex);
  rc = MsgWaitForMultipleObjects(2,
				 cv->events,
				 FALSE,	/* wait for either event */
				 INFINITE,
				 QS_ALLINPUT);
  if ( rc == WAIT_OBJECT_0+2 )
  { MSG msg;

    while( PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) )
    { TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    if ( LD->pending_signals )
    { EnterCriticalSection(external_mutex);
      return EINTR;
    }
  }

  EnterCriticalSection(external_mutex);

  cv->waiters--;
  last = (rc == WAIT_OBJECT_0 + BROADCAST && cv->waiters == 0);
  if ( last )
    ResetEvent (cv->events[BROADCAST]);

  return 0;
}


static int 
win32_cond_signal(win32_cond_t *cv)	/* must be holding associated mutex */
{ if ( cv->waiters > 0 )
    SetEvent(cv->events[SIGNAL]);

  return 0;
}


static int 
win32_cond_broadcast(win32_cond_t *cv)	/* must be holding associated mutex */
{ if ( cv->waiters > 0 )
    SetEvent(cv->events[BROADCAST]);

  return 0;
}

#define cv_broadcast 	win32_cond_broadcast
#define cv_signal    	win32_cond_signal
#define cv_init(cv,p)	win32_cond_init(cv)
#define cv_destroy	win32_cond_destroy
#else /*WIN32*/
#define cv_broadcast	pthread_cond_broadcast
#define cv_signal	pthread_cond_signal
#define cv_init(cv,p)	pthread_cond_init(cv, p)
#define cv_destroy	pthread_cond_destroy
#endif /*WIN32*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This code deals with telling other threads something.  The interface:

	thread_get_message(-Message)
	thread_send_message(+Id, +Message)

Queues can be waited for by   multiple  threads using different (partly)
instantiated patterns for Message. For this   reason all waiting threads
should be restarted using pthread_cond_broadcast().   However,  if there
are a large number of workers only   waiting for `any' message this will
cause all of them to wakeup for only   one  to grab the message. This is
highly undesirable and therefore the queue  keeps two counts: the number
of waiting threads and the number waiting  with a variable. Only if they
are not equal and there are multiple waiters we must be using broadcast.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct _thread_msg
{ struct _thread_msg *next;		/* next in queue */
  record_t            message;		/* message in queue */
  word		      key;		/* Indexing key */
} thread_message;


static void
queue_message(message_queue *queue, term_t msg)
{ thread_message *msgp;

  msgp = allocHeap(sizeof(*msgp));
  msgp->next    = NULL;
  msgp->message = PL_record(msg);
  msgp->key     = getIndexOfTerm(msg);
  
  simpleMutexLock(&queue->mutex);

  if ( !queue->head )
  { queue->head = queue->tail = msgp;
  } else
  { queue->tail->next = msgp;
    queue->tail = msgp;
  }

  if ( queue->waiting )
  { if ( queue->waiting > queue->waiting_var && queue->waiting > 1 )
    { DEBUG(1, Sdprintf("%d of %d non-var waiters; broadcasting\n",
			queue->waiting - queue->waiting_var,
		        queue->waiting));
      cv_broadcast(&queue->cond_var);
    } else
    { DEBUG(1, Sdprintf("%d var waiters; signalling\n", queue->waiting));
      cv_signal(&queue->cond_var);
    }
  } else
  { DEBUG(1, Sdprintf("No waiters\n"));
  }

  simpleMutexUnlock(&queue->mutex);
}


typedef struct
{ message_queue *queue;
  int            isvar;
} get_msg_cleanup_context;


static void
cleanup_get_message(void *context)
{ get_msg_cleanup_context *ctx = context;

  ctx->queue->waiting--;
  ctx->queue->waiting_var -= ctx->isvar;
  simpleMutexUnlock(&ctx->queue->mutex);
}

#ifdef WIN32

static int
dispatch_cond_wait(message_queue *queue)
{ return win32_cond_wait(&queue->cond_var, &queue->mutex);
}

#else /*WIN32*/

#if 0

static int
dispatch_cond_wait(message_queue *queue)
{ int rc;

  rc = pthread_cond_wait(&queue->cond_var, &queue->mutex);

  return rc;
}

#else

static int
dispatch_cond_wait(message_queue *queue)
{ struct timeval now;
  struct timespec timeout;
  int rc;

  for(;;)
  { gettimeofday(&now, NULL);
    timeout.tv_sec  = now.tv_sec;
    timeout.tv_nsec = (now.tv_usec+250000) * 1000;

    if ( timeout.tv_nsec >= 1000000000 ) /* some platforms demand this */
    { timeout.tv_nsec -= 1000000000;
      timeout.tv_sec += 1;
    }

    rc = pthread_cond_timedwait(&queue->cond_var, &queue->mutex, &timeout);
#ifdef O_DEBUG
    if ( LD && LD->thread.info )	/* can be absent during shutdown */
    { switch( LD->thread.info->ldata_status )
      { case LDATA_IDLE:
	case LDATA_ANSWERED:
	case LDATA_SIGNALLED:
	  break;
	default:
	  Sdprintf("%d: ldata_status = %d\n",
		   PL_thread_self(), LD->thread.info->ldata_status);
      }
    } else
    { return EINTR;
    }
#endif

    switch( rc )
    { case ETIMEDOUT:
      { if ( LD->pending_signals )
	  return EINTR;

	if ( queue->head )
	{ DEBUG(1, Sdprintf("[%d]: ETIMEDOUT: queue not empty\n",
			    PL_thread_self()));
	  return 0;
	}

	continue;
      }
      default:
	return rc;
    }
  }
}
#endif


#endif /*WIN32*/

static int
get_message(message_queue *queue, term_t msg)
{ get_msg_cleanup_context ctx;
  term_t tmp = PL_new_term_ref();
  int isvar = PL_is_variable(msg) ? 1 : 0;
  word key = (isvar ? 0L : getIndexOfTerm(msg));
  int rval = TRUE;
  mark m;

  Mark(m);

  ctx.queue = queue;
  ctx.isvar = isvar;
  pthread_cleanup_push(cleanup_get_message, (void *)&ctx);
  simpleMutexLock(&queue->mutex);

  for(;;)
  { thread_message *msgp = queue->head;
    thread_message *prev = NULL;

    DEBUG(1, Sdprintf("%d: scanning queue\n", PL_thread_self()));
    for( ; msgp; prev = msgp, msgp = msgp->next )
    { if ( key && msgp->key && key != msgp->key )
	continue;			/* fast search */

      PL_recorded(msgp->message, tmp);

      if ( PL_unify(msg, tmp) )
      { DEBUG(1, Sdprintf("%d: match\n", PL_thread_self()));
	if ( prev )
	{ if ( !(prev->next = msgp->next) )
	    queue->tail = prev;
	} else
	{ if ( !(queue->head = msgp->next) )
	    queue->tail = NULL;
	}
	PL_erase(msgp->message);
	freeHeap(msgp, sizeof(*msgp));
	goto out;
      }

      Undo(m);			/* reclaim term */
    }
				/* linux docs say it may return EINTR */
				/* does it re-lock in that case? */
    queue->waiting++;
    queue->waiting_var += isvar;
    DEBUG(1, Sdprintf("%d: waiting on queue\n", PL_thread_self()));
    while( dispatch_cond_wait(queue) == EINTR )
    { DEBUG(1, Sdprintf("%d: EINTR\n", PL_thread_self()));

      if ( !LD )			/* needed for clean exit */
      { Sdprintf("Forced exit from get_message()\n");
	exit(1);
      }
      
      if ( PL_handle_signals() < 0 )	/* thread-signal */
      { queue->waiting--;
	queue->waiting_var -= isvar;
	rval = FALSE;
	goto out;
      }
    }
    DEBUG(1, Sdprintf("%d: wakeup on queue\n", PL_thread_self()));
    queue->waiting--;
    queue->waiting_var -= isvar;
  }
out:

  simpleMutexUnlock(&queue->mutex);
  pthread_cleanup_pop(0);

  return rval;
}


static int
peek_message(message_queue *queue, term_t msg)
{ thread_message *msgp;
  term_t tmp = PL_new_term_ref();
  word key = getIndexOfTerm(msg);
  mark m;

  Mark(m);

  simpleMutexLock(&queue->mutex);
  msgp = queue->head;

  for( msgp = queue->head; msgp; msgp = msgp->next )
  { if ( key && msgp->key && key != msgp->key )
      continue;
    PL_recorded(msgp->message, tmp);

    if ( PL_unify(msg, tmp) )
    { simpleMutexUnlock(&queue->mutex);
      succeed;
    }

    Undo(m);
  }
     
  simpleMutexUnlock(&queue->mutex);
  fail;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Deletes the contents of the message-queue as well as the queue itself.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
destroy_message_queue(message_queue *queue)
{ thread_message *msgp;
  thread_message *next;

  for( msgp = queue->head; msgp; msgp = next )
  { next = msgp->next;

    PL_erase(msgp->message);
    freeHeap(msgp, sizeof(*msgp));
  }
  
  simpleMutexDelete(&queue->mutex);
  cv_destroy(&queue->cond_var);
}


static void
init_message_queue(message_queue *queue)
{ memset(queue, 0, sizeof(*queue));
  simpleMutexInit(&queue->mutex);
  cv_init(&queue->cond_var, NULL);
}

					/* Prolog predicates */

word
pl_thread_send_message(term_t queue, term_t msg)
{ message_queue *q;

  LOCK();
  if ( !get_message_queue(queue, &q) )
  { UNLOCK();
    fail;
  }

  queue_message(q, msg);
  UNLOCK();

  succeed;
}


foreign_t
pl_thread_get_message(term_t msg)
{ return get_message(&LD->thread.messages, msg);
}


word
pl_thread_peek_message(term_t msg)
{ return peek_message(&LD->thread.messages, msg);
}


		 /*******************************
		 *     USER MESSAGE QUEUES	*
		 *******************************/

static int
unify_queue(term_t t, message_queue *q)
{ if ( isAtom(q->id) )
    return PL_unify_atom(t, q->id);
  else
    return PL_unify_term(t,
			 PL_FUNCTOR, FUNCTOR_dmessage_queue1,
			 PL_LONG, valInt(q->id));
}


static message_queue *
unlocked_message_queue_create(term_t queue)
{ Symbol s;
  atom_t name = NULL_ATOM;
  message_queue *q;
  word id;

  if ( !queueTable )
    queueTable = newHTable(16);
  
  if ( PL_get_atom(queue, &name) )
  { if ( (s = lookupHTable(queueTable, (void *)name)) ||
	 (s = lookupHTable(threadTable, (void *)name)) )
    { PL_error("message_queue_create", 1, NULL, ERR_PERMISSION,
	       ATOM_message_queue, ATOM_create, queue);
      return NULL;
    }
    id = name;
  } else if ( PL_is_variable(queue) )
  { id = consInt(queue_id++);
  } else
  { PL_error("message_queue_create", 1, NULL,
	     ERR_TYPE, ATOM_message_queue, queue);
    return NULL;
  }

  q = PL_malloc(sizeof(*q));
  init_message_queue(q);
  q->id    = id;
  addHTable(queueTable, (void *)id, q);

  if ( unify_queue(queue, q) )
    return q;

  return NULL;
}


/* MT: Caller must hold the L_THREAD mutex
*/

static int
get_message_queue(term_t t, message_queue **queue)
{ atom_t name;
  word id = 0;
  int tid;

  if ( PL_get_atom(t, &name) )
  { id = name;
  } else if ( PL_is_functor(t, FUNCTOR_dmessage_queue1) )
  { term_t a = PL_new_term_ref();
    long i;

    PL_get_arg(1, t, a);
    if ( PL_get_long(a, &i) )
      id = consInt(i);
  } else if ( PL_get_integer(t, &tid) )
  { thread_queue:
    if ( tid < 0 || tid >= MAX_THREADS ||
	 threads[tid].status == PL_THREAD_UNUSED ||
	 !threads[tid].thread_data )
      return PL_error(NULL, 0, NULL, ERR_EXISTENCE, ATOM_thread, t);

    *queue = &threads[tid].thread_data->thread.messages;
    return TRUE;
  }
  if ( !id )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_message_queue, t);

  if ( queueTable )
  { Symbol s = lookupHTable(queueTable, (void *)id);

    if ( s )
    { *queue = s->value;
      return TRUE;
    }
  }
  if ( threadTable )
  { Symbol s = lookupHTable(threadTable, (void *)id);

    if ( s )
    { tid = (int)(long)s->value;
      goto thread_queue;
    }
  }

  return PL_error(NULL, 0, NULL, ERR_EXISTENCE, ATOM_message_queue, t);
}


static
PRED_IMPL("message_queue_create", 1, message_queue_create, 0)
{ int rval;

  LOCK();
  rval = (unlocked_message_queue_create(A1) ? TRUE : FALSE);
  UNLOCK();

  return rval;
}


static
PRED_IMPL("message_queue_destroy", 1, message_queue_destroy, 0)
{ message_queue *q;
  Symbol s;

  LOCK();
  if ( !get_message_queue(A1, &q) )
  { UNLOCK();
    fail;
  }

					/* only heuristic!  How to do */
					/* proper locking? */
  if ( q->waiting )
  { PL_error("message_queue_destroy", 1, "Has waiting threads", ERR_PERMISSION,
	     ATOM_message_queue, ATOM_destroy, A1);
    UNLOCK();
    fail;
  }

  s = lookupHTable(queueTable, (void *)q->id);
  assert(s);
  destroy_message_queue(q);
  deleteSymbolHTable(queueTable, s);
  PL_free(q);
  UNLOCK();

  succeed;
}


static 
PRED_IMPL("message_queue_size", 2, message_queue_size, 0)
{ message_queue *q;
  thread_message *m;
  int rc, n;

  LOCK();
  rc = get_message_queue(A1, &q);
  UNLOCK();
  if ( !rc )
    fail;


  simpleMutexLock(&q->mutex);
  for(n=0, m=q->head; m; m = m->next)
    n++;
  simpleMutexUnlock(&q->mutex);

  return PL_unify_integer(A2, n);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
thread_get_message(+Queue, -Message)
thread_get_message(-Message)
    Get a message from a message queue. If the queue is not provided get
    a message from the queue implicitly associated to the thread.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static
PRED_IMPL("thread_get_message", 2, thread_get_message, 0)
{ message_queue *q;
  int rc;
  
  LOCK();
  rc=get_message_queue(A1, &q);
  UNLOCK();
  if ( !rc )
    fail;
  
  return get_message(q, A2);
}


static
PRED_IMPL("thread_peek_message", 2, thread_peek_message, 0)
{ message_queue *q;
  int rc;
  
  LOCK();
  rc=get_message_queue(A1, &q);
  UNLOCK();
  if ( !rc )
    fail;
  
  return peek_message(q, A2);
}


		 /*******************************
		 *	 MUTEX PRIMITIVES	*
		 *******************************/

#ifdef NEED_RECURSIVE_MUTEX_INIT

#ifdef RECURSIVE_MUTEXES
static int
recursive_attr(pthread_mutexattr_t **ap)
{ static int done;
  static pthread_mutexattr_t attr;
  int rc;

  if ( done )
  { *ap = &attr;
    return 0;
  }

  LOCK();
  if ( done )
  { UNLOCK();

    *ap = &attr;
    return 0;
  }
  if ( (rc=pthread_mutexattr_init(&attr)) )
    goto error;
#ifdef HAVE_PTHREAD_MUTEXATTR_SETTYPE
  if ( (rc=pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)) )
    goto error;
#else
#ifdef HAVE_PTHREAD_MUTEXATTR_SETKIND_NP
  if ( (rc=pthread_mutexattr_setkind_np(&attr, PTHREAD_MUTEX_RECURSIVE_NP)) )
    goto error;
#endif
#endif

  done = TRUE;
  UNLOCK();
  *ap = &attr;

  return 0;

error:
  UNLOCK();
  return rc;
}
#endif

int
recursiveMutexInit(recursiveMutex *m)
{ 
#ifdef RECURSIVE_MUTEXES
  pthread_mutexattr_t *attr = NULL;
  int rc;
  
  if ( (rc=recursive_attr(&attr)) )
    return rc;
  
  return pthread_mutex_init(m, attr);

#else /*RECURSIVE_MUTEXES*/

  m->owner = 0;
  m->count = 0;
  return pthread_mutex_init(&(m->lock), NULL);

#endif /* RECURSIVE_MUTEXES */
}

#endif /*NEED_RECURSIVE_MUTEX_INIT*/

#ifdef NEED_RECURSIVE_MUTEX_DELETE

int
recursiveMutexDelete(recursiveMutex *m)
{ if ( m->owner != 0 )
    return EBUSY;

  return pthread_mutex_destroy(&(m->lock));
}

#endif /*NEED_RECURSIVE_MUTEX_DELETE*/

#ifndef RECURSIVE_MUTEXES
int
recursiveMutexLock(recursiveMutex *m)
{ int result = 0;
  pthread_t self = pthread_self();

  if ( pthread_equal(self, m->owner) )
    m->count++;
  else
  { result = pthread_mutex_lock(&(m->lock));
    m->owner = self;
    m->count = 1;
  }

  return result;
}


int
recursiveMutexTryLock(recursiveMutex *m)
{ int result = 0;
  pthread_t self = pthread_self();

  if ( pthread_equal(self, m->owner) )
    m->count++;
  else
  { result = pthread_mutex_trylock(&(m->lock));
    if ( result == 0 )
    { m->owner = self;
      m->count = 1;
    }
  }

  return result;
}


int
recursiveMutexUnlock(recursiveMutex *m)
{ int result = 0;
  pthread_t self = pthread_self();

  if ( pthread_equal(self,m->owner) )
  { if ( --m->count < 1 )
    { m->owner = 0;
      result = pthread_mutex_unlock(&(m->lock));
    }
  } else if ( !pthread_equal(m->owner, 0) )
  { Sdprintf("unlocking unowned mutex %p,not done!!\n", m);
    Sdprintf("\tlocking thread was %u , unlocking is %u\n",m->owner,self);

    result = -1;
  }

  return result;
}

#endif /*RECURSIVE_MUTEXES*/


counting_mutex *
allocSimpleMutex(const char *name)
{ counting_mutex *m = PL_malloc(sizeof(*m));

  simpleMutexInit(&m->mutex);
  m->count = 0L;
  m->unlocked = 0L;
#ifdef O_CONTENTION_STATISTICS
  m->collisions = 0L;
#endif
  if ( name )
    m->name = store_string(name);
  else
    m->name = NULL;
  LOCK();
  m->next = GD->thread.mutexes;
  GD->thread.mutexes = m;
  UNLOCK();

  return m;
}


void
freeSimpleMutex(counting_mutex *m)
{ counting_mutex *cm;

  simpleMutexDelete(&m->mutex);
  LOCK();
  if ( m == GD->thread.mutexes )
  { GD->thread.mutexes = m->next;
  } else
  { for(cm=GD->thread.mutexes; cm; cm=cm->next)
    { if ( cm->next == m )
	cm->next = m->next;
    }
  }
  UNLOCK();

  remove_string((char *)m->name);
  PL_free(m);
}


		 /*******************************
		 *	    USER MUTEXES	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
User-level mutexes. On Windows we can't   use  critical sections here as
TryEnterCriticalSection() is only defined on NT 4, not on Windows 95 and
friends.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
unify_mutex(term_t t, pl_mutex *m)
{ if ( isAtom(m->id) )
    return PL_unify_atom(t, m->id);
  else
    return PL_unify_term(t,
			 PL_FUNCTOR, FUNCTOR_dmutex1,
			 PL_LONG, valInt(m->id));
}


static int
unify_mutex_owner(term_t t, int owner)
{ if ( owner )
    return unify_thread_id(t, &threads[owner]);
  else
    return PL_unify_nil(t);
}


static pl_mutex *
mutexCreate(atom_t name)
{ pl_mutex *m;

  m = allocHeap(sizeof(*m));
  pthread_mutex_init(&m->mutex, NULL);
  m->count = 0;
  m->owner = 0;
  m->id    = name;
  addHTable(GD->thread.mutexTable, (void *)name, m);

  return m;
}


static pl_mutex *
unlocked_pl_mutex_create(term_t mutex)
{ Symbol s;
  atom_t name = NULL_ATOM;
  pl_mutex *m;
  word id;

  if ( PL_get_atom(mutex, &name) )
  { if ( (s = lookupHTable(GD->thread.mutexTable, (void *)name)) )
    { PL_error("mutex_create", 1, NULL, ERR_PERMISSION,
	       ATOM_mutex, ATOM_create, mutex);
      return NULL;
    }
    id = name;
  } else if ( PL_is_variable(mutex) )
  { id = consInt(GD->thread.mutex_next_id++);
  } else
  { PL_error("mutex_create", 1, NULL, ERR_TYPE, ATOM_mutex, mutex);
    return NULL;
  }

  m = mutexCreate(id);

  if ( unify_mutex(mutex, m) )
    return m;

  return NULL;
}


foreign_t
pl_mutex_create(term_t mutex)
{ int rval;

  LOCK();
  rval = (unlocked_pl_mutex_create(mutex) ? TRUE : FALSE);
  UNLOCK();

  return rval;
}


static int
get_mutex(term_t t, pl_mutex **mutex, int create)
{ atom_t name;
  word id = 0;

  if ( PL_get_atom(t, &name) )
  { id = name;
  } else if ( PL_is_functor(t, FUNCTOR_dmutex1) )
  { term_t a = PL_new_term_ref();
    long i;

    PL_get_arg(1, t, a);
    if ( PL_get_long(a, &i) )
      id = consInt(i);
  }
  if ( !id )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_mutex, t);

  LOCK();
  if ( GD->thread.mutexTable )
  { Symbol s = lookupHTable(GD->thread.mutexTable, (void *)id);

    if ( s )
    { *mutex = s->value;
      UNLOCK();
      return TRUE;
    }
  }

  if ( create && isTextAtom(id) )
  { pl_mutex *new;

    if ( (new = unlocked_pl_mutex_create(t)) )
    { *mutex = new;
      UNLOCK();
      return TRUE;
    }
  }
  UNLOCK();

  return PL_error(NULL, 0, NULL, ERR_EXISTENCE, ATOM_mutex, t);
}



int
PL_mutex_lock(struct pl_mutex *m)
{ int self = PL_thread_self();

  if ( self == m->owner )
  { m->count++;
  } else
  { pthread_mutex_lock(&m->mutex);
    m->count = 1;
    m->owner = self;
  }

  succeed;
}


foreign_t
pl_mutex_lock(term_t mutex)
{ pl_mutex *m;

  if ( !get_mutex(mutex, &m, TRUE) )
    fail;

  return  PL_mutex_lock(m);
}


foreign_t
pl_mutex_trylock(term_t mutex)
{ pl_mutex *m;
  int self = PL_thread_self();
  int rval;

  if ( !get_mutex(mutex, &m, TRUE) )
    fail;

  if ( self == m->owner )
  { m->count++;
  } else if ( (rval = pthread_mutex_trylock(&m->mutex)) == 0 )
  { m->count = 1;
    m->owner = self;
  } else if ( rval == EBUSY )
  { fail;
  } else
  { assert(0);
  }

  succeed;
}


int
PL_mutex_unlock(struct pl_mutex *m)
{ int self = PL_thread_self();

  if ( self == m->owner )
  { if ( --m->count == 0 )
    { m->owner = 0;

      pthread_mutex_unlock(&m->mutex);
    }

    succeed;
  }

  fail;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The error message of this  predicate  is   not  thread-safe.  I.e. it is
possible the message is wrong. This can   only be fixed by modifying the
API of PL_mutex_unlock(), which is asking a  bit too much for this small
error.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

foreign_t
pl_mutex_unlock(term_t mutex)
{ pl_mutex *m;

  if ( !get_mutex(mutex, &m, FALSE) )
    fail;

  if ( !PL_mutex_unlock(m) )
  { char *msg = m->owner ? "not owner" : "not locked";

    return PL_error("mutex_unlock", 1, msg, ERR_PERMISSION,
		    ATOM_mutex, ATOM_unlock, mutex);
  }

  succeed;
}


foreign_t
pl_mutex_unlock_all()
{ TableEnum e;
  Symbol s;
  int tid = PL_thread_self();

  e = newTableEnum(GD->thread.mutexTable);
  while( (s = advanceTableEnum(e)) )
  { pl_mutex *m = s->value;
    
    if ( m->owner == tid )
    { m->count = 0;
      m->owner = 0;
      pthread_mutex_unlock(&m->mutex);
    }
  }
  freeTableEnum(e);
  succeed;
}


foreign_t
pl_mutex_destroy(term_t mutex)
{ pl_mutex *m;
  Symbol s;

  if ( !get_mutex(mutex, &m, FALSE) )
    fail;

  LOCK();
  if ( m->owner )
  { char msg[100];
    
    UNLOCK();
    Ssprintf(msg, "Owned by thread %d", m->owner); /* TBD: named threads */
    return PL_error("mutex_destroy", 1, msg,
		    ERR_PERMISSION, ATOM_mutex, ATOM_destroy, mutex);
  }

  pthread_mutex_destroy(&m->mutex);
  s = lookupHTable(GD->thread.mutexTable, (void *)m->id);
  deleteSymbolHTable(GD->thread.mutexTable, s);
  freeHeap(m, sizeof(*m));
  UNLOCK();

  succeed;
}


foreign_t
pl_current_mutex(term_t mutex, term_t owner, term_t count, control_t h)
{ TableEnum e;
  Symbol s;
  mark mrk;

  switch(ForeignControl(h))
  { case FRG_FIRST_CALL:
    { if ( PL_is_variable(mutex) )
      { e = newTableEnum(GD->thread.mutexTable);
      } else
      { pl_mutex *m;

        if ( get_mutex(mutex, &m, FALSE) &&
	     unify_mutex_owner(owner, m->owner) &&
	     PL_unify_integer(count, m->count) )
	  succeed;

	fail;
      }
      break;
    }
    case FRG_REDO:
      e = ForeignContextPtr(h);
      break;
    case FRG_CUTTED:
      e = ForeignContextPtr(h);
      freeTableEnum(e);
    default:
      succeed;
  }

  Mark(mrk);
  while ( (s = advanceTableEnum(e)) )
  { pl_mutex *m = s->value;

    if ( unify_mutex(mutex, m) &&
	 unify_mutex_owner(owner, m->owner) &&
	 PL_unify_integer(count, m->count) )
    { ForeignRedoPtr(e);
    }
    Undo(mrk);
  }

  freeTableEnum(e);
  fail;
}


		 /*******************************
		 *	FOREIGN INTERFACE	*
		 *******************************/

int
PL_thread_attach_engine(PL_thread_attr_t *attr)
{ PL_thread_info_t *info;
  PL_local_data_t *ldnew;
  PL_local_data_t *ldmain;

  if ( LD )
    LD->thread.info->open_count++;

  LOCK();
  if ( !GD->thread.enabled )
  { UNLOCK();
#ifdef EPERM				/* FIXME: Better reporting */
    errno = EPERM;
#endif
    return -1;
  }

  info = alloc_thread();
  UNLOCK();
  if ( !info )
    return -1;				/* out of threads */

  ldmain = threads[1].thread_data;
  ldnew = info->thread_data;

  if ( attr )
  { if ( attr->local_size )
      info->local_size = attr->local_size * 1024;
    if ( attr->global_size )
      info->global_size = attr->global_size * 1024;
    if ( attr->trail_size )
      info->trail_size = attr->trail_size * 1024;
    if ( attr->argument_size )
      info->argument_size = attr->argument_size * 1024;

    info->cancel = attr->cancel;
  }
  
  info->goal       = NULL;
  info->module     = MODULE_user;
  info->detached   = TRUE;		/* C-side should join me */
  info->status     = PL_THREAD_RUNNING;
  info->open_count = 1;
  init_message_queue(&info->thread_data->thread.messages);

  ldnew->prompt			 = ldmain->prompt;
  ldnew->modules		 = ldmain->modules;
  ldnew->IO			 = ldmain->IO;
  ldnew->_fileerrors		 = ldmain->_fileerrors;
  ldnew->float_format		 = ldmain->float_format;
  ldnew->encoding		 = ldmain->encoding;
  ldnew->_debugstatus		 = ldmain->_debugstatus;
  ldnew->_debugstatus.retryFrame = NULL;
  ldnew->feature.mask		 = ldmain->feature.mask;
  if ( ldmain->feature.table )
  { TLD_set(PL_ldata, info->thread_data);

    PL_LOCK(L_FEATURE);
    ldnew->feature.table	 = copyHTable(ldmain->feature.table);
    PL_UNLOCK(L_FEATURE);
  }

  if ( !initialise_thread(info) )
  { free_thread_info(info);
    errno = ENOMEM;
    return -1;
  }
  info->tid = pthread_self();		/* we are complete now */
  set_system_thread_id(info);
  if ( attr && attr->alias )
  { if ( !aliasThread(info->pl_tid, PL_new_atom(attr->alias)) )
    { free_thread_info(info);
      errno = EPERM;
      return -1;
    }
  }
  PL_call_predicate(MODULE_system, PL_Q_NORMAL, PROCEDURE_dthread_init0, 0);

  return info->pl_tid;
}


int
PL_thread_destroy_engine()
{ PL_local_data_t *ld = LD;

  if ( ld )
  { if ( --ld->thread.info->open_count == 0 )
    { free_prolog_thread(ld);
      TLD_set(PL_ldata, NULL);
    }

    return TRUE;
  }

  return FALSE;				/* we had no thread */
}


int
attachConsole()
{ fid_t fid = PL_open_foreign_frame();
  int rval;
  predicate_t pred = PL_predicate("attach_console", 0, "user");

  rval = PL_call_predicate(NULL, PL_Q_NODEBUG, pred, 0);

  PL_discard_foreign_frame(fid);

  return rval;
}


		 /*******************************
		 *	      ENGINES		*
		 *******************************/

static PL_engine_t
PL_current_engine(void)
{ return LD;
}


static void
detach_engine(PL_engine_t e)
{ PL_thread_info_t *info = e->thread.info;

#ifdef __linux__
  info->pid = -1;
#endif
#ifdef WIN32
  info->w32id = 0;
#endif
  info->tid = 0L;

  TLD_set(PL_ldata, NULL);
}


int
PL_set_engine(PL_engine_t new, PL_engine_t *old)
{ PL_engine_t current = PL_current_engine();

  if ( new != current && new != PL_ENGINE_CURRENT )
  { LOCK();

    if ( new )
    { if ( new == PL_ENGINE_MAIN )
	new = &PL_local_data;
  
      if ( new->magic != LD_MAGIC )
      { UNLOCK();
	return PL_ENGINE_INVAL;
      }
      if ( new->thread.info->tid )
      { UNLOCK();
	return PL_ENGINE_INUSE;
      }
    }
  
    if ( current )
      detach_engine(current);
  
    if ( new )
    { TLD_set(PL_ldata, new);
      new->thread.info->tid = pthread_self();
      set_system_thread_id(new->thread.info);
    }
  
    UNLOCK();
  }

  if ( old )
  { *old = current;
  }

  return PL_ENGINE_SET;
}


PL_engine_t
PL_create_engine(PL_thread_attr_t *attributes)
{ PL_engine_t e, current;

  PL_set_engine(NULL, &current);
  if ( PL_thread_attach_engine(attributes) >= 0 )
  { e = PL_current_engine();
  } else
    e = NULL;

  PL_set_engine(current, NULL);

  return e;
}


int
PL_destroy_engine(PL_engine_t e)
{ int rc;

  if ( e == PL_current_engine() )
  { rc = PL_thread_destroy_engine();
  } else
  { PL_engine_t current;
    
    if ( PL_set_engine(e, &current) == PL_ENGINE_SET )
    { rc = PL_thread_destroy_engine();
      PL_set_engine(current, NULL);

    } else
      rc = FALSE;
  }

  return rc;
}


		 /*******************************
		 *	     STATISTICS		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
thread_statistics(+Thread, +Key, -Value)
    Same as statistics(+Key, -Value) but operates on another thread.

statistics(heapused, X) walks along  all   threads  and  therefore locks
L_THREAD. As it returns the same result   on  all threads we'll redirect
this to run on our own thread.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static
PRED_IMPL("thread_statistics", 3, thread_statistics, 0)
{ PL_thread_info_t *info;
  PL_local_data_t *ld;
  int rval;
  atom_t k;

  LOCK();
  if ( !get_thread(A1, &info, TRUE) )
  { UNLOCK();
    fail;
  }

  if ( !(ld=info->thread_data) )
  { UNLOCK();
    return PL_error(NULL, 0, NULL,
		    ERR_PERMISSION,
		    ATOM_statistics, ATOM_thread, A1);
  }

  if ( !PL_get_atom(A2, &k) )
    k = 0;

  if ( k == ATOM_heapused )
    ld = LD;
  else if ( k == ATOM_cputime || k == ATOM_runtime )
    ld->statistics.user_cputime = ThreadCPUTime(info, CPU_USER);
  else if ( k == ATOM_system_time )
    ld->statistics.system_cputime = ThreadCPUTime(info, CPU_SYSTEM);

  if ( LD == ld )		/* self: unlock first to avoid deadlock */
  { UNLOCK();
    return pl_statistics_ld(A2, A3, ld PASS_LD);
  }

  rval = pl_statistics_ld(A2, A3, ld PASS_LD);
  UNLOCK();

  return rval;
}


#ifdef WIN32

/* How to make the memory visible?
*/

					/* see also pl-nt.c */
#define nano * 0.0000001
#define ntick 1.0			/* manual says 100.0 ??? */

static double
ThreadCPUTime(PL_thread_info_t *info, int which)
{ double t;
  FILETIME created, exited, kerneltime, usertime;
  HANDLE win_thread = pthread_getw32threadhandle_np(info->tid);

  if ( GetThreadTimes(win_thread,
		      &created, &exited, &kerneltime, &usertime) )
  { FILETIME *p;

    if ( which == CPU_SYSTEM )
      p = &kerneltime;
    else
      p = &usertime;

    t = (double)p->dwHighDateTime * (4294967296.0 * ntick nano);
    t += (double)p->dwLowDateTime  * (ntick nano);

    return t;
  }

  return 0.0;
}

#else /*WIN32*/

#define timespec_to_double(ts) \
	(double)ts.tv_sec + (double)ts.tv_nsec/1000000000ull

#ifdef PTHREAD_CPUCLOCKS

static double
ThreadCPUTime(PL_thread_info_t *info, int which)
{ if ( info->tid )
  { clockid_t clock_id;
    struct timespec ts;

    pthread_getcpuclockid(info->tid, &clock_id);
    if (clock_gettime(clock_id, &ts) == 0)
      return timespec_to_double(ts);
  }

  return 0.0;
}

#else /*PTHREAD_CPUCLOCKS*/

#ifdef LINUX_CPUCLOCKS

static double
ThreadCPUTime(PL_thread_info_t *info, int which)
{
  struct timespec ts;

  if (syscall(__NR_clock_gettime, CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return timespec_to_double(ts);

  return 0.0;
}

#else /*LINUX_CPUCLOCKS*/

#ifdef LINUX_PROCFS

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Unfortunately POSIX threads does not define a   way  to get the CPU time
per thread. Some systems do have mechanisms  for that. POSIX does define
clock_gettime(CLOCK_THREAD_CPUTIME_ID), but it certainly doesn't work in
Linux 2.6.8.

Autoconf detects the presense of  /proc  and   we  read  the values from
there. This is rather slow. To make things not  too bad we use a pool of
open handles to entries in the /proc  table. All this junk should really
be moved into a file trying to implement  thread CPU time for as many as
possible platforms. If you happen to know  such a library, please let me
know.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <fcntl.h>

#define CACHED_PROCPS_ENTRIES 5

typedef struct
{ int tid;				/* process id */
  int fd;				/* file descriptor */
  int offset;				/* start of numbers */
  int usecoount;
} procps_entry;

static procps_entry procps_entries[CACHED_PROCPS_ENTRIES]; /* cached entries */

static void
close_procps_entry(procps_entry *e)
{ if ( e->tid )
  { close(e->fd);
    memset(e, 0, sizeof(*e));
  }
}


static procps_entry*
reclaim_procps_entry()
{ procps_entry *e, *low;
  int i; int lowc;

  low=e=procps_entries;
  lowc=low->usecoount;

  for(e++, i=1; i<CACHED_PROCPS_ENTRIES; e++, i++)
  { if ( e->usecoount < lowc )
    { lowc = e->usecoount;
      low = e;
    }
  }

  for(e=procps_entries, i=0; i<CACHED_PROCPS_ENTRIES; e++, i++)
    e->usecoount = 0;

  close_procps_entry(low);

  return low;
}


static procps_entry *
open_procps_entry(procps_entry *e, int tid)
{ char fname[256];
  int fd;

  sprintf(fname, "/proc/self/task/%d/stat", tid);
  if ( (fd=open(fname, O_RDONLY)) >= 0 )
  { char buffer[1000];
    int pos;

    pos = read(fd, buffer, sizeof(buffer)-1);
    if ( pos > 0 )
    { char *bp;

      buffer[pos] = EOS;
      if ( (bp=strrchr(buffer, ')')) )
      { e->tid = tid;
	e->fd = fd;
	e->offset = (bp-buffer)+4;
	e->usecoount = 1;
	
	return e;
      }
    }
  }

  return NULL;
}


static procps_entry*
get_procps_entry(int tid)
{ int i;
  procps_entry *e;

  for(e=procps_entries, i=0; i<CACHED_PROCPS_ENTRIES; e++, i++)
  { if ( e->tid == tid )
    { e->usecoount++;

      return e;
    }
  }

  for(e=procps_entries, i=0; i<CACHED_PROCPS_ENTRIES; e++, i++)
  { if ( e->tid == 0 )
      return open_procps_entry(e, tid);
  }

  e = reclaim_procps_entry();
  return open_procps_entry(e, tid);
}


static double
ThreadCPUTime(PL_thread_info_t *info, int which)
{ procps_entry *e;

  if ( (e=get_procps_entry(info->pid)) )
  { char buffer[1000];
    char *s;
    long long ticks;
    int i, n, nth = 10;			/* user time */

    if ( which == CPU_SYSTEM )
      nth++;

    lseek(e->fd, e->offset, SEEK_SET);
    n = read(e->fd, buffer, sizeof(buffer)-1);
    if ( n >= 0 )
      buffer[n] = EOS;
					/* most likely does not need reuse */
    if ( info->status != PL_THREAD_RUNNING )
      close_procps_entry(e);
    
    for(s=buffer, i=0; i<nth; i++, s++)
    { while(*s != ' ')
	s++;
    }

    ticks = atoll(s);
    return (double)ticks/100.0;
  }

  return 0.0;
}

#else /*LINUX_PROCFS*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Code that probably only works  on   Linux  2.4  systems, where CpuTime()
returns the per-thread time. This isn't very   nice as the time is store
in LD in addition to being returned, but it  is ok for now and Linux 2.4
is almost dead anyway.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
SyncUserCPU(int sig)
{ LD->statistics.user_cputime = CpuTime(CPU_USER);
  sem_post(sem_mark_ptr);
}


static void
SyncSystemCPU(int sig)
{ LD->statistics.system_cputime = CpuTime(CPU_SYSTEM);
  sem_post(sem_mark_ptr);
}


static double
ThreadCPUTime(PL_thread_info_t *info, int which)
{ if ( info->thread_data == LD )
  { return CpuTime(which);
  } else
  { struct sigaction old;
    struct sigaction new;

    sem_init(sem_mark_ptr, USYNC_THREAD, 0);
    memset(&new, 0, sizeof(new));
    if ( which == CPU_USER )
      new.sa_handler = SyncUserCPU;
    else 
      new.sa_handler = SyncSystemCPU;

    new.sa_flags   = SA_RESTART;
    sigaction(SIG_FORALL, &new, &old);
    if ( pthread_kill(info->tid, SIG_FORALL) == 0 )
    { while( sem_wait(sem_mark_ptr) == -1 && errno == EINTR )
	;
    }
    sem_destroy(&sem_mark);
    sigaction(SIG_FORALL, &old, NULL);

    if ( which == CPU_USER )
      return LD->statistics.user_cputime;
    else
      return LD->statistics.system_cputime;
  }
}

#endif /*LINUX_PROCFS*/
#endif /*LINUX_CPUCLOCKS*/
#endif /*PTHREAD_CPUCLOCKS*/
#endif /*WIN32*/


		 /*******************************
		 *	      ATOM-GC		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This  is  hairy  and  hard-to-port   code.    The   job   of  the  entry
threadMarkAtomsOtherThreads() is to mark the   atoms referenced from all
other threads. This function is  called from pl_garbage_collect_atoms(),
which already has locked the L_ATOM and L_THREAD mutexes.

We set up a semaphore and  signal   all  the  other threads. Each thread
receiving a the SIG_MARKATOMS signal calls markAtomsOnStacks() and posts
the semaphore. The latter performs its  job with certain heuristics, but
must ensure it doesn't  forget  any  atoms   (a  few  too  many  is ok).
Basically this signal handler can run whenever  necessary, as long as as
the thread is not in a GC,  which   makes  it impossible to traverse the
stacks.

Special attention is required  for   stack-creation  and destruction. We
should not be missing threads that  are   about  to be created or signal
them when they have just died. We   do this by locking the status-change
with the L_THREAD mutex, which is held by the atom-garbage collector, so
each starting thread will hold  until   collection  is complete and each
terminating one will live a bit longer until atom-GC is complete.

After a thread is done marking its atom  is just continues. This is safe
as it may stop referencing atoms but   this  doesn't matter. It can only
refer to new atoms by creating them, in which case the thread will block
or by executing an instruction that refers to the atom. In this case the
atom is locked by the instruction anyway.

[WIN32]  The  windows  case  is  entirely    different  as  we  have  no
asynchronous signals. Fortunately we  can   suspend  and resume threads.
This makes the code a lot easier as   you can see below. Problem is that
only one processor is doing the  job,   where  atom-gc  is a distributed
activity in the POSIX based code.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef WIN32

void					/* For comments, see above */
forThreadLocalData(void (*func)(PL_local_data_t *), unsigned flags)
{ int i;
  int me = PL_thread_self();

  for(i=0; i<MAX_THREADS; i++)
  { if ( threads[i].thread_data && i != me &&
	 threads[i].status == PL_THREAD_RUNNING )
    { HANDLE win_thread = pthread_getw32threadhandle_np(threads[i].tid);

      if ( SuspendThread(win_thread) != -1L )
      { (*func)(threads[i].thread_data);
        if ( (flags & PL_THREAD_SUSPEND_AFTER_WORK) )
	  threads[i].status = PL_THREAD_SUSPENDED;
	else
	  ResumeThread(win_thread);
      }
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Resume all suspended threads.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
resumeThreads(void)
{ int i;
  int me = PL_thread_self();

  for(i=0; i<MAX_THREADS; i++)
  { if ( threads[i].thread_data && i != me &&
	 threads[i].status == PL_THREAD_SUSPENDED )
    { HANDLE win_thread = pthread_getw32threadhandle_np(threads[i].tid);

      ResumeThread(win_thread);
      threads[i].status = PL_THREAD_RUNNING;
    }
  }
}

#else /*WIN32*/

static void (*ldata_function)(PL_local_data_t *data);

static void
wait_resume(PL_thread_info_t *t)
{ sigset_t signal_set;

  sigfillset(&signal_set);
  sigdelset(&signal_set, SIG_RESUME);
  do
  { sigsuspend(&signal_set);
  } while(t->status != PL_THREAD_RESUMING);
  t->status = PL_THREAD_RUNNING;

  DEBUG(1, Sdprintf("Resuming %d\n", t-threads));
}


static void
resume_handler(int sig)
{ sem_post(sem_mark_ptr);
}


void
resumeThreads(void)
{ struct sigaction old;
  struct sigaction new;
  int i;
  PL_thread_info_t *t;
  int signalled = 0;

  memset(&new, 0, sizeof(new));
  new.sa_handler = resume_handler;
  new.sa_flags   = SA_RESTART;
  sigaction(SIG_RESUME, &new, &old);

  sem_init(sem_mark_ptr, USYNC_THREAD, 0);

  for(t = threads, i=0; i<MAX_THREADS; i++, t++)
  { if ( t->status == PL_THREAD_SUSPENDED )
    { int rc;

      t->status = PL_THREAD_RESUMING;

      DEBUG(1, Sdprintf("Sending SIG_RESUME to %d\n", i));
      if ( (rc=pthread_kill(t->tid, SIG_RESUME)) == 0 )
	signalled++;
      else
	Sdprintf("resumeThreads(): Failed to signal %d: %s\n", i, ThError(rc));
    }
  }

  while(signalled)
  { while(sem_wait(sem_mark_ptr) == -1 && errno == EINTR)
      ;
    signalled--;
  }
  sem_destroy(&sem_mark);

  sigaction(SIG_RESUME, &old, NULL);
}


#if 0					/* don't need it right now */
#ifdef HAVE_EXECINFO_H
#define BACKTRACE 1

#if BACKTRACE
#include <execinfo.h>
#include <string.h>

static void
print_trace (void)
{ void *array[5];
  size_t size;
  char **strings;
  size_t i;
     
  size = backtrace(array, sizeof(array)/sizeof(void *));
  strings = backtrace_symbols(array, size);
     
  Sdprintf("C-stack:\n");
  
  for(i = 0; i < size; i++)
  { Sdprintf("\t[%d] %s\n", i, strings[i]);
  }
       
  free(strings);
}
#endif /*BACKTRACE*/
#endif /*HAVE_EXECINFO_H*/
#endif /*O_DEBUG*/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
doThreadLocalData()

Does the signal  handling  to  deal   with  asynchronous  inspection  if
thread-local  data.  It  currently    assumes  pthread_getspecific()  is
async-signal-safe, which is  not  guaranteed.  It   is  adviced  to  use
__thread classified data to deal  with   thread  identity for this case.
Must be studied.  See also

https://listman.redhat.com/archives/phil-list/2003-December/msg00042.html

Note that the use of info->ldata_status   is actually not necessary, but
it largely simplifies debugging if not all doThreadLocalData() do answer
for whathever reason.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
doThreadLocalData(int sig)
{ PL_local_data_t *ld;
  PL_thread_info_t *info;

  ld = LD;
  info = ld->thread.info;

  info->ldata_status = LDATA_ANSWERING;

  (*ldata_function)(ld);

  if ( ld->thread.forall_flags & PL_THREAD_SUSPEND_AFTER_WORK )
  { DEBUG(1, Sdprintf("\n\tDone work on %d; suspending ...",
		      info->pl_tid));
    
    info->status = PL_THREAD_SUSPENDED;
    sem_post(sem_mark_ptr);
    wait_resume(info);
  } else
  { DEBUG(1, Sdprintf("\n\tDone work on %d", info->pl_tid));
    sem_post(sem_mark_ptr);
  }

  info->ldata_status = LDATA_ANSWERED;
}


void
forThreadLocalData(void (*func)(PL_local_data_t *), unsigned flags)
{ struct sigaction old;
  struct sigaction new;
  int me = PL_thread_self();
  int signalled = 0;
  PL_thread_info_t *th;
  sigset_t sigmask;

  DEBUG(1, Sdprintf("Calling forThreadLocalData() from %d\n", me));

  assert(ldata_function == NULL);
  ldata_function = func;

  if ( sem_init(sem_mark_ptr, USYNC_THREAD, 0) != 0 )
  { perror("sem_init");
    exit(1);
  }

  allSignalMask(&sigmask);
  memset(&new, 0, sizeof(new));
  new.sa_handler = doThreadLocalData;
  new.sa_flags   = SA_RESTART;
  new.sa_mask    = sigmask;
  sigaction(SIG_FORALL, &new, &old);

  for(th = &threads[1]; th < &threads[MAX_THREADS]; th++)
  { if ( th->thread_data && th->pl_tid != me &&
	 th->status == PL_THREAD_RUNNING )
    { int rc;

      DEBUG(1, Sdprintf("Signalling %d\n", th->pl_tid));
      th->thread_data->thread.forall_flags = flags;
      th->ldata_status = LDATA_SIGNALLED;
      if ( (rc=pthread_kill(th->tid, SIG_FORALL)) == 0 )
      { signalled++;
      } else if ( rc != ESRCH )
	Sdprintf("forThreadLocalData(): Failed to signal: %s\n", ThError(rc));
    }
  }

  DEBUG(1, Sdprintf("Signalled %d threads.  Waiting ... ", signalled));

  while(signalled)
  { if ( sem_wait(sem_mark_ptr) == 0 )
    { DEBUG(1, Sdprintf(" (ok)"));
      signalled--;
    } else if ( errno != EINTR )
    { perror("sem_wait");
      exit(1);
    }
  }

  sem_destroy(&sem_mark);
  for(th = &threads[1]; th < &threads[MAX_THREADS]; th++)
    th->ldata_status = LDATA_IDLE;

  DEBUG(1, Sdprintf(" All done!\n"));

  sigaction(SIG_FORALL, &old, NULL);

  assert(ldata_function == func);
  ldata_function = NULL;
}

#endif /*WIN32*/

		 /*******************************
		 *	    PREDICATES		*
		 *******************************/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
localiseDefinition(Definition def)
    Create a thread-local definition for the predicate `def'.

    This function is called from getProcDefinition() if the procedure is
    not yet `localised'.  Calling this function must be guarded by the
    L_PREDICATE mutex.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifndef offsetof
#define offsetof(structure, field) ((int) &(((structure *)NULL)->field))
#endif

static void
registerLocalDefinition(Definition def)
{ DefinitionChain cell = allocHeap(sizeof(*cell));

  cell->definition = def;
  cell->next = LD->thread.local_definitions;
  LD->thread.local_definitions = cell;
}


Definition
localiseDefinition(Definition def)
{ Definition local = allocHeap(sizeof(*local));
  int id = LD->thread.info->pl_tid;

  *local = *def;
  local->codes = NULL;			/* TBD: dynamic supervisor */
  local->mutex = NULL;
  clear(local, P_THREAD_LOCAL);		/* remains DYNAMIC */
  local->definition.clauses = NULL;
  local->hash_info = NULL;
  
  if ( !def->definition.local ||
       id >= def->definition.local->size )
  { int newsize = def->definition.local ? def->definition.local->size : 1;
    LocalDefinitions new;
    int bytes;
    int i=0;

    do
    { newsize *= 2;
    } while ( newsize <= id );
     
    bytes = offsetof(struct local_definitions, thread[newsize]);
    new = allocHeap(bytes);
    new->size = newsize;
    if ( def->definition.local )
    { for(; i<def->definition.local->size; i++)
	new->thread[i] = def->definition.local->thread[i];
    }
    for(; i<newsize; i++)
      new->thread[i] = NULL;
    if ( def->definition.local )
      freeHeap(def->definition.local,
	       offsetof(struct local_definitions,
			thread[def->definition.local->size]));
    def->definition.local = new;
  }

  def->definition.local->thread[id] = local;
  registerLocalDefinition(def);

  return local;
}


static void
cleanupLocalDefinitions(PL_local_data_t *ld)
{ DefinitionChain ch = ld->thread.local_definitions;
  DefinitionChain next;
  int id = ld->thread.info->pl_tid;

  for( ; ch; ch = next)
  { Definition local, def = ch->definition;
    next = ch->next;
    
    assert(true(def, P_THREAD_LOCAL));
    LOCKDEF(def);
    local = def->definition.local->thread[id];
    def->definition.local->thread[id] = NULL;
    UNLOCKDEF(def);

    destroyDefinition(local);

    freeHeap(ch, sizeof(*ch));
  }
}


		 /*******************************
		 *	DEBUGGING SUPPORT	*
		 *******************************/

PL_local_data_t *
_LD()
{ PL_local_data_t *ld = ((PL_local_data_t *)TLD_get(PL_ldata));
  return ld;
}

PL_local_data_t *
_LDN(int n)
{ return threads[n].thread_data;
}

#undef lBase
LocalFrame
lBase()
{ return (LD->stacks.local.base);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This function is called from  GNU  <assert.h>,   so  we  can print which
thread caused the problem. If the thread is   not the main one, we could
try to recover!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
__assert_fail(const char *assertion,
	      const char *file,
	      unsigned int line,
	      const char *function)
{ Sdprintf("[Thread %d] %s:%d: %s: Assertion failed: %s\n",
	   PL_thread_self(),
	   file, line, function, assertion);
  abort();
}

#else /*O_PLMT*/

#define pl_mutex_lock(mutex)
#define pl_mutex_unlock(mutex)

int
PL_thread_self()
{ return -2;
}

int
PL_thread_at_exit(void (*function)(void *), void *closure, int global)
{ return FALSE;
}

int
PL_thread_attach_engine(PL_thread_attr_t *attr)
{ return -2;
}

int
PL_thread_destroy_engine()
{ return FALSE;
}

#ifdef WIN32
int
PL_w32thread_raise(DWORD id, int sig)
{ return PL_raise(sig);
}
#endif

foreign_t
pl_thread_self(term_t id)
{ return PL_unify_atom(id, ATOM_main);
}

PL_engine_t
PL_current_engine(void)
{ return LD;
}

int
PL_set_engine(PL_engine_t new, PL_engine_t *old)
{ if ( new != LD && new != PL_ENGINE_MAIN )
    return PL_ENGINE_INVAL;

  if ( old )
  { *old = LD;
  }

  return PL_ENGINE_SET;
}

PL_engine_t
PL_create_engine(PL_thread_attr_t *attributes)
{ return NULL;
}

int
PL_destroy_engine(PL_engine_t e)
{ fail;
}


void
initPrologThreads()
{					/* TBD: only once? */
#ifdef O_MULTIPLE_ENGINES
  PL_current_engine_ptr = &PL_local_data;
#endif
}

#endif  /*O_PLMT*/

		 /*******************************
		 *	    WITH-MUTEX		*
		 *******************************/

foreign_t
pl_with_mutex(term_t mutex, term_t goal)
{ term_t ex = 0;
  int rval;

  pl_mutex_lock(mutex);
  rval = callProlog(NULL, goal, PL_Q_CATCH_EXCEPTION, &ex);
  pl_mutex_unlock(mutex);

  if ( !rval && ex )
  { SECURE(checkData(valTermRef(ex)));
    PL_raise_exception(ex);
  }

  return rval;
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(thread)
#ifdef O_PLMT
  PRED_DEF("thread_detach", 1, thread_detach, 0)
  PRED_DEF("thread_statistics", 3, thread_statistics, 0)
  PRED_DEF("message_queue_create", 1, message_queue_create, 0)
  PRED_DEF("thread_get_message", 2, thread_get_message, 0)
  PRED_DEF("thread_peek_message", 2, thread_peek_message, 0)
  PRED_DEF("message_queue_destroy", 1, message_queue_destroy, 0)
  PRED_DEF("thread_setconcurrency", 2, thread_setconcurrency, 0)
  PRED_DEF("mutex_statistics", 0, mutex_statistics, 0)
  PRED_DEF("message_queue_size", 2, message_queue_size, 0)
#endif
EndPredDefs

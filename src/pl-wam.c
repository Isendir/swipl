/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        wielemak@science.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2007, University of Amsterdam

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

/*#define O_SECURE 1*/
/*#define O_DEBUG 1*/
#include "pl-incl.h"

#define	     BFR (LD->choicepoints)	/* choicepoint registration */

#if sun
#include <prof.h>			/* in-function profiling */
#else
#define MARK(label)
#endif

static Choice	newChoice(choice_type type, LocalFrame fr ARG_LD);

#if COUNTING

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The counting code has been added   while investigating the time critical
WAM  instructions.  The  current  implementation  runs  on  top  of  the
information  provided  by  code_info   (from    pl-comp.c)   and  should
automatically addapt to modifications in the VM instruction set.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct
{ code  code;
  int	times;
  int  *vartimesptr;
} count_info;

#define MAXVAR 8

static count_info counting[I_HIGHEST];

static void
count(code c, Code PC)
{ const code_info *info = &codeTable[c];

  counting[c].times++;
  switch(info->argtype)
  { case CA1_VAR:
    { int v = (int)*PC;
      
      v -= ARGOFFSET/sizeof(word);
      assert(v>=0);
      if ( v >= MAXVAR )
	v = MAXVAR-1;

      if ( !counting[c].vartimesptr )
      { int bytes = sizeof(int)*MAXVAR;

	counting[c].vartimesptr = allocHeap(bytes);
	memset(counting[c].vartimesptr, 0, bytes);
      }
      counting[c].vartimesptr[v]++;
    }
  }
}


static void
countHeader()
{ int m;
  int amax = MAXVAR;
  char last[20];

  Sfprintf(Scurout, "%-13s %8s ", "Instruction", "times");
  for(m=0; m < amax-1; m++)
    Sfprintf(Scurout, " %8d", m);
  Ssprintf(last, ">%d", m);
  Sfprintf(Scurout, " %8s\n", last);
  for(m=0; m<(31+amax*8); m++)
    Sputc('=', Scurout);
  Sfprintf(Scurout, "\n");
}  


static int
cmpcounts(const void *p1, const void *p2)
{ const count_info *c1 = p1;
  const count_info *c2 = p2;
  
  return c2->times - c1->times;
}


word
pl_count()
{ int i;
  count_info counts[I_HIGHEST];
  count_info *c;

  countHeader();

  memcpy(counts, counting, sizeof(counts));
  for(i=0, c=counts; i<I_HIGHEST; i++, c++)
    c->code = i;
  qsort(counts, I_HIGHEST, sizeof(count_info), cmpcounts);

  for(c = counts, i=0; i<I_HIGHEST; i++, c++)
  { const code_info *info = &codeTable[c->code];

    Sfprintf(Scurout, "%-13s %8d ", info->name, c->times);
    if ( c->vartimesptr )
    { int n, m=MAXVAR;

      while(m>0 && c->vartimesptr[m-1] == 0 )
	m--;
      for(n=0; n<m; n++)
	Sfprintf(Scurout, " %8d", c->vartimesptr[n]);
    }
    Sfprintf(Scurout, "\n");
  }

  succeed;
}

#else /* ~COUNTING */

#define count(id, pc)			/* no debugging not counting */

#endif /* COUNTING */

		 /*******************************
		 *	     DEBUGGING		*
		 *******************************/

#if defined(O_DEBUG) || defined(SECURE_GC) || defined(O_MAINTENANCE)
static inline intptr_t
loffset(void *p)
{ if ( p == NULL )
    return 0;

  assert((intptr_t)p % sizeof(word) == 0);
  return (Word)p-(Word)lBase;
}
#endif

#ifdef O_DEBUG

static void
DbgPrintInstruction(LocalFrame FR, Code PC)
{ static LocalFrame ofr = NULL;

  DEBUG(3,
	if ( ofr != FR )
	{ Sfprintf(Serror, "#%ld at [%ld] predicate %s\n",
		   loffset(FR),
		   levelFrame(FR),
		   predicateName(FR->predicate));
	  ofr = FR;
	});

  DEBUG(3, wamListInstruction(Serror, FR->clause->clause, PC));
}

#else

#define DbgPrintInstruction(fr, pc)

#endif




#include "pl-alloc.c"
#include "pl-index.c"

		 /*******************************
		 *	    ASYNC HOOKS		*
		 *******************************/

#if O_ASYNC_HOOK

static struct
{ PL_async_hook_t	hook;		/* the hook function */
  unsigned int		mask;		/* the mask */
} async;


PL_async_hook_t
PL_async_hook(unsigned int count, PL_async_hook_t hook)
{ PL_async_hook_t old = async.hook;

  async.hook = hook;
  async.mask = 1;
  while(async.mask < count)
    async.mask <<= 1;
  async.mask--;

  return old;
}


#endif /*O_ASYNC_HOOK*/

		 /*******************************
		 *	     SIGNALS		*
		 *******************************/

#if 0 /*def O_SAFE_SIGNALS*/

static inline int
is_signalled()
{ sigset_t set;

  sigpending(&set);

  return set != 0;			/* non-portable! */
}

#else

static inline int
is_signalled(ARG1_LD)
{
#ifdef O_PLMT
  if ( LD->cancel_counter++ % 64 == 0 )
    pthread_testcancel();
#endif

  return (LD->pending_signals != 0);
}

#endif


		 /*******************************
		 *	   STACK-LAYOUT		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Brief description of the local stack-layout.  This stack contains:

	* struct localFrame structures for the Prolog stackframes.
	* argument vectors and local variables for Prolog goals.
	* choice-points (struct choice)
	* term-references for foreign code.  The layout:


	lTop  -->| first free location |
		 -----------------------
		 | local variables     |
		 |        ...	       |
		 | arguments for goal  |
		 | localFrame struct   |
		 | queryFrame struct   |
		 -----------------------
		 |        ...	       |
		 | term-references     |
		 -----------------------
	lBase -->| # fliFrame struct   |
		 -----------------------

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */


		 /*******************************
		 *	    FOREIGN FRAME	*
		 *******************************/

#undef LD
#define LD LOCAL_LD

static fid_t
open_foreign_frame(ARG1_LD)
{ FliFrame fr = (FliFrame) lTop;

  requireStack(local, sizeof(struct fliFrame));
  lTop = addPointer(lTop, sizeof(struct fliFrame));
  fr->size = 0;
  Mark(fr->mark);
  fr->parent = fli_context;
  fr->magic = FLI_MAGIC;
  fli_context = fr;

  return consTermRef(fr);
}


static void
close_foreign_frame(fid_t id ARG_LD)
{ FliFrame fr = (FliFrame) valTermRef(id);

  assert(fr->magic == FLI_MAGIC);
  fr->magic = FLI_MAGIC_CLOSED;
  fli_context = fr->parent;
  lTop = (LocalFrame) fr;
}


fid_t
PL_open_foreign_frame()
{ GET_LD

  return open_foreign_frame(PASS_LD1);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Open a foreign frame to handle a signal.  We must skip MAXARITY words to
deal with the fact that the WAM write-mode   writes above the top of the
stack.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

fid_t
PL_open_signal_foreign_frame()
{ GET_LD
  FliFrame fr;
  size_t margin = sizeof(struct localFrame) + MAXARITY*sizeof(word);

  requireStack(local, sizeof(struct fliFrame)+margin);
  lTop = addPointer(lTop, margin);
  fr = (FliFrame) lTop;

  fr->magic = FLI_MAGIC;
  fr->size = 0;
  Mark(fr->mark);
  fr->parent = fli_context;
  lTop = (LocalFrame)(fr+1);
  fli_context = fr;

  return consTermRef(fr);
}


void
PL_close_foreign_frame(fid_t id)
{ GET_LD
  
  close_foreign_frame(id PASS_LD);
}

#define PL_open_foreign_frame()    open_foreign_frame(PASS_LD1)
#define PL_close_foreign_frame(id) close_foreign_frame((id) PASS_LD)

void
PL_rewind_foreign_frame(fid_t id)
{ GET_LD
  FliFrame fr = (FliFrame) valTermRef(id);

  Undo(fr->mark);
  lTop = addPointer(fr, sizeof(struct fliFrame));
  fr->size = 0;
}


void
PL_discard_foreign_frame(fid_t id)
{ GET_LD
  FliFrame fr = (FliFrame) valTermRef(id);

  DEBUG(8, Sdprintf("Discarding foreign frame %p\n", fr));
  fli_context = fr->parent;
  Undo(fr->mark);
  lTop = (LocalFrame) fr;
}

		/********************************
		*         FOREIGN CALLS         *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Calling foreign predicates.  We will have to  set  `lTop',  compose  the
argument  vector  for  the  foreign  function,  call  it and analyse the
result.  The arguments of the frame are derefenced  here  to  avoid  the
need for explicit dereferencing in most foreign predicates themselves.

A non-deterministic foreign predicate  can   return  either the constant
FALSE  to  start  backtracking,  TRUE    to   indicate  success  without
alternatives or anything  else.  The  return   value  is  saved  in  the
choice-point that is  created  after   return  of  the non-deterministic
foreign function. On `redo', the  foreign   predicate  is  called with a
control_t argument that indicates the context   value and the reason for
the call-back.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define MAX_FLI_ARGS 10			/* extend switches on change */

#define CALLDETFN(r, argc) \
  { switch(argc) \
    { case 0: \
	r = F(); \
        break; \
      case 1: \
	r = F(A(0)); \
	break; \
      case 2: \
	r = F(A(0),A(1)); \
        break; \
      case 3: \
	r = F(A(0),A(1),A(2)); \
        break; \
      case 4: \
	r = F(A(0),A(1),A(2),A(3)); \
        break; \
      case 5: \
	r = F(A(0),A(1),A(2),A(3),A(4)); \
        break; \
      case 6: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5)); \
        break; \
      case 7: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6)); \
        break; \
      case 8: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7)); \
        break; \
      case 9: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8)); \
        break; \
      case 10: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9)); \
        break; \
      default: \
	r = sysError("Too many arguments to foreign function (>%d)", \
		     MAX_FLI_ARGS); \
    } \
  }

#define CALLNDETFN(r, argc, c) \
  { switch(argc) \
    { case 0: \
	r = F(c); \
        break; \
      case 1: \
	r = F(A(0),(c)); \
	break; \
      case 2: \
	r = F(A(0),A(1),(c)); \
        break; \
      case 3: \
	r = F(A(0),A(1),A(2),(c)); \
        break; \
      case 4: \
	r = F(A(0),A(1),A(2),A(3),(c)); \
        break; \
      case 5: \
	r = F(A(0),A(1),A(2),A(3),A(4),(c)); \
        break; \
      case 6: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),(c)); \
        break; \
      case 7: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6),(c)); \
        break; \
      case 8: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),(c)); \
        break; \
      case 9: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),(c)); \
        break; \
      case 10: \
	r = F(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),(c)); \
        break; \
      default: \
	r = sysError("Too many arguments to foreign function (>%d)", \
		     MAX_FLI_ARGS); \
    } \
  }


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  * We are after a `normal call', so we have MAXARITY free cells on the
    local stack

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define F (*function)    
#define A(n) (h0+n)

static bool
callForeign(LocalFrame frame, frg_code control ARG_LD)
{ Definition def = frame->predicate;
  Func function = def->definition.function;
  int argc = def->functor->arity;
  word result;
  term_t h0 = argFrameP(frame, 0) - (Word)lBase;
  FliFrame ffr;

#ifdef O_DEBUGGER
retry:
  if ( debugstatus.debugging )
  { int port = (control == FRG_FIRST_CALL ? CALL_PORT : REDO_PORT);

    lTop = (LocalFrame)argFrameP(frame, argc);

    switch( tracePort(frame, LD->choicepoints, port, NULL PASS_LD) )
    { case ACTION_FAIL:
	exception_term = 0;
	fail;
      case ACTION_IGNORE:
	exception_term = 0;
	succeed;
      case ACTION_RETRY:
	exception_term = 0;
	control = FRG_FIRST_CALL;
	frame->clause = NULL;
    }
  }
#endif /*O_DEBUGGER*/

					/* open foreign frame */
  ffr  = (FliFrame)argFrameP(frame, argc);
  lTop = addPointer(ffr, sizeof(struct fliFrame));
  ffr->magic = FLI_MAGIC;
  ffr->size = 0;
  Mark(ffr->mark);
  ffr->parent = fli_context;
  fli_context = ffr;

  SECURE({ int n;
	   Word p0 = argFrameP(frame, 0);
	   
	   for(n=0; n<argc; n++)
	     checkData(p0+n);
	 });

					/* do the call */
  { SaveLocalPtr(fid, frame);
    SaveLocalPtr(cid, ffr);

    if ( true(def, P_VARARG) )
    { struct foreign_context context;
  
      context.context = (word)frame->clause;
      context.engine  = LD;
      context.control = control;
  
      frame->clause = NULL;
      result = F(h0, argc, &context);
    } else
    { if ( false(def, NONDETERMINISTIC) )
      { CALLDETFN(result, argc);
      } else
      { struct foreign_context context;
  
	context.context = (word)frame->clause;
	context.engine  = LD;
	context.control = control;

	frame->clause = NULL;
	CALLNDETFN(result, argc, &context);
      }
    }
    
    RestoreLocalPtr(fid, frame);
    RestoreLocalPtr(cid, ffr);
  }

  if ( exception_term )			/* EXCEPTION */
  { frame->clause = NULL;		/* no discardFrame() needed */

    if ( result )			/* No, false alarm */
    { exception_term = 0;
      setVar(*valTermRef(exception_bin));
    } else
    { mark m = ffr->mark;
      Choice ch;

      fli_context = ffr->parent;
      lTop = (LocalFrame)ffr;
      ch = newChoice(CHP_DEBUG, frame PASS_LD);
      ch->mark = m;

      return FALSE;
    }
  }

#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { int port = (result ? EXIT_PORT : FAIL_PORT);

    if ( port == FAIL_PORT )
    { Undo(ffr->mark);
    }

    switch( tracePort(frame, LD->choicepoints, port, NULL PASS_LD) )
    { case ACTION_FAIL:
	exception_term = 0;
        fail;
      case ACTION_IGNORE:
	exception_term = 0;
        succeed;
      case ACTION_RETRY:
	Undo(ffr->mark);
	control = FRG_FIRST_CALL;
        frame->clause = NULL;
	fli_context = ffr->parent;
	exception_term = 0;
	goto retry;
    }
  }
#endif

					/* deterministic result */
  if ( result == TRUE || result == FALSE )
  { fli_context = ffr->parent;
    return (int)result;
  }

  if ( true(def, NONDETERMINISTIC) )
  { mark m = ffr->mark;
    Choice ch;

    if ( (result & FRG_REDO_MASK) == REDO_INT )
    {					/* must be a signed shift */
      result = (word)(((intptr_t)result)>>FRG_REDO_BITS);
    } else
      result &= ~FRG_REDO_MASK;

    fli_context = ffr->parent;
    ch = (Choice)ffr;
    lTop = addPointer(ch, sizeof(*ch));

					/* see newChoice() */
    ch->type = CHP_FOREIGN;
    ch->frame = frame;
    ch->parent = BFR;
    ch->mark = m;
    ch->value.foreign = result;
#ifdef O_PROFILE
    ch->prof_node = LD->profile.current;
#endif
    BFR = ch;

    frame->clause = (ClauseRef)result; /* for discardFrame() */
    
    return TRUE;
  } else				/* illegal return */
  { FunctorDef fd = def->functor;
    term_t ex = PL_new_term_ref();

    PL_put_intptr(ex, result);

    PL_error(stringAtom(fd->name), fd->arity, NULL, ERR_DOMAIN,
	     ATOM_foreign_return_value, ex);
    fli_context = ffr->parent;
    return FALSE;
  }
}
#undef A
#undef F


static void
discardForeignFrame(LocalFrame fr ARG_LD)
{ Definition def = fr->predicate;
  int argc       = def->functor->arity;
  Func function  = def->definition.function;
  struct foreign_context context;
  word result;
  fid_t fid;

#define F	(*function)
#define A(n)	0

  DEBUG(5, Sdprintf("\tCut %s, context = 0x%lx\n",
		    predicateName(def), context));

  context.context = (word)fr->clause;
  context.control = FRG_CUTTED;
  context.engine  = LD; 

  fid = PL_open_foreign_frame();
  if ( true(def, P_VARARG) )
  { result = F(0, argc, &context);
  } else
  { CALLNDETFN(result, argc, &context);
  }
  PL_close_foreign_frame(fid);

#undef A
#undef F
}


enum finished
{ FINISH_EXIT = 0,
  FINISH_FAIL,
  FINISH_CUT,
  FINISH_EXCEPT,
  FINISH_EXITCLEANUP
};


static int
unify_finished(term_t catcher, enum finished reason)
{ GET_LD

  static atom_t reasons[] = 
  { ATOM_exit,
    ATOM_fail,
    ATOM_cut,
    ATOM_exception,
    ATOM_exit
  };

  if ( reason == FINISH_EXCEPT )
  { SECURE(checkData(valTermRef(exception_bin)));

    return PL_unify_term(catcher,
			 PL_FUNCTOR, FUNCTOR_exception1,
			   PL_TERM, exception_bin);
  } else if ( reason == FINISH_EXIT )
  { fail;
  } else
  { return PL_unify_atom(catcher, reasons[reason]);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
frameFinished() is used for two reasons:   providing hooks for the (GUI)
debugger  for  updating   the   stack-view    and   for   dealing   with
call_cleanup/3. Both may call-back the Prolog engine, but in general the
system is not in a state where we can do garbage collection.

As a consequence the cleanup-handler  of   call_cleanup()  runs  with GC
disables and so do the callEventHook()  hooks.   The  latter is merely a
developers issue. Cleanup seems reasonable too.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
callCleanupHandler(LocalFrame fr, enum finished reason ARG_LD)
{ if ( fr->predicate == PROCEDURE_setup_and_call_cleanup4->definition &&
       false(fr, FR_CATCHED) )		/* from handler */
  { fid_t cid  = PL_open_foreign_frame();
    term_t catcher = argFrameP(fr, 2) - (Word)lBase;

    set(fr, FR_CATCHED);
    if ( unify_finished(catcher, reason) )
    { term_t clean = argFrameP(fr, 3) - (Word)lBase;
      term_t ex;
      int rval;
      
      blockGC(PASS_LD1);
      if ( reason == FINISH_EXCEPT )
      {	term_t pending = PL_new_term_ref();

	*valTermRef(pending) = *valTermRef(exception_bin);

	exception_term = 0;
	*valTermRef(exception_bin) = 0;
	rval = callProlog(fr->context, clean, PL_Q_CATCH_EXCEPTION, &ex);
	if ( rval || !ex )
	{ *valTermRef(exception_bin) = *valTermRef(pending);
	  exception_term = exception_bin;
	}
      } else
      { rval = callProlog(fr->context, clean, PL_Q_CATCH_EXCEPTION, &ex);
      }
      unblockGC(PASS_LD1);

      if ( !rval && ex )
	PL_raise_exception(ex);
    }
    
    PL_close_foreign_frame(cid);
  }
}


static void
frameFinished(LocalFrame fr, enum finished reason ARG_LD)
{ fid_t cid;

  callCleanupHandler(fr, reason PASS_LD);

#ifdef O_DEBUGGER
  cid  = PL_open_foreign_frame();
  callEventHook(PLEV_FRAMEFINISHED, fr);
  PL_close_foreign_frame(cid);
#endif

}

#ifdef O_DESTRUCTIVE_ASSIGNMENT

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Trailing of destructive assignments. This feature   is  used by setarg/3
and put_attr/2.

Such an assignment is trailed by first  pushing the assigned address (as
normal) and then pushing a marked pointer to  a cell on the global stack
holding the old (overwritten) value.

Undo is slightly more complicated as it has to check for these special
cells on the trailstack.

The garbage collector has to take care in  a number of places: it has to
pass through the trail-stack, marking   the  global-stack references for
assigned data and the sweep_trail() must be   careful about this type of
marks.

Note this function doesn't call Trail() for   the address as it can only
be called from setarg/3 and the argument  is thus always a term-argument
on the global stack.

(*) Enabling this test triggers an asserion error in unifiable/3. In any
case, we need tighter assignment of  LD->mark_bar as foreign frames that
surround each foreign predicate currently creates a mark.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
TrailAssignment__LD(Word p ARG_LD)
{ /*if ( p < LD->mark_bar )  see (*) */
  { Word old = allocGlobal(1);

    assert(!(*p & (MARK_MASK|FIRST_MASK)));
    *old = *p;				/* save the old value on the global */
    requireStack(trail, 2*sizeof(struct trail_entry));
    (tTop++)->address = p;
    (tTop++)->address = tagTrailPtr(old);
  }
}


static inline void
__do_undo(mark *m ARG_LD)
{ TrailEntry tt = tTop;
  TrailEntry mt = m->trailtop;

  while(--tt >= mt)
  { Word p = tt->address;

    if ( isTrailVal(p) )
    { DEBUG(2, Sdprintf("Undoing a trailed assignment\n"));
      tt--;
      *tt->address = trailVal(p);
      assert(!(*tt->address & (MARK_MASK|FIRST_MASK)));
    } else
      setVar(*p);
  }

  tTop = mt;
  if ( LD->frozen_bar > m->globaltop )
  { SECURE(assert(gTop >= LD->frozen_bar));
    gTop = LD->frozen_bar;
  } else
  { gTop = m->globaltop;
  }
}


void
do_undo(mark *m)
{ GET_LD
  __do_undo(m PASS_LD);
}

#undef Undo
#define Undo(m) __do_undo(&m PASS_LD)
#endif /*O_DESTRUCTIVE_ASSIGNMENT*/


		 /*******************************
		 *	    PROCEDURES		*
		 *******************************/

static inline Definition
pl__getProcDefinition(Procedure proc ARG_LD)
{
#ifdef O_PLMT
  Definition def = proc->definition;

  if ( true(def, P_THREAD_LOCAL) )
  { int i = LD->thread.info->pl_tid;
    Definition local;

    LOCKDEF(def);
    if ( !def->definition.local ||
	 i >= def->definition.local->size ||
	 !(local=def->definition.local->thread[i]) )
      local = localiseDefinition(def);
    UNLOCKDEF(def);

    return local;
  }

  return def;
#else
  return proc->definition;
#endif
}


Definition
getProcDefinition(Procedure proc)
{ GET_LD

  return pl__getProcDefinition(proc PASS_LD);
}

#define getProcDefinition(proc) pl__getProcDefinition(proc PASS_LD)


static inline Definition
getProcDefinedDefinition(LocalFrame *frp, Code PC, Procedure proc ARG_LD)
{ Definition def = proc->definition;

  if ( !def->definition.clauses && false(def, PROC_DEFINED) )
    def = trapUndefined(frp, PC, proc PASS_LD);

#ifdef O_PLMT
  if ( true(def, P_THREAD_LOCAL) )
  { int i = LD->thread.info->pl_tid;
    Definition local;

    LOCKDEF(def);
    if ( !def->definition.local ||
	 i >= def->definition.local->size ||
	 !(local=def->definition.local->thread[i]) )
      local = localiseDefinition(def);
    UNLOCKDEF(def);

    return local;
  }

  return def;
#else
  return def;
#endif
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
foreignWakeup() calls delayed goals while executing a foreign procedure.
Note that the  choicepoints  of  the   awoken  code  are  destroyed  and
therefore this code can only be used in places introducing an (implicit)
cut such as \=/2 (implemented as A \= B :- ( A = B -> fail ; true )).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

bool
foreignWakeup(ARG1_LD)
{ if ( *valTermRef(LD->attvar.head) )
  { fid_t fid = PL_open_foreign_frame();
    int rval;
    term_t a0 = PL_new_term_ref();

    PL_put_term(a0, LD->attvar.head);
    setVar(*valTermRef(LD->attvar.head));
    setVar(*valTermRef(LD->attvar.tail));

    rval = PL_call_predicate(NULL, PL_Q_NORMAL, PROCEDURE_dwakeup1,
			     a0);

    PL_close_foreign_frame(fid);

    return rval;
  }

  succeed;
}


		 /*******************************
		 *   FOREIGN-LANGUAGE INTERFACE *
		 *******************************/

#include "pl-fli.c"

#if O_BLOCK
		/********************************
		*         BLOCK SUPPORT         *
		*********************************/

static LocalFrame
findBlock(LocalFrame fr, Word block)
{ GET_LD
  for(; fr; fr = fr->parent)
  { if ( fr->predicate == PROCEDURE_block3->definition &&
	 unify_ptrs(argFrameP(fr, 0), block PASS_LD) )
      return fr;
  }

  PL_error(NULL, 0, NULL, ERR_EXISTENCE, ATOM_block, wordToTermRef(block));

  return NULL;
}

#endif /*O_BLOCK*/

#ifdef O_DEBUGGER
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
findStartChoice(LocalFrame fr, Choice ch)
    Within the same query, find the choice-point that was created at the
    start of this frame.  This is used for the debugger at the fail-port
    as well as for realising retry.

    Note that older versions also considered the initial choicepoint a
    choicepoint for the initial frame, but this is not correct as the
    frame may be replaced due to last-call optimisation.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static Choice
findStartChoice(LocalFrame fr, Choice ch)
{ for( ; (void *)ch > (void *)fr; ch = ch->parent )
  { if ( ch->frame == fr )
    { switch ( ch->type )
      { case CHP_JUMP:
	case CHP_NONE:
	  continue;			/* might not be at start */
	default:
	  return ch;
      }
    }
  }

  return NULL;
}
#endif /*O_DEBUGGER*/


#if O_CATCHTHROW
		/********************************
		*        EXCEPTION SUPPORT      *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Find the I_EXIT of catch/3. We use this as the return address of catch/3
when running the handler. Maybe we can remove the catch/3 in the future?
This would also fix the problem that  we   need  to be sure not to catch
exceptions from the handler.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static Code
findCatchExit()
{ if ( !GD->exceptions.catch_exit_address )
  { Definition catch3 = PROCEDURE_catch3->definition;
    Clause cl = catch3->definition.clauses->clause;
    Code Exit = &cl->codes[cl->code_size-1];
    assert(*Exit == encode(I_EXIT));

    GD->exceptions.catch_exit_address = Exit;
  }

  return GD->exceptions.catch_exit_address;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Find the frame running catch/3. If we found  it, we will mark this frame
and not find it again, as a catcher   can  only catch once from the 1-st
argument goal. Exceptions from the  recover   goal  should be passed (to
avoid a loop and allow for re-throwing).   With  thanks from Gertjan van
Noord.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static LocalFrame
findCatcher(LocalFrame fr, Word ex ARG_LD)
{ Definition catch3  = PROCEDURE_catch3->definition;

  for(; fr; fr = fr->parent)
  { if ( fr->predicate != catch3 )
      continue;
    if ( true(fr, FR_CATCHED) )
      continue;
    if ( unify_ptrs(argFrameP(fr, 1), ex PASS_LD) )
    { set(fr, FR_CATCHED);
      return fr;
    }
  }

  return NULL;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
See whether some outer  environment  will   catch  this  exception. I.e.
catch(Goal, ...), where Goal calls C, calls   Prolog  and then raises an
exception somewhere.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifndef offset
#define offset(s, f) ((size_t)(&((struct s *)NULL)->f))
#endif

#ifdef O_DEBUGGER
static int
isCatchedInOuterQuery(QueryFrame qf, Word catcher)
{ Definition catch3 = PROCEDURE_catch3->definition;

  while( qf && true(qf, PL_Q_PASS_EXCEPTION) )
  { LocalFrame fr = qf->saved_environment;

    while( fr )
    { if ( fr->predicate == catch3 && can_unify(argFrameP(fr, 1), catcher) )
	succeed;

      if ( fr->parent )
      { fr = fr->parent;
      } else
      { qf = QueryOfTopFrame(fr);
	break;
      }
    }

  }

  fail;
}


static inline int
slotsInFrame(LocalFrame fr, Code PC)
{ Definition def = fr->predicate;

  if ( !PC || true(def, FOREIGN) || !fr->clause )
    return def->functor->arity;

  return fr->clause->clause->prolog_vars;
}


static void
updateMovedTerm(LocalFrame fr, word old, word new)
{ Code pc = NULL;

  for(; fr; fr=fr->parent)
  { int slots = slotsInFrame(fr, pc);
    Word p = argFrameP(fr, 0);
    
    for(; slots-- > 0; p++)
    { if ( *p == old )
	*p = new;
    }
  }
}


#endif /*O_DEBUGGER*/

static int
exception_hook(LocalFrame fr, LocalFrame catcher ARG_LD)
{ if ( PROCEDURE_exception_hook4->definition->definition.clauses )
  { if ( !LD->exception.in_hook )
    { fid_t fid, wake;
      qid_t qid;
      term_t av;
      int debug, trace, rc;
  
      LD->exception.in_hook++;
      blockGC(PASS_LD1);
      wake = saveWakeup(PASS_LD1);
      fid = PL_open_foreign_frame();
      av = PL_new_term_refs(4);
  
      PL_put_term(av+0, exception_bin);
      PL_put_frame(av+2, fr);
      if ( catcher )
	catcher = parentFrame(catcher);
      PL_put_frame(av+3, catcher);
  
      exception_term = 0;
      setVar(*valTermRef(exception_bin));
      qid = PL_open_query(MODULE_user, PL_Q_NODEBUG,
			  PROCEDURE_exception_hook4, av);
      rc = PL_next_solution(qid);
      debug = debugstatus.debugging;
      trace = debugstatus.tracing;
      PL_cut_query(qid);
      if ( rc )				/* pass user setting trace/debug */
      { if ( debug ) debugstatus.debugging = TRUE;
	if ( trace ) debugstatus.tracing = TRUE;
      }

      PL_put_term(exception_bin, rc ? av+1 : av+0);
      exception_term = exception_bin;
      
      PL_close_foreign_frame(fid);
      restoreWakeup(wake PASS_LD);
      unblockGC(PASS_LD1);
      LD->exception.in_hook--;

      return rc;
    } else
    { PL_warning("Recursive exception in prolog_exception_hook/4");
    }
  }

  return FALSE;
}


#endif /*O_CATCHTHROW*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
isSimpleGoal(Word g)
    Determines whether we need to compile a call (as call/1) to the
    specified term (see I_USERCALL0) or we can call it directly.  The
    choice is based on optimisation.  Compilation is slower, but almost
    required to deal with really complicated cases.
 
    TBD: use CONTROL_F
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static bool
isSimpleGoal(Word a ARG_LD)		/* a is dereferenced and compound */
{ functor_t f = functorTerm(*a);

  if ( f == FUNCTOR_comma2 ||
       f == FUNCTOR_semicolon2 ||
       f == FUNCTOR_bar2 )
    fail;

  succeed;
}

		 /*******************************
		 *	  TAIL-RECURSION	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Tail recursion copy of the arguments of the new frame back into the  old
one.   This  should  be  optimised  by the compiler someday, but for the
moment this will do.

The new arguments block can contain the following types:
  - Instantiated data (atoms, ints, reals, strings, terms
    These can just be copied.
  - Plain variables
    These can just be copied.
  - References to frames older than the `to' frame
    These can just be copied.
  - 1-deep references into the `to' frame.
    This is hard as there might be two of  them  pointing  to  the  same
    location  in  the  `to' frame, indicating sharing variables.  In the
    first pass we will fill the  variable  in  the  `to'  frame  with  a
    reference  to the new variable.  If we get another reference to this
    field we will copy the reference saved in the `to'  field.   Because
    on  entry  references into this frame are always 1 deep we KNOW this
    is a saved reference.  The critical program for this is:

	a :- b(X, X).
	b(X, Y) :- X == Y.
	b(X, Y) :- write(bug), nl.

					This one costed me 1/10 bottle of
					brandy to Huub Knops, SWI
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
copyFrameArguments(LocalFrame from, LocalFrame to, int argc ARG_LD)
{ Word ARGD, ARGS, ARGE;

  if ( argc == 0 )
    return;

  ARGS = argFrameP(from, 0);
  ARGE = ARGS+argc;
  ARGD = argFrameP(to, 0);
  for( ; ARGS < ARGE; ARGS++, ARGD++) /* dereference the block */
  { word k = *ARGS;

    if ( isRefL(k) )
    { Word p = unRefL(k);

      if ( p > (Word)to )
      { if ( isVar(*p) )
	{ *p = makeRefL(ARGD);
	  setVar(*ARGS);
	} else
	  *ARGS = *p;
      }
    }
  }    
  ARGS = argFrameP(from, 0);
  ARGD = argFrameP(to, 0);
  while( ARGS < ARGE )			/* now copy them */
    *ARGD++ = *ARGS++;  
}

		/********************************
		*          INTERPRETER          *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
			 MACHINE REGISTERS

  - DEF
    Definition structure of current procedure.
  - PC
    Virtual machine `program counter': pointer to the next byte code  to
    interpret.
  - ARGP
    Argument pointer.  Pointer to the next argument to be matched  (when
    in the clause head) or next argument to be instantiated (when in the
    clause  body).   Saved  and  restored  via  the  argument  stack for
    functors.
  - FR
    Current environment frame
  - BFR
    Frame where execution should continue if  the  current  goal  fails.
    Used by I_CALL and deviates to fill the backtrackFrame slot of a new
    frame and set by various instructions.
  - deterministic
    Last clause has been found deterministically
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define FRAME_FAILED		goto frame_failed
#define CLAUSE_FAILED		goto clause_failed
#define BODY_FAILED		goto body_failed

#ifdef O_PROFILE
#define Profile(g) if ( LD->profile.active ) g
#else
#define Profile(g) (void)0
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
{leave,discard}Frame()
     Exit from a frame.  leaveFrame() is used for normal leaving due to
     failure.  discardFrame() is used for frames that have
     been cut.  If such frames are running a foreign predicate, the
     functions should be called again using FRG_CUTTED context.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
leaveFrame(LocalFrame fr ARG_LD)
{ Definition def = fr->predicate;

  if ( false(def, FOREIGN) )
    leaveDefinition(def);

  if ( true(fr, FR_WATCHED) )
    frameFinished(fr, FINISH_FAIL PASS_LD);
}


static void
discardFrame(LocalFrame fr, enum finished reason ARG_LD)
{ Definition def = fr->predicate;

  DEBUG(2, Sdprintf("discard #%d running %s\n",
		    loffset(fr),
		    predicateName(fr->predicate)));

  if ( true(def, FOREIGN) )
  { if ( fr->clause )
    { discardForeignFrame(fr PASS_LD);
      fr->clause = NULL;
    }
  } else
  { fr->clause = NULL;		/* leaveDefinition() may destroy clauses */
    leaveDefinition(def);
  }

  if ( true(fr, FR_WATCHED) )
    frameFinished(fr, reason PASS_LD);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Discard all choice-points created after  the   creation  of the argument
environment. See also discardFrame().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if defined(O_DEBUG) || defined(SECURE_GC) || defined(O_MAINTENANCE)
char *
chp_chars(Choice ch)
{ static char buf[256];

  Ssprintf(buf, "Choice at #%ld for frame #%ld, type %s",
	   loffset(ch), loffset(ch->frame),
	   ch->type == CHP_JUMP ? "JUMP" :
	   ch->type == CHP_CLAUSE ? "CLAUSE" :
	   ch->type == CHP_FOREIGN ? "FOREIGN" : 
	   ch->type == CHP_TOP ? "TOP" :
	   ch->type == CHP_DEBUG ? "DEBUG" :
	   ch->type == CHP_CATCH ? "CATCH" : "NONE");

  return buf;
}
#endif


static void
discardChoicesAfter(LocalFrame fr ARG_LD)
{ for(; BFR && (LocalFrame)BFR > fr; BFR = BFR->parent)
  { LocalFrame fr2;

    DEBUG(3, Sdprintf("Discarding %s\n", chp_chars(BFR)));
    for(fr2 = BFR->frame;    
	fr2 && fr2->clause && fr2 > fr;
	fr2 = fr2->parent)
    { discardFrame(fr2, FINISH_CUT PASS_LD);
      if ( exception_term )
	break;
    }
  }

  DEBUG(3, Sdprintf(" --> BFR = #%ld\n", loffset(BFR)));
  LD->mark_bar = BFR->mark.globaltop;
} 


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Discard choicepoints in debugging mode.  As we might be doing callbacks
on behalf of the debugger we need to preserve the pending exception.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
dbg_discardChoicesAfter(LocalFrame fr ARG_LD)
{ blockGC(PASS_LD1);

  if ( exception_term )
  { Word p = valTermRef(exception_term);
    DEBUG(3, Sdprintf("dbg_discardChoicesAfter(): saving exception: ");
	     pl_writeln(exception_term));
    exception_term = 0;
    discardChoicesAfter(fr PASS_LD);
    *valTermRef(exception_bin) = *p;
    exception_term = exception_bin;
  } else
    discardChoicesAfter(fr PASS_LD);

  unblockGC(PASS_LD1);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
newChoice(CH_*, FR) Creates a new  choicepoint.   After  creation of the
choice-point, the user has to fill the choice-points mark as well as the
required context value.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static Choice
newChoice(choice_type type, LocalFrame fr ARG_LD)
{ Choice ch = (Choice)lTop;

  requireStack(local, sizeof(*ch));
  lTop = addPointer(lTop, sizeof(*ch));
  ch->type = type;
  ch->frame = fr;
  ch->parent = BFR;
  Mark(ch->mark);
  BFR = ch;
#ifdef O_PROFILE
  ch->prof_node = LD->profile.current;
#endif
  DEBUG(3, Sdprintf("NEW %s\n", chp_chars(ch)));

  return ch;
}


qid_t
PL_open_query(Module ctx, int flags, Procedure proc, term_t args)
{ GET_LD
  QueryFrame qf;
  LocalFrame fr;
  Definition def;
  int arity;
  Word ap;

  DEBUG(2, { FunctorDef f = proc->definition->functor;
	     unsigned int n;

	     Sdprintf("PL_open_query: %s(", stringAtom(f->name));
	     for(n=0; n < f->arity; n++)
	     { if ( n > 0 )
		 Sdprintf(", ");
	       PL_write_term(Serror, args+n, 999, 0);
	     }
	     Sdprintf(")\n");
	   });

					/* should be struct alignment, */
					/* but for now, I think this */
					/* is always the same */
#ifdef JMPBUF_ALIGNMENT
  while ( (uintptr_t)lTop % JMPBUF_ALIGNMENT )
    lTop = addPointer(lTop, sizeof(word));
#endif

  requireStack(local, sizeof(struct queryFrame));

  qf	     = (QueryFrame) lTop;
  fr         = &qf->frame;
  fr->parent = NULL;
  fr->flags  = FR_INBOX;
  def        = getProcDefinedDefinition(&fr, NULL, proc PASS_LD);
#ifdef O_SHIFT_STACKS
  qf	     = (QueryFrame) lTop;
#endif
  arity	     = def->functor->arity;

  requireStack(local, sizeof(struct queryFrame)+arity*sizeof(word));

  SECURE(checkStacks(environment_frame, NULL));
  assert((uintptr_t)fli_context > (uintptr_t)environment_frame);
  assert((uintptr_t)lTop >= (uintptr_t)(fli_context+1));

  if ( flags == TRUE )			/* compatibility */
    flags = PL_Q_NORMAL;
  else if ( flags == FALSE )
    flags = PL_Q_NODEBUG;
  flags &= 0x1f;			/* mask reserved flags */

  qf->magic		= QID_MAGIC;
  qf->foreign_frame	= 0;
  qf->flags		= flags;
  qf->saved_environment = environment_frame;
  qf->saved_bfr		= LD->choicepoints;
  qf->aSave             = aTop;
  qf->solutions         = 0;
  qf->exception		= 0;
  qf->exception_env.parent = NULL;
  qf->saved_throw_env   = LD->exception.throw_environment;

					/* fill frame arguments */
  ap = argFrameP(fr, 0);
  { int n;
    Word p = valTermRef(args);

    for( n = arity; n-- > 0; p++ )
      *ap++ = linkVal(p);
  }
					/* lTop above the arguments */
  lTop = (LocalFrame)ap;

					/* initialise flags and level */
  if ( qf->saved_environment )
  { setLevelFrame(fr, levelFrame(qf->saved_environment)+1);
    if ( true(qf->saved_environment, FR_NODEBUG) )
      set(fr, FR_NODEBUG);
  } else
  { setLevelFrame(fr, 1);
  }
			
  DEBUG(3, Sdprintf("Level = %d\n", levelFrame(fr)));
  if ( true(qf, PL_Q_NODEBUG) )
  { set(fr, FR_NODEBUG);
    debugstatus.suspendTrace++;
    qf->debugSave = debugstatus.debugging;
    debugstatus.debugging = DBG_OFF;
#ifdef O_LIMIT_DEPTH
    qf->saved_depth_limit   = depth_limit;
    qf->saved_depth_reached = depth_reached;
    depth_limit = (uintptr_t)DEPTH_NO_LIMIT;
#endif
  }
  fr->predicate      = def;
  fr->clause         = NULL;
					/* create initial choicepoint */
  qf->choice.type   = CHP_TOP;
  qf->choice.parent = NULL;
  qf->choice.frame  = fr;
#ifdef O_PROFILE
  qf->choice.prof_node = NULL;
  fr->prof_node = NULL;			/* true? */
#endif
  Mark(qf->choice.mark);
					/* publish environment */
  LD->choicepoints  = &qf->choice;

  if ( true(def, FOREIGN) )
  { fr->clause = NULL;			/* initial context */
  } else
  { fr->clause = def->definition.clauses;
  }
#ifdef O_LOGICAL_UPDATE
  fr->generation = GD->generation;
#endif
					/* context module */
  if ( true(def, METAPRED) )
  { if ( ctx )
      fr->context = ctx;
    else if ( qf->saved_environment )
      fr->context = qf->saved_environment->context;
    else
      fr->context = MODULE_user;
  } else
    fr->context = def->module;

  environment_frame = fr;
  DEBUG(2, Sdprintf("QID=%d\n", QidFromQuery(qf)));

  return QidFromQuery(qf);
}


static void
discard_query(QueryFrame qf)
{ GET_LD
  LocalFrame FR  = &qf->frame;

  discardChoicesAfter(FR PASS_LD);
  discardFrame(FR, FINISH_CUT PASS_LD);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Restore the environment. If an exception was raised by the query, and no
new  exception  has  been  thrown,  consider    it  handled.  Note  that
LD->choicepoints must be restored *before*   environment_frame to ensure
async safeness for markAtomsInEnvironments().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
restore_after_query(QueryFrame qf)
{ GET_LD
  if ( qf->exception && !exception_term )
    *valTermRef(exception_printed) = 0;

  LD->choicepoints  = qf->saved_bfr;
  environment_frame = qf->saved_environment;
  LD->exception.throw_environment = qf->saved_throw_env;
  aTop		    = qf->aSave;
  lTop		    = (LocalFrame)qf;
  if ( true(qf, PL_Q_NODEBUG) )
  { debugstatus.suspendTrace--;
    debugstatus.debugging = qf->debugSave;
#ifdef O_LIMIT_DEPTH
    depth_limit   = qf->saved_depth_limit;
    depth_reached = qf->saved_depth_reached;
#endif /*O_LIMIT_DEPTH*/
  }
  SECURE(checkStacks(environment_frame, NULL));
}


void
PL_cut_query(qid_t qid)
{ GET_LD
  QueryFrame qf = QueryFromQid(qid);

  SECURE(assert(qf->magic == QID_MAGIC));
  if ( qf->foreign_frame )
    PL_close_foreign_frame(qf->foreign_frame);

  if ( false(qf, PL_Q_DETERMINISTIC) )
    discard_query(qf);

  restore_after_query(qf);
  qf->magic = 0;			/* disqualify the frame */
}


void
PL_close_query(qid_t qid)
{ GET_LD
  QueryFrame qf = QueryFromQid(qid);

  SECURE(assert(qf->magic == QID_MAGIC));
  if ( qf->foreign_frame )
    PL_close_foreign_frame(qf->foreign_frame);

  if ( false(qf, PL_Q_DETERMINISTIC) )
    discard_query(qf);

  if ( !(qf->exception && true(qf, PL_Q_PASS_EXCEPTION)) )
    Undo(qf->choice.mark);

  restore_after_query(qf);
  qf->magic = 0;			/* disqualify the frame */
}


term_t
PL_exception(qid_t qid)
{ GET_LD
  QueryFrame qf = QueryFromQid(qid);

  return qf->exception;
}


#if O_SHIFT_STACKS
#define SAVE_REGISTERS(qid) \
	{ QueryFrame qf = QueryFromQid(qid); \
	  qf->registers.fr  = FR; \
	}
#define LOAD_REGISTERS(qid) \
	{ QueryFrame qf = QueryFromQid(qid); \
	  FR = qf->registers.fr; \
	}
#else /*O_SHIFT_STACKS*/
#define SAVE_REGISTERS(qid)
#define LOAD_REGISTERS(qid)
#endif /*O_SHIFT_STACKS*/

#ifndef ASM_NOP
int _PL_nop_counter;

#define ASM_NOP _PL_nop_counter++
#endif

int
PL_next_solution(qid_t qid)
{ GET_LD
  AR_CTX
  QueryFrame QF;			/* Query frame */
  LocalFrame FR;			/* current frame */
  Word	     ARGP = NULL;		/* current argument pointer */
  Code	     PC = NULL;			/* program counter */
  Definition DEF = NULL;		/* definition of current procedure */
  Word *     aFloor = aTop;		/* don't overwrite old arguments */
#define	     CL (FR->clause)		/* clause of current frame */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Get the labels of the various  virtual-machine instructions in an array.
This is for exploiting GCC's `goto   var' language extension. This array
can only be allocated insite this   function. The initialisation process
calls PL_next_solution() with qid =  QID_EXPORT_WAM_TABLE. This function
will export jmp_table as the compiler  needs   to  know  this table. See
pl-comp.c
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef O_PROF_PENTIUM
#include "pentium.h"

#define PROF_FOREIGN (I_HIGHEST+1)

#else
#define START_PROF(id, name)
#define END_PROF()
#endif

#if VMCODE_IS_ADDRESS
  static void *jmp_table[] =
  { &&I_NOP_LBL,
    &&I_ENTER_LBL,
    &&I_CALL_LBL,
    &&I_DEPART_LBL,
    &&I_EXIT_LBL,
    &&B_FUNCTOR_LBL,
    &&B_RFUNCTOR_LBL,
    &&H_FUNCTOR_LBL,
    &&H_RFUNCTOR_LBL,
    &&I_POPF_LBL,
    &&B_VAR_LBL,
    &&H_VAR_LBL,
    &&B_CONST_LBL,
    &&H_CONST_LBL,
    &&B_STRING_LBL,
    &&H_STRING_LBL,
    &&B_MPZ_LBL,
    &&H_MPZ_LBL,
    &&B_INTEGER_LBL,
    &&H_INTEGER_LBL,
    &&B_INT64_LBL,
    &&H_INT64_LBL,
    &&B_FLOAT_LBL,
    &&H_FLOAT_LBL,

    &&B_FIRSTVAR_LBL,
    &&H_FIRSTVAR_LBL,
    &&B_VOID_LBL,
    &&H_VOID_LBL,
    &&B_ARGFIRSTVAR_LBL,
    &&B_ARGVAR_LBL,

    &&H_NIL_LBL,
    &&B_NIL_LBL,
    &&H_LIST_LBL,
    &&H_RLIST_LBL,
    &&B_LIST_LBL,
    &&B_RLIST_LBL,

    &&B_VAR0_LBL,
    &&B_VAR1_LBL,
    &&B_VAR2_LBL,

    &&I_USERCALL0_LBL,
    &&I_USERCALLN_LBL,
    &&I_CUT_LBL,
    &&I_APPLY_LBL,

#if O_COMPILE_ARITH
    &&A_ENTER_LBL,
    &&A_INTEGER_LBL,
    &&A_INT64_LBL,
    &&A_MPZ_LBL,
    &&A_DOUBLE_LBL,
    &&A_VAR0_LBL,
    &&A_VAR1_LBL,
    &&A_VAR2_LBL,
    &&A_VAR_LBL,
    &&A_FUNC0_LBL,
    &&A_FUNC1_LBL,
    &&A_FUNC2_LBL,
    &&A_FUNC_LBL,
    &&A_LT_LBL,
    &&A_GT_LBL,
    &&A_LE_LBL,
    &&A_GE_LBL,
    &&A_EQ_LBL,
    &&A_NE_LBL,
    &&A_IS_LBL,
#endif /* O_COMPILE_ARITH */

#if O_COMPILE_OR
    &&C_OR_LBL,
    &&C_JMP_LBL,
    &&C_MARK_LBL,
    &&C_CUT_LBL,
    &&C_IFTHENELSE_LBL,
    &&C_VAR_LBL,
    &&C_END_LBL,
    &&C_NOT_LBL,
    &&C_FAIL_LBL,
#endif /* O_COMPILE_OR */

#if O_BLOCK
    &&I_CUT_BLOCK_LBL,
    &&B_EXIT_LBL,
#endif /*O_BLOCK*/
#if O_INLINE_FOREIGNS
    &&I_CALL_FV0_LBL,
    &&I_CALL_FV1_LBL,
    &&I_CALL_FV2_LBL,
#endif /*O_INLINE_FOREIGNS*/
    &&I_FAIL_LBL,
    &&I_TRUE_LBL,
#ifdef O_SOFTCUT
    &&C_SOFTIF_LBL,
    &&C_SOFTCUT_LBL,
#endif
    &&I_EXITFACT_LBL,
    &&D_BREAK_LBL,
#if O_CATCHTHROW
    &&I_CATCH_LBL,
    &&I_EXITCATCH_LBL,
    &&B_THROW_LBL,
#endif
    &&I_CONTEXT_LBL,
    &&C_LCUT_LBL,
    &&I_CALLCLEANUP_LBL,
    &&I_EXITCLEANUP_LBL,
    NULL
  };


#define VMI(Name)		Name ## _LBL: \
				  count(Name, PC); \
				  START_PROF(Name, #Name);
#define NEXT_INSTRUCTION	{ DbgPrintInstruction(FR, PC); \
				  END_PROF(); \
				  goto *(void *)((intptr_t)(*PC++)); \
				}
#ifndef ASM_NOP
#define ASM_NOP asm("nop")
#endif
#define SEPERATE_VMI ASM_NOP

#else /* VMCODE_IS_ADDRESS */

code thiscode;

#define VMI(Name)		case Name: \
				  count(Name, PC); \
				  START_PROF(Name, #Name);
#define NEXT_INSTRUCTION	{ DbgPrintInstruction(FR, PC); \
				  END_PROF(); \
                                  goto next_instruction; \
				}
#define SEPERATE_VMI		(void)0

#endif /* VMCODE_IS_ADDRESS */

#if VMCODE_IS_ADDRESS
  if ( qid == QID_EXPORT_WAM_TABLE )
  { interpreter_jmp_table = jmp_table;	/* make it globally known */
    succeed;
  }
#endif /* VMCODE_IS_ADDRESS */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This is the real start point  of   this  function.  Simply loads the VMI
registers from the frame filled by   PL_open_query()  and either jump to
depart_continue() to do the normal thing or to the backtrack point.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  QF  = QueryFromQid(qid);
  SECURE(assert(QF->magic == QID_MAGIC));
  if ( true(QF, PL_Q_DETERMINISTIC) )	/* last one succeeded */
  { fid_t fid = QF->foreign_frame;
    QF->foreign_frame = 0;
    PL_close_foreign_frame(fid);
    Undo(QF->choice.mark);
    fail;
  }
  FR  = &QF->frame;
  DEBUG(9, Sdprintf("QF=%p, FR=%p\n", QF, FR));

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Check for exceptions raised by foreign code.  PL_throw() uses longjmp()
to get back here.  Our task is to restore the environment and throw the
Prolog exception.

setjmp()/longjmp clobbers register variables. FR   is  restored from the
environment. BFR is volatile, and qid is an argument. These are the only
variables used in the B_THROW instruction.

Is there a way to make the compiler keep its mouth shut!?
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  DEBUG(9, Sdprintf("Setjmp env at %p\n", &QF->exception_env.exception_jmp_env));
  if ( setjmp(QF->exception_env.exception_jmp_env) != 0 )
  { FliFrame ffr;
#ifdef O_PLMT
    __PL_ld = GLOBAL_LD;		/* might be clobbered */
#endif
    ffr = fli_context;

    FR = environment_frame;
    DEF = FR->predicate;
    while(ffr && (void *)ffr > (void *)FR) /* discard foreign contexts */
      ffr = ffr->parent;
    fli_context = ffr;

    AR_CLEANUP();

    if ( LD->current_signal ) 
    { unblockSignal(LD->current_signal);
      LD->current_signal = 0;	/* TBD: saved? */
    }

    goto b_throw;
  }

  LD->exception.throw_environment = &QF->exception_env;
  DEF = FR->predicate;
  if ( QF->solutions )			/* retry */
  { fid_t fid = QF->foreign_frame;
    QF->foreign_frame = 0;
    PL_close_foreign_frame(fid);
    BODY_FAILED;
  } else
    goto retry_continue;		/* first call */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Main entry of the virtual machine cycle.  A branch to `next instruction'
will  cause  the  next  instruction  to  be  interpreted.   All  machine
registers  should  hold  valid  data  and  the  machine stacks should be
initialised properly.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if VMCODE_IS_ADDRESS
  NEXT_INSTRUCTION;
#else
next_instruction:
  thiscode = *PC++;
#ifdef O_DEBUGGER
resumebreak:
#endif
  switch( thiscode )
#endif
  {

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
D_BREAK implements break-points in the  code.   A  break-point is set by
replacing  an  instruction  by  a   D_BREAK  instruction.  The  orininal
instruction is saved in a table. replacedBreak() fetches it.

Note that we must  be  careful  that   the  user  may  have  removed the
break-point in the debugger, so we must check for it.

We might be in a state where  we   are  writing  the arguments above the
current lTop, and therefore with higher this  with the maximum number of
arguments.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(D_BREAK)
#if O_DEBUGGER
    if ( debugstatus.debugging )
    { int action;
      LocalFrame lSave = lTop;

      lTop = (LocalFrame)argFrameP(lTop, MAXARITY);
      clearUninitialisedVarsFrame(FR, PC-1);
      action = tracePort(FR, BFR, BREAK_PORT, PC-1 PASS_LD);
      lTop = lSave;

      switch(action)
      { case ACTION_RETRY:
	  goto retry;
      }

      if ( PC[-1] != encode(D_BREAK) )
      { PC--;
	NEXT_INSTRUCTION;
      }
    }
#if VMCODE_IS_ADDRESS
    { void *c = (void *)replacedBreak(PC-1);
      
      goto *c;
    }
#else
    thiscode = replacedBreak(PC-1);
    goto resumebreak;
#endif      
#endif /*O_DEBUGGER*/

    VMI(I_NOP)
	NEXT_INSTRUCTION;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
An atomic constant in the head  of  the  clause.   ARGP  points  to  the
current  argument  to be matched.  ARGP is derefenced and unified with a
constant argument.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  { word c;
    Word k;						MARK(HCONST);

    VMI(H_CONST)
	c = (word)*PC++;
	goto common_hconst;
    VMI(H_NIL)
        c = ATOM_nil;

  common_hconst:
        deRef2(ARGP++, k);
        if ( *k == c )
	  NEXT_INSTRUCTION;
        if ( canBind(*k) )
	{ bindConst(k, c);
	  NEXT_INSTRUCTION;
	}
        CLAUSE_FAILED;
  }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
32-bit integer in the head. Copy to the  global stack if the argument is
variable, compare the numbers otherwise.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(H_INTEGER) MARK(HINT)
      { Word k;

	deRef2(ARGP++, k);
	if ( canBind(*k) )
	{ Word p = allocGlobal(2+WORDS_PER_INT64);
	  word c = consPtr(p, TAG_INTEGER|STG_GLOBAL);
	  union
	  { int64_t val;
	    word w[WORDS_PER_INT64];
	  } cvt;
	  Word vp = cvt.w;

	  cvt.val = (int64_t)(intptr_t)*PC++;
	  *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
	  cpInt64Data(p, vp);
	  *p = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
	  bindConst(k, c);
	  NEXT_INSTRUCTION;
	} else if ( isBignum(*k) && valBignum(*k) == (intptr_t)*PC++ )
	  NEXT_INSTRUCTION;

      	CLAUSE_FAILED;
      }  

    VMI(H_INT64) MARK(HINT64)
      { Word k;

	deRef2(ARGP++, k);
	if ( canBind(*k) )
	{ Word p = allocGlobal(2+WORDS_PER_INT64);
	  word c = consPtr(p, TAG_INTEGER|STG_GLOBAL);
	  size_t i;

	  *p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
	  for(i=0; i<WORDS_PER_INT64; i++)
	    *p++ = (word)*PC++;
	  *p = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
	  bindConst(k, c);
	  NEXT_INSTRUCTION;
	} else if ( isBignum(*k) )
	{ Word vk = valIndirectP(*k);
	  size_t i;

	  for(i=0; i<WORDS_PER_INT64; i++)
	  { if ( *vk++ != (word)*PC++ )
	      CLAUSE_FAILED;
	  }  
	  NEXT_INSTRUCTION;
	}

      	CLAUSE_FAILED;
      }

    VMI(H_FLOAT) MARK(HFLOAT)
      { Word k;

	deRef2(ARGP++, k);
	if ( canBind(*k) )
	{ Word p = allocGlobal(2+WORDS_PER_DOUBLE);
	  word c = consPtr(p, TAG_FLOAT|STG_GLOBAL);

	  *p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
	  cpDoubleData(p, PC);
	  *p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
	  bindConst(k, c);
	  NEXT_INSTRUCTION;
	} else if ( isReal(*k) )
	{ Word p = valIndirectP(*k);

	  switch(WORDS_PER_DOUBLE) /* depend on compiler to clean up */
	  { case 2:
	      if ( *p++ != *PC++ )
	        CLAUSE_FAILED;
	    case 1:
	      if ( *p++ == *PC++ )
	        NEXT_INSTRUCTION;
	      CLAUSE_FAILED;
	    default:
	      assert(0);
	  }
	}

      	CLAUSE_FAILED;
      }  

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
General indirect in the head.  Used for strings only at the moment.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(H_MPZ)    MARK(HMPZ);
      SEPERATE_VMI;
    VMI(H_STRING) MARK(HSTR);
      { Word k;

	deRef2(ARGP++, k);
	if ( canBind(*k) )
	{ word c = globalIndirectFromCode(&PC);
	  bindConst(k, c);
	  NEXT_INSTRUCTION;
	}
	if ( isIndirect(*k) && equalIndirectFromCode(*k, &PC) )
	  NEXT_INSTRUCTION;
	CLAUSE_FAILED;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
An atomic constant in the body of  a  clause.   We  know  that  ARGP  is
pointing  to  a  not  yet  instantiated  argument  of the next frame and
therefore can just fill the argument.  Trailing is not needed as this is
above the stack anyway.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_CONST) MARK(BCONST);
      { *ARGP++ = (word)*PC++;
	NEXT_INSTRUCTION;
      }
    VMI(B_NIL) MARK(BNIL);
      { *ARGP++ = ATOM_nil;
        NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
32-bit integer in write-mode (body).  Simply   create  the bignum on the
global stack and assign the pointer to *ARGP.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(B_INTEGER) MARK(BINT)
      { Word p = allocGlobal(2+WORDS_PER_INT64);
	union
	{ int64_t val;
	  word w[WORDS_PER_INT64];
	} cvt;
	Word vp = cvt.w;

	cvt.val = (int64_t)(intptr_t)*PC++;
	*ARGP++ = consPtr(p, TAG_INTEGER|STG_GLOBAL);
	*p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
	cpInt64Data(p, vp);
	*p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
	NEXT_INSTRUCTION;
      }  

    VMI(B_INT64) MARK(BINT64)
      { Word p = allocGlobal(2+WORDS_PER_INT64);
	size_t i;
	
	*ARGP++ = consPtr(p, TAG_INTEGER|STG_GLOBAL);
	*p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);
	for(i=0; i<WORDS_PER_INT64; i++)
	  *p++ = (word)*PC++;
	*p++ = mkIndHdr(WORDS_PER_INT64, TAG_INTEGER);

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Double  in  the  body.  Simply  copy  to  the  global  stack.  See  also
globalReal().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(B_FLOAT) MARK(BINT)
      { Word p = allocGlobal(2+WORDS_PER_DOUBLE);

	*ARGP++ = consPtr(p, TAG_FLOAT|STG_GLOBAL);
	*p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
	cpDoubleData(p, PC);
	*p++ = mkIndHdr(WORDS_PER_DOUBLE, TAG_FLOAT);
	NEXT_INSTRUCTION;
      }  


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
B_STRING need to copy the  value  on   the  global  stack  because the
XR-table might be freed due to a retract.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_MPZ)	  MARK(BMPZ)
      SEPERATE_VMI;
    VMI(B_STRING) MARK(BSTR);
      { *ARGP++ = globalIndirectFromCode(&PC);
	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A variable in the head which is not an anonymous one and is not used for
the first time.  Invoke general unification between the argument pointer
and the variable, whose offset is given relative to  the  frame.

Its doubtfull whether inlining (the simple   cases)  is worthwhile. I've
tested this on various platforms, and   the  results vary. Simplicity is
probably worth more than the 0.001% performance to gain.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(H_VAR) MARK(HVAR);
      { Word p1 = varFrameP(FR, *PC++);
	Word p2 = ARGP++;

	if ( raw_unify_ptrs(p1, p2 PASS_LD) )
	  NEXT_INSTRUCTION;
	if ( exception_term )
	  goto b_throw;
	CLAUSE_FAILED;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A variable in the body which is not an anonymous one, is  not  used  for
the  first  time  and is nested in a term (with B_FUNCTOR).  We now know
that *ARGP is a variable,  so  we  either  copy  the  value  or  make  a
reference.   The  difference between this one and B_VAR is the direction
of the reference link in case *k turns out to be variable.

ARGP is pointing into the term on the global stack we are creating.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_ARGVAR) MARK(BAVAR);
      { Word k;

	deRef2(varFrameP(FR, *PC++), k);	
	if ( isVar(*k) )
	{ if ( ARGP < k )
	  { setVar(*ARGP);
	    *k = makeRefG(ARGP++);
	    Trail(k);
	    NEXT_INSTRUCTION;
	  }
	  *ARGP++ = makeRefG(k);	/* both on global stack! */
#ifdef O_ATTVAR
	} else if ( isAttVar(*k) )
	{ *ARGP++ = makeRefG(k);
#endif
	} else
	{ *ARGP++ = *k;
	}

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A variable in the body which is not an anonymous one and is not used for
the first time.  We now know that *ARGP is a variable, so we either copy
the value or make a reference.  Trailing is not needed as we are writing
above the stack.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  { int n;

    VMI(B_VAR0)						MARK(BVAR0);
      n = VAROFFSET(0);
      goto common_bvar;
    VMI(B_VAR1)						MARK(BVAR1);
      n = VAROFFSET(1);
      goto common_bvar;
    VMI(B_VAR2)						MARK(BVAR2);
      n = VAROFFSET(2);
      goto common_bvar;
    VMI(B_VAR)						MARK(BVARN);
      n = (int)*PC++;
    common_bvar:
    { Word k = varFrameP(FR, n);

      *ARGP++ = linkVal(k);

      NEXT_INSTRUCTION;
    }
  }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A variable in the head, which is  not anonymous, but encountered for the
first time. So we know that the variable   is  still a variable. Copy or
make a reference. Trailing is  not  needed   as  we  are writing in this
frame. As ARGP is pointing in the  argument   list,  it  is on the local
stack.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(H_FIRSTVAR)
      MARK(HFVAR);
      { varFrame(FR, *PC++) = (needsRef(*ARGP) ? makeRef(ARGP) : *ARGP);
	ARGP++;
	NEXT_INSTRUCTION;
      }
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A variable in the body nested in a term, encountered for the first time.
We now know both *ARGP and the variable are variables.  ARGP  points  to
the  argument  of  a  term  on  the  global stack.  The reference should
therefore go from k to ARGP.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_ARGFIRSTVAR)
      MARK(BAFVAR);
      { setVar(*ARGP);
	varFrame(FR, *PC++) = makeRefG(ARGP++);
	NEXT_INSTRUCTION;
      }
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A variable in the body, encountered for the first  time.   We  now  know
both  *ARGP and the variable are variables.  We set the variable to be a
variable (it is uninitialised memory) and make a reference.  No trailing
needed as we are writing in this and the next frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_FIRSTVAR)
      MARK(BFVAR);
      { Word k = varFrameP(FR, *PC++);

	setVar(*k);
	*ARGP++ = makeRefL(k);
	NEXT_INSTRUCTION;
      }
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A singleton variable in the head.  Just increment the argument  pointer.
Also generated for non-singleton variables appearing on their own in the
head  and  encountered  for  the  first  time.   Note  that the compiler
suppresses H_VOID when there are no other instructions before I_ENTER or
I_EXIT.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(H_VOID) MARK(HVOID);
      { ARGP++;
	NEXT_INSTRUCTION;
      }
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A singleton variable in the body. Ensure the argument is a variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_VOID) MARK(BVOID);
      { setVar(*ARGP++);
	NEXT_INSTRUCTION;
      }
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A functor in the head.  If the current argument is a  variable  we  will
instantiate  it  with  a  new  term,  all  whose  arguments  are  set to
variables.  Otherwise we check the functor  definition.   In  both  case
ARGP  is  pushed  on the argument stack and set to point to the leftmost
argument of the  term.   Note  that  the  instantiation  is  trailed  as
dereferencing might have caused we are now pointing in a parent frame or
the global stack (should we check?  Saves trail! How often?).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(H_FUNCTOR)
      MARK(HFUNC);
      { functor_t f;

	requireStack(argument, sizeof(Word));
	*aTop++ = ARGP + 1;
    VMI(H_RFUNCTOR)
	f = (functor_t) *PC++;
        deRef(ARGP);
	if ( canBind(*ARGP) )
	{ int arity = arityFunctor(f);
	  Word ap, ap0;
	  word c;

#ifdef O_SHIFT_STACKS
	  if ( gTop + 1 + arity > gMax )
	  { if ( !growStacks(FR, BFR, PC, 0, sizeof(word)*(1+arity), 0) )
	      goto b_throw;
	  }
#else
	  requireStack(global, sizeof(word)*(1+arity));
#endif

	  ap = gTop;
	  c = consPtr(ap, TAG_COMPOUND|STG_GLOBAL);
	  *ap++ = f;
	  ap0 = ap;
	  while(arity-- > 0)
	  { setVar(*ap++);
	  }
	  gTop = ap;
	  bindConst(ARGP, c);
	  ARGP = ap0;
	  NEXT_INSTRUCTION;
	}
	if ( hasFunctor(*ARGP, f) )
	{ ARGP = argTermP(*ARGP, 0);
	  NEXT_INSTRUCTION;
	}
	CLAUSE_FAILED;	    

    VMI(H_LIST) MARK(HLIST);
        requireStack(argument, sizeof(Word));
	*aTop++ = ARGP + 1;
    VMI(H_RLIST) MARK(HRLIST);
	deRef(ARGP);
	if ( canBind(*ARGP) )
	{ Word ap = gTop;
	  word c;

#if O_SHIFT_STACKS
  	  if ( ap + 3 > gMax )
	  { if ( !growStacks(FR, BFR, PC, 0, 3*sizeof(word), 0) )
	      goto b_throw;
	    ap = gTop;
	  }
#else
	  requireStack(global, 3*sizeof(word));
#endif
	  ap[0] = FUNCTOR_dot2;
	  setVar(ap[1]);
	  setVar(ap[2]);
	  gTop = ap+3;
	  c = consPtr(ap, TAG_COMPOUND|STG_GLOBAL);
	  bindConst(ARGP, c);
	  ARGP = ap+1;
	  NEXT_INSTRUCTION;
	}
	if ( isList(*ARGP) )
	{ ARGP = argTermP(*ARGP, 0);
	  NEXT_INSTRUCTION;
	}
	CLAUSE_FAILED;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A functor in the body.  As we don't expect ARGP to point to  initialised
memory  while  in  body  mode  we  just  allocate  the  term,  but don't
initialise the arguments to variables.  Allocation is done in  place  to
avoid a function call.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_FUNCTOR) MARK(BFUNC);
      { functor_t f;
	int arity;

	requireStack(argument, sizeof(Word));
	*aTop++ = ARGP+1;
    VMI(B_RFUNCTOR) MARK(BRFUNC);
	f = (functor_t) *PC++;
	arity = arityFunctor(f);
	requireStack(global, sizeof(word) * (1+arity));
	*ARGP = consPtr(gTop, TAG_COMPOUND|STG_GLOBAL);
	*gTop++ = f;
	ARGP = gTop;
	gTop += arity;

	NEXT_INSTRUCTION;
      }

    VMI(B_LIST) MARK(BLIST);
      { requireStack(argument, sizeof(Word));
	*aTop++ = ARGP+1;
    VMI(B_RLIST) MARK(BRLIST);
	requireStack(global, sizeof(word) * 3);
	*ARGP = consPtr(gTop, TAG_COMPOUND|STG_GLOBAL);
	*gTop++ = FUNCTOR_dot2;
	ARGP = gTop;
	gTop += 2;

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Pop the saved argument pointer (see H_FUNCTOR and B_FUNCTOR).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(I_POPF) MARK(POP);
      { ARGP = *--aTop;
	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Enter the body of the clause.  This  instruction  is  left  out  if  the
clause  has no body.  The basic task of this instruction is to move ARGP
from the argument part of this frame into the argument part of the child
frame to be built.  `BFR' (the last frame with alternatives) is  set  to
this   frame   if   this   frame  has  alternatives,  otherwise  to  the
backtrackFrame of this frame.

If this frame has no alternatives it is possible to  put  the  backtrack
frame  immediately  on  the backtrack frame of this frame.  This however
makes debugging much more  difficult  as  the  system  will  do  a  deep
backtrack without showing the fail ports explicitely.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(I_ENTER) MARK(ENTER);
      { 
#if O_DEBUGGER
	if ( debugstatus.debugging )
	{ clearUninitialisedVarsFrame(FR, PC);
	  switch(tracePort(FR, BFR, UNIFY_PORT, PC PASS_LD))
	  { case ACTION_RETRY:
	      goto retry;
	    case ACTION_FAIL:
	      FRAME_FAILED;
	  }
	}
#endif /*O_DEBUGGER*/

	ARGP = argFrameP(lTop, 0);
#ifdef O_ATTVAR
	if ( *valTermRef(LD->attvar.head) ) /* can be faster */
	  goto wakeup;
#endif
        NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CONTEXT is used by  non-meta  predicates   that  are  compiled  into a
different  module  using  <module>:<head>  :-    <body>.  The  I_CONTEXT
instruction immediately follows the I_ENTER. The argument is the module.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(I_CONTEXT) MARK(CONTEXT);
      { Module m = (Module)*PC++;

	FR->context = m;

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Part of call_cleanup(:Goal, :Cleanup).  Simply set a flag on the frame and
call the 1-st argument.  See also I_CATCH.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(I_CALLCLEANUP)
      { newChoice(CHP_CATCH, FR PASS_LD);
	set(FR, FR_WATCHED);
				/* = B_VAR1 */
	*argFrameP(lTop, 0) = linkVal(argFrameP(FR, 1));

	goto i_usercall0;
      }
      
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_EXITCLEANUP is at the end of call_cleanup/3. If there is no
choice-point created this is the final exit.  If this frame has no
parent (it is the entry of PL_next_solution()), 

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(I_EXITCLEANUP)
      { while( BFR && BFR->type == CHP_DEBUG )
	  BFR = BFR->parent;

	if ( BFR->frame == FR && BFR->type == CHP_CATCH )
	{ DEBUG(3, Sdprintf(" --> BFR = #%ld\n", loffset(BFR->parent)));
	  for(BFR = BFR->parent; BFR > (Choice)FR; BFR = BFR->parent)
	  { if ( BFR->frame > FR )
	      NEXT_INSTRUCTION;		/* choice from setup of setup_and_call_cleanup/4 */
	    assert(BFR->type == CHP_DEBUG);
	  }

	  frameFinished(FR, FINISH_EXITCLEANUP PASS_LD);
	  if ( exception_term )
	    goto b_throw;
	}

	NEXT_INSTRUCTION;		/* goto i_exit? */
      }

#if O_CATCHTHROW
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CATCH has to fake a choice-point to   make  it possible to undo before
starting the recover action. Otherwise it simply   has to call the first
argument.  Catch is defined as:

catch(Goal, Pattern, Recover) :-
	$catch.

which is translated to:
	I_ENTER
	I_CATCH
	I_EXITCATCH
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(I_CATCH)
      { if ( BFR->frame == FR && BFR == (Choice)argFrameP(FR, 3) )
	{ assert(BFR->type == CHP_DEBUG);
	  BFR->type = CHP_CATCH;
	} else
	  newChoice(CHP_CATCH, FR PASS_LD);

					/* = B_VAR0 */
	*argFrameP(lTop, 0) = linkVal(argFrameP(FR, 0));

	goto i_usercall0;
      }
      
    VMI(I_EXITCATCH)
      { if ( BFR->frame == FR && BFR == (Choice)argFrameP(FR, 3) )
	{ assert(BFR->type == CHP_CATCH);
	  BFR = BFR->parent;
	}

	goto exit_builtin_cont;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The B_THROW code is the implementation for   throw/1.  The call walks up
the stack, looking for a frame running catch/3 on which it can unify the
exception code. It then cuts all  choicepoints created since throw/3. If
throw/3 is not found, it sets  the   query  exception  field and returns
failure. Otherwise, it will simulate an I_USERCALL0 instruction: it sets
the FR and lTop as it it  was   running  the  throw/3 predicate. Then it
pushes the recovery goal from throw/3 and jumps to I_USERCALL0.

NOTE (**): At the moment  this   code  uses  undo_while_saving_term() to
unwind while preserving the ball. This  call   may  move the ball in the
current implementation but there may be   references  from the old ball.
What exactly are the conditions and how   can we avoid trouble here? For
the moment the code marked (**) handles this not very elegant
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_THROW) MARK(B_THROW);
      { Word catcher;
	word except;
	LocalFrame catchfr;

	PL_raise_exception(argFrameP(lTop, 0) - (Word)lBase);
    b_throw:
        assert(exception_term);
        catcher = valTermRef(exception_term);

	SECURE(checkData(catcher));
	DEBUG(1, { Sdprintf("[%d] Throwing ", PL_thread_self());
		   PL_write_term(Serror, wordToTermRef(catcher), 1200, 0);
		   Sdprintf("\n");
		 });

	except = *catcher;
        catchfr = findCatcher(FR, catcher PASS_LD);
	DEBUG(1, { if ( catchfr )
		   { Sdprintf("[%d]: found catcher at %d\n",
			      PL_thread_self(), levelFrame(catchfr));
		   } else
		   { Sdprintf("[%d]: not caught\n", PL_thread_self());
		   }
		 });

	SECURE(checkData(catcher));	/* verify all data on stacks stack */
	SECURE(checkStacks(FR, LD->choicepoints));

	if ( debugstatus.suspendTrace == FALSE &&
	     exception_hook(FR, catchfr PASS_LD) )
	{ catcher = valTermRef(exception_term);
	  except = *catcher;
	  /*catchfr = findCatcher(FR, catcher PASS_LD); already unified */
	}

#if O_DEBUGGER
	if ( !catchfr &&
	     hasFunctor(except, FUNCTOR_error2) &&
	     *valTermRef(exception_printed) != except )
	{ QF = QueryFromQid(qid);	/* reload for relocation */

	  if ( trueFeature(DEBUG_ON_ERROR_FEATURE) &&
	       false(QF, PL_Q_CATCH_EXCEPTION) &&
	       !isCatchedInOuterQuery(QF, catcher) )
	  { fid_t fid = PL_open_foreign_frame();
	    term_t t0 = PL_new_term_refs(2);
	    
	    PL_put_atom(t0+0, ATOM_error);
	    *valTermRef(t0+1) = except;
	    PL_call_predicate(NULL,
			      PL_Q_NODEBUG|PL_Q_CATCH_EXCEPTION,
			      PROCEDURE_print_message2, t0);
	    PL_close_foreign_frame(fid);
	    *valTermRef(exception_printed) = except;

	    pl_trace();
	  }
	}

        if ( debugstatus.debugging )
	{ blockGC(PASS_LD1);
	  for( ; FR && FR > catchfr; FR = FR->parent )
	  { Choice ch = findStartChoice(FR, LD->choicepoints);

	    if ( ch )
	    { int printed = (*valTermRef(exception_printed) == except);
	      word old = except;

					/* needed to avoid destruction */
					/* in the undo */
	      SECURE(checkStacks(FR, ch));
	      dbg_discardChoicesAfter((LocalFrame)ch PASS_LD);
	      undo_while_saving_term(&ch->mark, catcher);
	      except = *catcher;
	      *valTermRef(LD->exception.pending) = except;
	      if ( printed )
		*valTermRef(exception_printed) = except;

	      if ( old != except )
		updateMovedTerm(FR, old, except);  /* (**) See above */

	      environment_frame = FR;
	      SECURE(checkStacks(FR, ch));
	      switch(tracePort(FR, ch, EXCEPTION_PORT, PC PASS_LD))
	      { case ACTION_RETRY:
		  *valTermRef(exception_printed) = 0;
		  exception_term = 0;
		  Undo(ch->mark);
		  discardChoicesAfter(FR PASS_LD);
		  DEF = FR->predicate;
#ifdef O_LOGICAL_UPDATE
		  if ( false(DEF, DYNAMIC) )
		    FR->generation = GD->generation;
#endif
  		  unblockGC(PASS_LD1);
		  goto retry_continue;
	      }

	      *valTermRef(LD->exception.pending) = 0;
	    }

	    exception_term = 0;		/* save exception over call-back */
	    discardChoicesAfter(FR PASS_LD);
	    discardFrame(FR, FINISH_EXCEPT PASS_LD);
	    *valTermRef(exception_bin) = *catcher;
	    exception_term = exception_bin;
	  }
          unblockGC(PASS_LD1);
	} else
#endif /*O_DEBUGGER*/
	{ for( ; FR && FR > catchfr; FR = FR->parent )
	  { SECURE(checkData(catcher));
	    dbg_discardChoicesAfter(FR PASS_LD);
	    SECURE(checkData(catcher));
	    discardFrame(FR, FINISH_EXCEPT PASS_LD);
	    SECURE(checkData(catcher));
	  }
	}

        SECURE(checkData(catcher));

	if ( catchfr )
	{ Word p = argFrameP(FR, 1);
	  Choice ch = (Choice)argFrameP(FR, 3); /* Aligned above */

	  assert(ch->type == CHP_CATCH);

	  deRef(p);

	  assert(catchfr == FR);
	  discardChoicesAfter(FR PASS_LD);
	  environment_frame = FR;
	  undo_while_saving_term(&ch->mark, catcher);
	  unify_ptrs(p, catcher PASS_LD); /* undo_while_saving_term() also */
					  /* undoes unify of findCatcher() */
	  lTop = (LocalFrame) argFrameP(FR, 3); /* above the catch/3 */
	  argFrame(lTop, 0) = argFrame(FR, 2);  /* copy recover goal */
	  *valTermRef(exception_printed) = 0;   /* consider it handled */
	  *valTermRef(exception_bin)     = 0;
	  exception_term		 = 0;

	  PC = findCatchExit();
#if O_DYNAMIC_STACKS
	  considerGarbageCollect((Stack)NULL);

	  if ( LD->trim_stack_requested )
	    trimStacks(PASS_LD1);
#endif

	  goto i_usercall0;
	} else
	{ Word p;

	  QF = QueryFromQid(qid);	/* may be shifted: recompute */
	  set(QF, PL_Q_DETERMINISTIC);
	  FR = environment_frame = &QF->frame;
	  p = argFrameP(FR, FR->predicate->functor->arity);
	  lTop = (LocalFrame)(p+1);

					/* TBD: needs a foreign frame? */
	  QF->exception = consTermRef(p);
	  *p = except;

	  undo_while_saving_term(&QF->choice.mark, p);
	  if ( false(QF, PL_Q_PASS_EXCEPTION) )
	  { *valTermRef(exception_bin)     = 0;
	    exception_term		   = 0;
	    *valTermRef(exception_printed) = 0; /* consider it handled */
	  } else
	  { *valTermRef(exception_bin)     = *p;
	    exception_term		   = exception_bin;
	  }

#if O_DYNAMIC_STACKS
	  considerGarbageCollect((Stack)NULL);
#endif
	  if ( LD->trim_stack_requested )
	  { trimStacks(PASS_LD1);
	    QF = QueryFromQid(qid);	/* may be shifted: recompute */
	  }

	  QF->foreign_frame = PL_open_foreign_frame();
	  fail;
	}
      }
#endif /*O_CATCHTHROW*/

#if O_BLOCK
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
exit(Block, RVal).  First does !(Block).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(B_EXIT) MARK(B_EXIT);
      { Word name, rval;
	LocalFrame blockfr;

	name = argFrameP(lTop, 0); deRef(name);
	rval = argFrameP(lTop, 1); deRef(rval);
	lTop = (LocalFrame)argFrameP(lTop, 2);

        if ( !(blockfr = findBlock(FR, name)) )
	{ if ( exception_term )
	    goto b_throw;

	  BODY_FAILED;
	}
	
	if ( raw_unify_ptrs(argFrameP(blockfr, 2), rval PASS_LD) )
	{ for( ; ; FR = FR->parent )
	  { SECURE(assert(FR > blockfr));
	    discardChoicesAfter(FR PASS_LD);
	    discardFrame(FR, FINISH_CUT PASS_LD);
	    if ( FR->parent == blockfr )
	    { PC = FR->programPointer;
	      break;
	    }
	  }
					/* TBD: tracing? */
          environment_frame = FR = blockfr;
	  discardChoicesAfter(FR PASS_LD); /* delete possible CHP_DEBUG */
	  DEF = FR->predicate;
	  lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
	  ARGP = argFrameP(lTop, 0);

	  NEXT_INSTRUCTION;
	} else
	{ lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
	  ARGP = argFrameP(lTop, 0);

	  BODY_FAILED;
	}
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
!(Block).  Cuts all alternatives created after entering the named block.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(I_CUT_BLOCK) MARK(CUT_BLOCK);
      { LocalFrame cutfr;
        Choice ch;
        Word name;

        name = argFrameP(lTop, 0); deRef(name);

        if ( !(cutfr = findBlock(FR, name)) )
	{ if ( exception_term )
            goto b_throw;
	  BODY_FAILED;
	}

	for(ch=BFR; (void *)ch > (void *)cutfr; ch = ch->parent)
	{ LocalFrame fr2;

          DEBUG(3, Sdprintf("Discarding %s\n", chp_chars(ch)));
          for(fr2 = ch->frame;
              fr2 && fr2->clause && fr2 > FR;
	      fr2 = fr2->parent)
	      discardFrame(fr2, FINISH_CUT PASS_LD);
	}
        BFR = ch;

	lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
	ARGP = argFrameP(lTop, 0);

	NEXT_INSTRUCTION;
      }
#endif /*O_BLOCK*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
!. Task is to detroy all choicepoints   newer then the current frame. If
we are in debug-mode we create a   new CHP_DEBUG frame to provide proper
debugger output.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    /*i_cut:*/			/* from I_USERCALL0 */
    VMI(I_CUT)						MARK(CUT);
      { 
#ifdef O_DEBUGGER
	if ( debugstatus.debugging )
	{ Choice ch;
	  mark m;

	  switch(tracePort(FR, BFR, CUT_CALL_PORT, PC PASS_LD))
	  { case ACTION_RETRY:
	      goto retry;
	    case ACTION_FAIL:
	      FRAME_FAILED;
	  }

	  if ( (ch = findStartChoice(FR, BFR)) )
	    m = ch->mark;
	  dbg_discardChoicesAfter(FR PASS_LD);
	  lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
	  if ( ch )
	  { ch = newChoice(CHP_DEBUG, FR PASS_LD);
	    ch->mark = m;
	  }
	  ARGP = argFrameP(lTop, 0);
	  if ( exception_term )
	    goto b_throw;

	  switch(tracePort(FR, BFR, CUT_EXIT_PORT, PC PASS_LD))
	  { case ACTION_RETRY:
	      goto retry;
	    case ACTION_FAIL:
	      FRAME_FAILED;
	  }
	} else
#endif
	{ discardChoicesAfter(FR PASS_LD);
	  lTop = (LocalFrame) argFrameP(FR, CL->clause->variables);
	  ARGP = argFrameP(lTop, 0);
	  if ( exception_term )
	    goto b_throw;
	}

	NEXT_INSTRUCTION;
      }

#if O_COMPILE_OR
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
WAM support for ``A ; B'', ``A -> B'' and ``A -> B ; C'' constructs.  As
these functions introduce control within the WAM instructions  they  are
tagged `C_'.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_JMP skips the amount stated in the pointed argument.   The  PC++
could be compiled out, but this is a bit more neath.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(C_JMP) MARK(C_JMP);
      { PC += *PC;
	PC++;

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_MARK saves the value of BFR  (current   backtrack  frame) into a local
frame slot reserved by the compiler.  Note that the variable to hold the
local-frame pointer is  *not*  reserved   in  clause->variables,  so the
garbage collector won't see it.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
   VMI(C_MARK) MARK(C_MARK);
      { varFrame(FR, *PC++) = (word) BFR;

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_VAR is generated by the compiler to ensure the  instantiation  pattern
of  the  variables  is  the  same after finishing both paths of the `or'
wired in the clause.  Its task is to make the n-th variable slot of  the
current frame to be a variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
   VMI(C_VAR) MARK(C_VAR);
      { setVar(varFrame(FR, *PC++));

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_CUT will  destroy  all  backtrack  points  created  after  the  C_MARK
instruction in this clause.  It assumes the value of BFR has been stored
in the nth-variable slot of the current local frame.

We can dereference all frames that are older that the old backtrackframe
and older than this frame.

All frames created since what becomes now the  backtrack  point  can  be
discarded.

C_LCUT  results  from  !'s  encountered  in    the   condition  part  of
if->then;else and \+ (which  is  (g->fail;true)).   It  should  cut  all
choices created since the mark, but not   the mark itself. The test-case
is  a  :-  \+  (b,  !,  fail),    which   should  succeed.  The  current
implementation  walks  twice  over  the    choice-points,  but  cuts  in
conditions should be rare (I hope :-).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
      { Choice och;
	LocalFrame fr;
	Choice ch;
    VMI(C_LCUT)						MARK(C_LCUT);
	och = (Choice) varFrame(FR, *PC);
	PC++;

	for(ch=BFR; ch; ch = ch->parent)
	{ if ( ch->parent == och )
	  { och = ch;
	    goto c_cut;
	  }
	}
	assert(BFR == och);		/* no choicepoint yet */
	NEXT_INSTRUCTION;
    VMI(C_CUT) 						MARK(C_CUT);
	och = (Choice) varFrame(FR, *PC);
	PC++;				/* cannot be in macro! */
      c_cut:
	if ( !och || FR > och->frame )	/* most recent frame to keep */
	  fr = FR;
	else
	  fr = och->frame;

	for(ch=BFR; ch && ch > och; ch = ch->parent)
	{ LocalFrame fr2;

	  DEBUG(3, Sdprintf("Discarding %s\n", chp_chars(ch)));
	  for(fr2 = ch->frame;    
	      fr2 && fr2->clause && fr2 > fr;
	      fr2 = fr2->parent)
	  { discardFrame(fr2, FINISH_CUT PASS_LD);
	    if ( exception_term )
	      goto b_throw;
	  }
	}
	assert(och == ch);
	BFR = och;

	if ( (void *)och > (void *)fr )
	{ lTop = addPointer(och, sizeof(*och));
	} else
	{ int nvar = (true(fr->predicate, FOREIGN)
				? fr->predicate->functor->arity
				: fr->clause->clause->variables);
	  lTop = (LocalFrame) argFrameP(fr, nvar);
	}

	ARGP = argFrameP(lTop, 0);

	DEBUG(3, Sdprintf(" --> BFR = #%ld, lTop = #%ld\n",
			  loffset(BFR), loffset(lTop)));
        NEXT_INSTRUCTION;
      }

#ifdef O_SOFTCUT
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Handle the commit-to of A *-> B; C.  Simply mark the $alt/1 frame as cutted,
and control will not reach C again.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(C_SOFTCUT) MARK(CSOFTCUT);
      { Choice ch = (Choice) varFrame(FR, *PC);

	PC++;
	ch->type = CHP_NONE;
	NEXT_INSTRUCTION;
      }
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_END is a dummy instruction to help the decompiler to find the end of A
->  B.  (Note  that  a  :-  (b  ->  c),  d == a :- (b -> c, d) as far as
semantics.  They are different terms however.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
   VMI(C_END) MARK(C_END);
      {	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_FAIL is equivalent to fail/0. Used to implement \+/1.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
   VMI(C_FAIL) MARK(C_FAIL);
      {	BODY_FAILED;
      }
#endif /* O_COMPILE_OR */

#if O_COMPILE_ARITH
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Arithmic is compiled using a stack machine.   The  stack is allocated in
LD->arith.stack and manipulated through  the functions allocArithStack()
and friends.

Arguments to functions are pushed on the stack  starting  at  the  left,
thus `add1(X, Y) :- Y is X + 1' translates to:

    I_ENTER	% enter body
    B_VAR1	% push Y via ARGP
    A_ENTER	% align the stack to prepare for writing doubles
    A_VAR0	% evaluate X and push numeric result
    A_INTEGER 1	% Push 1 as numeric value
    A_FUNC2 0	% Add top-two of the stack and push result
    A_IS 	% unify Y with numeric result
    I_EXIT	% leave the clause

a_func0:	% executes arithmic function without arguments, pushing
		% its value on the stack
a_func1:	% unary function. Changes the top of the stack.
a_func2:	% binary function. Pops two values and pushes one.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(A_ENTER) MARK(AENTER)
      { AR_BEGIN();
	NEXT_INSTRUCTION;
      }

    VMI(A_INTEGER) MARK(AINT);
      {	Number n = allocArithStack(PASS_LD1);

	n->value.i = (intptr_t) *PC++;
	n->type    = V_INTEGER;
	NEXT_INSTRUCTION;
      }

    VMI(A_INT64) MARK(AINT64);
      {	Number n = allocArithStack(PASS_LD1);
	Word p = &n->value.w[0];

	cpInt64Data(p, PC);
	n->type    = V_INTEGER;
	NEXT_INSTRUCTION;
      }

    VMI(A_MPZ) MARK(AMPZ);		/* see globalMPZ() and compiler */
#ifdef O_GMP
      {	Number n = allocArithStack(PASS_LD1);
	Word p = (Word)PC;
	int size;

	p++;				/* skip indirect header */
	n->type = V_MPZ;
	n->value.mpz->_mp_size  = (int)*p++;
	n->value.mpz->_mp_alloc = 0;	/* avoid de-allocating */
	size = sizeof(mp_limb_t) * abs(n->value.mpz->_mp_size);
	n->value.mpz->_mp_d = (void*)p;

	p += (size+sizeof(word)-1)/sizeof(word);
 	PC = (Code)p;
	NEXT_INSTRUCTION;
      }
#else
	NEXT_INSTRUCTION;
#endif

    VMI(A_DOUBLE) MARK(ADOUBLE);
      {	Number n = allocArithStack(PASS_LD1);
	Word p = &n->value.w[0];

	cpDoubleData(p, PC);
	n->type       = V_REAL;
	NEXT_INSTRUCTION;
      }

    VMI(A_VAR) MARK(AVARN);
    { int offset;
      Number n;
      Word p, p2;

      offset = (int)*PC++;

    a_var_n:
      p = varFrameP(FR, offset);
      deRef2(p, p2);

      switch(tag(*p2))
      { case TAG_INTEGER:
	  n = allocArithStack(PASS_LD1);
	  get_integer(*p2, n);
	  NEXT_INSTRUCTION;
	case TAG_FLOAT:
	  n = allocArithStack(PASS_LD1);
	  n->value.f = valReal(*p2);
	  n->type = V_REAL;
	  NEXT_INSTRUCTION;
        default:
	{ intptr_t lsafe = (char*)lTop - (char*)lBase;
	  fid_t fid;
	  number result;
	  int rc;

	  lTop = (LocalFrame)argFrameP(lTop, 1); /* for is/2.  See below */
	  fid = PL_open_foreign_frame();
	  rc = valueExpression(consTermRef(p), &result PASS_LD);
	  PL_close_foreign_frame(fid);
	  lTop = addPointer(lBase, lsafe);

	  if ( rc )
	  { pushArithStack(&result PASS_LD);
	    NEXT_INSTRUCTION;
	  } else
	  { resetArithStack(PASS_LD1);
#if O_CATCHTHROW
            if ( exception_term )
	      goto b_throw;
#endif
	    BODY_FAILED;		/* check this */
	  }
	}
      }

    VMI(A_VAR0) MARK(AVAR0);
      offset = ARGOFFSET / sizeof(word);
      goto a_var_n;
    VMI(A_VAR1) MARK(AVAR1);
      offset = ARGOFFSET / sizeof(word) + 1;
      goto a_var_n;
    VMI(A_VAR2) MARK(AVAR2);
      offset = ARGOFFSET / sizeof(word) + 2;
      goto a_var_n;
    }

  { int an;
    code fn;

    VMI(A_FUNC0) MARK(A_FUNC0);
      {	an = 0;
	fn = *PC++;
	goto common_an;
      }

    VMI(A_FUNC1) MARK(A_FUNC1);
      {	an = 1;
	fn = *PC++;
	goto common_an;
      }

    VMI(A_FUNC2) MARK(A_FUNC2);
      {	an = 2;
	fn = *PC++;
	goto common_an;
      }

    VMI(A_FUNC) MARK(A_FUNC);
      {	fn = *PC++;
	an = (int) *PC++;

      common_an:
	if ( !ar_func_n((int)fn, an PASS_LD) )
	{ resetArithStack(PASS_LD1);

	  if ( exception_term )
	    goto b_throw;
	  BODY_FAILED;
	}

	NEXT_INSTRUCTION;
      }
  }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Translation of the arithmic comparison predicates (<, >, =<,  >=,  =:=).
Both sides are pushed on the stack, so we just compare the two values on
the  top  of  this  stack  and  backtrack  if  they  do  not suffice the
condition.  Example translation: `a(Y) :- b(X), X > Y'

    ENTER
    B_FIRSTVAR 1	% Link X from B's frame to a new var in A's frame
    CALL 0		% call b/1
    A_VAR 1		% Push X
    A_VAR 0		% Push Y
    A_GT		% compare
    EXIT
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  { int cmp;
    Number n;
    int rc;

    VMI(A_LT) MARK(A_LT);
        cmp = LT;
      acmp:				/* common entry */
	n = argvArithStack(2 PASS_LD);
	rc = ar_compare(n, n+1, cmp);
	popArgvArithStack(2 PASS_LD);
	AR_END();
	if ( rc )
	  NEXT_INSTRUCTION;
	BODY_FAILED;
    VMI(A_LE) MARK(A_LE); cmp = LE; goto acmp;
    VMI(A_GT) MARK(A_GT); cmp = GT; goto acmp;
    VMI(A_GE) MARK(A_GE); cmp = GE; goto acmp;
    VMI(A_EQ) MARK(A_EQ); cmp = EQ; goto acmp;
    VMI(A_NE) MARK(A_NE); cmp = NE; goto acmp;
  }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Translation of is/2.  The stack has two pushed values: the variable for
the result (a word) and the number holding the result.  For example:

	 a(X) :- X is sin(3).

	I_ENTER
	B_VAR 0			push left argument of is/2
	A_INTEGER 3		push integer as number
	A_FUNC <sin>		run function on it
	A_IS			bind value
	I_EXIT

Note that the left argument is pushed onto the local stack and therefore
we need to set the  local  stack   pointer  before  doing a call-back to
Prolog.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(A_IS) MARK(A_IS);
      { Number n = argvArithStack(1 PASS_LD);
	Word k;

	ARGP = argFrameP(lTop, 0);	/* 1-st argument */
	deRef2(ARGP, k);

	if ( canBind(*k) )
	{ word c = put_number(n);	/* can shift */

	  popArgvArithStack(1 PASS_LD);
	  AR_END();
#ifdef O_SHIFT_STACKS
	  ARGP = argFrameP(lTop, 0);
#endif
	  deRef2(ARGP, k);
	  bindConst(k, c);
#ifdef O_ATTVAR
	  if ( *valTermRef(LD->attvar.head) ) /* can be faster */
	    goto wakeup;
#endif
	  NEXT_INSTRUCTION;
	} else
	{ int rc;

	  if ( isInteger(*k) && intNumber(n) )
	  { number left;

	    get_integer(*k, &left);
	    rc = (cmpNumbers(&left, n) == 0);
	    clearNumber(&left);
	  } else if ( isReal(*k) && floatNumber(n) )
	  { rc = (valReal(*k) == n->value.f);
	  } else
	  { rc = FALSE;
	  }

	  popArgvArithStack(1 PASS_LD);
	  AR_END();
	  if ( rc )
	    NEXT_INSTRUCTION;
	}

	BODY_FAILED;
      }
#endif /* O_COMPILE_ARITH */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_USERCALL0 is generated by the compiler if a variable is encountered as
a subclause. Note that the compount   statement  opened here is encloses
also I_APPLY and I_CALL. This allows us to use local register variables,
but still jump to the `normal_call' label to   do the common part of all
these three virtual machine instructions.

I_USERCALL0 has the task of  analysing  the   goal:  it  should fill the
->procedure slot of the new frame and  save the current program counter.
It also is responsible of filling the   argument part of the environment
frame with the arguments of the term.

BUG: have to find out how to proceed in case of failure (I am afraid the
`goto frame_failed' is a bit dangerous here).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
#if O_CATCHTHROW
    i_usercall0:			/* from B_THROW */
#endif
    VMI(I_USERCALL0) MARK(USRCL0);
      { word goal;
	int arity;
	Word args, a;
	int n;
	LocalFrame next;
	Module module;
	functor_t functor;
	int callargs;

	module = NULL;
	next = lTop;
	a = argFrameP(next, 0);		/* get the goal */
	a = stripModule(a, &module PASS_LD);
	

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Determine the functor definition associated with the goal as well as the
arity and a pointer to the argument vector of the goal.

If the goal is not a  simple  goal,   the  body  is  compiled to `local'
clause. The compilation mode is presented to compileClause() by the NULL
`head'. This compilation-mode  constructs  a   stack-frame  whose  first
argument is the  goal-term,  followed   by  large  structures (compound,
string) from the arguments, followed  by   the  normal  local variables,
followed by the VM codes and, the   clause structure and finally a dummy
list for the clause-chain  (ClauseRef)  used   for  the  frames ->clause
field.

The clause data is discarded automatically  if the frame is invalidated.
Note that compilation does not give contained   atoms a reference as the
atom is referenced by the goal-term anyway.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	if ( isTextAtom(goal = *a) )
	{ /*if ( *a == ATOM_cut )		NOT ISO
	    goto i_cut; */
	  functor = lookupFunctorDef(goal, 0);
	  arity   = 0;
	  args    = NULL;
	} else if ( isTerm(goal) )
	{ if ( isSimpleGoal(a PASS_LD)
#if O_DEBUG
	       || GD->bootsession || !GD->initialised
#endif
	     )
	  { args    = argTermP(goal, 0);
	    functor = functorTerm(goal);
	    arity   = arityFunctor(functor);
	  } else
	  { Clause cl;
	    
	    a = &goal;			/* we're going to overwrite */
	    deRef(a);
	    DEBUG(1, { term_t g = a - (Word)lBase;
		       LocalFrame ot = lTop;
		       lTop += 100;
		       pl_write(g); pl_nl();
		       lTop = ot;
		     });
	    lTop = next;
	    if ( !(cl = compileClause(NULL, a, PROCEDURE_dcall1,
				      module PASS_LD)) )
	      goto b_throw;

	    DEF			 = next->predicate;
	    SECURE(assert(DEF == PROCEDURE_dcall1->definition));
	    next->flags	         = FR->flags;
	    next->parent	 = FR;
	    next->programPointer = PC;
#ifdef O_PROFILE
	    next->prof_node      = FR->prof_node;	    
#endif
#ifdef O_LOGICAL_UPDATE
	    cl->generation.erased = ~0L;
	    cl->generation.created = next->generation = GD->generation;
#endif
	    incLevel(next);
	    PC = cl->codes;
  
	    enterDefinition(DEF);
	    environment_frame = FR = next;
	    ARGP = argFrameP(lTop, 0);

	    NEXT_INSTRUCTION;
	  }
	} else
	{ fid_t fid;

	  lTop = (LocalFrame)argFrameP(next, 1);
	  fid = PL_open_foreign_frame();
	  PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_callable, wordToTermRef(argFrameP(next, 0)));
	  PL_close_foreign_frame(fid);
	  goto b_throw;
	}
	goto i_usercall_common;

    VMI(I_USERCALLN) MARK(USRCLN);
        callargs = (int)*PC++;
	next = lTop;
	a = argFrameP(next, 0);		/* get the (now) instantiated */
	deRef(a);			/* variable */

	module = NULL;
	a = stripModule(a, &module PASS_LD);

	if ( isTextAtom(goal = *a) )
	{ arity   = 0;
	  functor = lookupFunctorDef(goal, callargs);
	  args    = NULL;
	} else if ( isTerm(goal) )
	{ FunctorDef fdef = valueFunctor(functorTerm(goal));

	  arity   = fdef->arity;
	  functor = lookupFunctorDef(fdef->name, arity + callargs);
	  args    = argTermP(goal, 0);
	} else
	{ lTop = (LocalFrame)argFrameP(next, 1);
	  PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_callable, wordToTermRef(argFrameP(next, 0)));
	  goto b_throw;
	}

	if ( arity != 1 )
	{ int i, shift = arity - 1;

	  a = argFrameP(next, 1);	/* pointer to 1-st arg */
	  
	  if ( shift > 0 )
	  { for(i=callargs-1; i>=0; i--)
	    { if ( isRef(a[i]) )
	      { Word a1 = unRef(a[i]);
	    
		if ( a1 >= a && a1 < a+arity )
		  a[i+shift] = makeRef(a1+shift);
		else
		  a[i+shift] = a[i];
	      } else
		a[i+shift] = a[i];
	    }
	  } else
	  { for(i=0; i < callargs; i++)
	    { if ( isRef(a[i]) )
	      { Word a1 = unRef(a[i]);
		
		if ( a1 >= a && a1 < a+arity )
		  a[i+shift] = makeRef(a1+shift);
		else
		  a[i+shift] = a[i];
	      } else
		a[i+shift] = a[i];
	    }
	  }
	}

    i_usercall_common:
	next->flags = FR->flags;
	if ( true(DEF, HIDE_CHILDS) )
	  set(next, FR_NODEBUG);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Now scan the argument vector of the goal and fill the arguments  of  the
frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	if ( arity > 0 )
	{ ARGP = argFrameP(next, 0);

	  for(; arity-- > 0; ARGP++, args++)
	    *ARGP = linkVal(args);
	}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Find the associated procedure.  First look in the specified module.   If
the function is not there then look in the user module.  Finally specify
the context module environment for the goal. This is not necessary if it
will  be  specified  correctly  by  the goal started.  Otherwise tag the
frame and write  the  module  name  just  below  the  frame.   See  also
contextModule().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	SAVE_REGISTERS(qid);
	DEF = getProcDefinedDefinition(&next, PC,
				       resolveProcedure(functor, module)
				       PASS_LD);
	LOAD_REGISTERS(qid);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Save the program counter (note  that   I_USERCALL0  has no argument) and
continue as with a normal call.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	next->context = module;
	goto normal_call;
	
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Fast control functions. Should  set-up  normal   call  if  the  function
doesn't exist.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(I_FAIL) MARK(I_FAIL);
#ifdef O_DEBUGGER
      if ( debugstatus.debugging )
      { next = lTop;
	next->flags = FR->flags;
	if ( true(DEF, HIDE_CHILDS) ) /* parent has hide_childs */
	  set(next, FR_NODEBUG);
	DEF = lookupProcedure(FUNCTOR_fail0, MODULE_system)->definition;
	next->context = FR->context;

	goto normal_call;
      }
#endif
      BODY_FAILED;

    VMI(I_TRUE) MARK(I_TRUE);
#ifdef O_DEBUGGER
      if ( debugstatus.debugging )
      { next = lTop;
	next->flags = FR->flags;
	if ( true(DEF, HIDE_CHILDS) ) /* parent has hide_childs */
	  set(next, FR_NODEBUG);
	DEF = lookupProcedure(FUNCTOR_true0, MODULE_system)->definition;
	next->context = FR->context;

	goto normal_call;
      }
#endif
      NEXT_INSTRUCTION;

#if O_COMPILE_OR
#ifdef O_SOFTCUT
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
A *-> B ; C is translated to C_SOFIF <A> C_SOFTCUT <B> C_JMP end <C>.  See
pl-comp.c and C_SOFTCUT implementation for details.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(C_SOFTIF) MARK(C_SOFTIF);
      { varFrame(FR, *PC++) = (word) lTop; /* see C_SOFTCUT */

	goto c_or;
      }

#endif
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
If-then-else is a contraction of C_MARK and C_OR.  This contraction  has
been  made  to help the decompiler distinguis between (a ; b) -> c and a
-> b ; c, which would otherwise only be  possible  to  distinguis  using
look-ahead.

The asm("nop") is a tricky. The problem   is that C_NOT and C_IFTHENELSE
are the same instructions. The one is generated on \+/1 and the other on
(Cond -> True ; False). Their different   virtual-machine  id is used by
the decompiler. Now, as the VMCODE_IS_ADDRESS   is  in effect, these two
instruction would become the same. The  asm("nop") ensures they have the
same *functionality*, but a *different* address.  If your machine does't
like nop, define the macro ASM_NOP in  your md-file to do something that
1) has *no effect* and 2) is *not optimised* away by the compiler.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(C_NOT)						MARK(C_NOT)
      SEPERATE_VMI;
    VMI(C_IFTHENELSE)
      MARK(C_ITE);
      { varFrame(FR, *PC++) = (word) BFR; /* == C_MARK */

	/*FALL-THROUGH to C_OR*/
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
C_OR introduces a backtrack point within the clause.   The  argument  is
how  many  entries  of  the  code  array  to skip should backtracking be
necessary.  It is implemented by calling a foreign  functions  predicate
with as argument the amount of bytes to skip.  The foreign function will
on  first  call  succeed,  leaving  a  backtrack  point.   It does so by
returning the amount to skip as backtracking  argument.   On  return  it
will increment PC in its frame with this amount (which will be popped on
its exit) and succeed deterministically.

Note that this one is enclosed in the compound statement of I_USERCALL0,
I_APPLY, I_CALL and I_DEPART to allow   sharing of the register variable
`next' with them and thus make the `goto common_call' valid.

NOTE: as of SWI-Prolog 2.0.2, the  call   to  $alt/1  is `inlined'. As a
consequence it has lost its argument and   is  now $alt/0. We just build
the frame for $alt/1 and then  continue   execution.  This  is ok as the
first call of $alt/1 simply succeeds.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    VMI(C_OR) MARK(C_OR);
    c_or:
      { size_t skip = *PC++;
	Choice ch = newChoice(CHP_JUMP, FR PASS_LD);
	ch->value.PC = PC+skip;
	ARGP = argFrameP(lTop, 0);

	NEXT_INSTRUCTION;
      }
#endif /* O_COMPILE_OR */

#ifdef O_INLINE_FOREIGNS
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CALL_FV[012] Call a deterministic foreign procedures with a 0, 1, or 2
arguments that appear as variables  in   the  clause.  This covers true,
fail, var(X) and other type-checking  predicates,   =/2  in  a number of
cases (i.e. X = Y, not X = 5).

The VMI for these calls are ICALL_FVN, proc, var-index ...
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    { int nvars;
      Procedure fproc;
      Word v;

      VMI(I_CALL_FV0) MARK(CFV0);
      { fproc = (Procedure) *PC++;
	nvars = 0;

	goto common_call_fv;
      }

      VMI(I_CALL_FV1) MARK(CFV1);
      { fproc = (Procedure) *PC++;
	nvars = 1;
	v = varFrameP(FR, *PC++);
	*ARGP++ = (needsRef(*v) ? makeRefL(v) : *v);
	goto common_call_fv;
      }

      VMI(I_CALL_FV2) MARK(CFV2);
      { fproc = (Procedure) *PC++;
	nvars = 2;
	v = varFrameP(FR, *PC++);
	*ARGP++ = (needsRef(*v) ? makeRefL(v) : *v);
	v = varFrameP(FR, *PC++);
	*ARGP++ = (needsRef(*v) ? makeRefL(v) : *v);

      common_call_fv:
	{ Definition def;
	  Func f;
	  word rval;

	  next = lTop;
	  SAVE_REGISTERS(qid);
	  def = getProcDefinedDefinition(&next, PC, fproc PASS_LD);
	  LOAD_REGISTERS(qid);
	  f = def->definition.function;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
If we are debugging, just build a normal  frame and do the normal thing,
so the inline call is expanded to a normal call and may be traced.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	  if ( !f ||
#ifdef O_DEBUGGER
	       debugstatus.debugging ||
#endif
	       false(def, FOREIGN) )
	  { next->flags = FR->flags;
	    if ( true(DEF, HIDE_CHILDS) ) /* parent has hide_childs */
	      set(next, FR_NODEBUG);
	    DEF = def;
	    next->context = FR->context;

	    goto normal_call;
	  } else
	  { intptr_t oldtop = (char*)lTop - (char*)lBase;
	    term_t h0;
	    fid_t fid;
	
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
We must create a frame and mark the  stacks for two reasons: undo if the
foreign call fails *AND*  make  sure   Trail()  functions  properly.  We
increase lTop too to prepare for asynchronous interrupts.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	    LD->statistics.inferences++;
	    h0 = argFrameP(next, 0) - (Word)lBase;
	    lTop = (LocalFrame) argFrameP(next, nvars);
	    if ( true(def, METAPRED) )
	      next->context = FR->context;
	    else
	      next->context = def->module;
	    next->predicate      = def;
	    next->programPointer = PC;
	    next->parent         = FR;
	    next->flags		 = FR->flags;
  	    next->clause	 = NULL; /* for handling exceptions */
#ifdef O_LOGICAL_UPDATE
	    next->generation     = GD->generation;
#endif
	    incLevel(next);
#ifdef O_PROFILE	
	    if ( LD->profile.active )
	      next->prof_node = profCall(def PASS_LD);
	    else
	      next->prof_node = NULL;
#endif
	    environment_frame = next;

	    exception_term = 0;
	    SAVE_REGISTERS(qid);
	    fid = PL_open_foreign_frame();
	    if ( is_signalled(PASS_LD1) )
	    { PL_handle_signals();
	      LOAD_REGISTERS(qid);
	      if ( exception_term )
	      { PL_close_foreign_frame(fid);
		goto b_throw;
	      }
	    }
	    END_PROF();
	    START_PROF(PROF_FOREIGN, "PROF_FOREIGN");
	    if ( true(def, P_VARARG) )
	    { struct foreign_context ctx;
	      ctx.context = 0;
	      ctx.control = FRG_FIRST_CALL;
	      ctx.engine  = LD;

	      rval = (*f)(h0, nvars, &ctx);
	    } else
	    { switch(nvars)
	      { case 0:
		  rval = (*f)();
		  break;
		case 1:
		  rval = (*f)(h0);
		  break;
		case 2:
		default:
		  rval = (*f)(h0, h0+1);
		  break;
	      }
	    }
	    PL_close_foreign_frame(fid);
	    LOAD_REGISTERS(qid);

	    environment_frame = FR;
	    lTop = addPointer(lBase, oldtop);
	    ARGP = argFrameP(lTop, 0);

	    if ( exception_term )
	    { if ( rval )
	      { exception_term = 0;
		setVar(*valTermRef(exception_bin));
	      } else
		goto b_throw;
	    }

	    if ( rval )
	    { assert(rval == TRUE);
	      Profile(profExit(FR->prof_node PASS_LD));

#ifdef O_ATTVAR
	      if ( *valTermRef(LD->attvar.head) ) /* can be faster */
		goto wakeup;
#endif
	      NEXT_INSTRUCTION;
	    }

	    LD->statistics.inferences++;	/* is a redo! */
	    BODY_FAILED;
	  }
	}
      }
    }
#endif /*O_INLINE_FOREIGNS*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_APPLY is the code generated by the Prolog goal $apply/2 (see reference
manual for the definition of apply/2).  We   expect  a term in the first
argument of the frame and a  list   in  the second, comtaining aditional
arguments. Most comments of I_USERCALL0 apply   to I_APPLY as well. Note
that the two arguments are copied in  local variables as they will later
be overwritten by the arguments for the actual call.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
      VMI(I_APPLY) MARK(APPLY);
      { Word lp;
	Word gp;
	word a1 = 0;

	module = NULL;
	next = lTop;
	next->flags = FR->flags;
	if ( true(DEF, HIDE_CHILDS) )
	  set(next, FR_NODEBUG);

	ARGP = argFrameP(next, 0); deRef(ARGP); gp = ARGP;
	ARGP = argFrameP(next, 1); deRef(ARGP); lp = ARGP;
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Obtain the functor of the actual goal from the first argument  and  copy
the arguments of this term in the frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	
	gp = stripModule(gp, &module PASS_LD);
	next->context = module;
	goal = *gp;

	ARGP = argFrameP(next, 0);

	if ( isTextAtom(goal) )
	{ functor = goal;
	  arity = 0;
	} else if ( isTerm(goal) )
	{ Functor     f = valueTerm(goal);
	  FunctorDef fd = valueFunctor(f->definition);

	  functor = fd->name;
	  arity   = fd->arity;
	  args    = f->arguments;
	  for(n=0; n<arity; n++, ARGP++, args++)
	  { if ( n == 1 )
	      a1 = linkVal(args);
	    else
	      *ARGP = linkVal(args);
	  }
	} else
	{ lTop = (LocalFrame)argFrameP(next, 2);
	  PL_error("apply", 2, NULL, ERR_TYPE,
		   ATOM_callable, wordToTermRef(argFrameP(next, 0)));
	  goto b_throw;
	}
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Scan the list and add the elements to the argument vector of the frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	for( ;isList(*lp); ARGP++ )
	{ args = argTermP(*lp, 0);	/* i.e. the head */
	  if ( arity++ == 1 )
	    a1 = linkVal(args);
	  else
	    *ARGP = linkVal(args);
	  if ( arity > MAXARITY )
	  { PL_error("apply", 2, NULL, ERR_REPRESENTATION, ATOM_max_arity);
	    goto b_throw;
	  }
	  lp = argTermP(*lp, 1);	/* i.e. the tail */
	  deRef(lp);
	}
	if ( !isNil(*lp) )
	{ lTop = (LocalFrame)argFrameP(next, 2);
	  PL_error("apply", 2, NULL, ERR_TYPE,
		   ATOM_list, wordToTermRef(argFrameP(next, 1)));
	  goto b_throw;
	}
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 
Finally do the delayed assignment of a1 (delayed to avoid overwriting the
argument list).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	if ( a1 )
	  argFrame(next, 1) = a1;
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Find the associated procedure (see I_CALL for module handling), save the
program pointer and jump to the common part.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	{ functor_t fdef;

	  fdef = lookupFunctorDef(functor, arity);
	  SAVE_REGISTERS(qid);
	  DEF = getProcDefinedDefinition(&next, PC,
					 resolveProcedure(fdef, module)
					 PASS_LD);
	  LOAD_REGISTERS(qid);
	  next->context = module;
	}

	goto normal_call;
      }

#ifdef O_ATTVAR
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Attributed variable handling
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
      wakeup:
	DEBUG(1, Sdprintf("Activating wakeup\n"));
	next = lTop;
	next->context = MODULE_system;
        next->flags = FR->flags;
        SAVE_REGISTERS(qid);
        DEF = getProcDefinedDefinition(&next, PC,
				       PROCEDURE_dwakeup1
				       PASS_LD);
	LOAD_REGISTERS(qid);
        ARGP = argFrameP(next, 0);
	ARGP[0] = *valTermRef(LD->attvar.head);
	setVar(*valTermRef(LD->attvar.head));
	setVar(*valTermRef(LD->attvar.tail));

	goto normal_call;
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
I_CALL and I_DEPART form the normal code generated by the  compiler  for
calling  predicates.   The  arguments  are  already written in the frame
starting at `lTop'.  I_DEPART implies it is the last  subclause  of  the
clause.  This is be the entry point for tail recursion optimisation.

The task of I_CALL is to  save  necessary  information  in  the  current
frame,  fill  the next frame and initialise the machine registers.  Then
execution can continue at `next_instruction'
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
#define LASTCALL 1
      VMI(I_DEPART)
							 MARK(DEPART);
#if LASTCALL
	if ( (void *)BFR <= (void *)FR
#if O_DEBUGGER
	     && trueFeature(LASTCALL_FEATURE)
#endif
	   )
	{ Procedure proc = (Procedure) *PC++;
	  Definition ndef;

	  SAVE_REGISTERS(qid);
	  ndef = getProcDefinedDefinition(&lTop, PC, proc PASS_LD);
	  LOAD_REGISTERS(qid);
	  arity = ndef->functor->arity;

	  if ( true(FR, FR_WATCHED) )
	  { LocalFrame lSave = lTop;
	    lTop = (LocalFrame)argFrameP(lTop, arity);
	    frameFinished(FR, FINISH_EXIT PASS_LD);
	    lTop = lSave;
	  }

	  if ( DEF )
	  { if ( true(DEF, HIDE_CHILDS) )
	      set(FR, FR_NODEBUG);
	    leaveDefinition(DEF);
	  }

	  FR->clause = NULL;		/* for save atom-gc */
	  FR->predicate = DEF = ndef;
	  copyFrameArguments(lTop, FR, arity PASS_LD);

	  goto depart_continue;
	}
#endif /*LASTCALL*/
	/*FALLTHROUGH*/
      VMI(I_CALL)					MARK(CALL);
        next = lTop;
        next->flags = FR->flags;
	if ( true(DEF, HIDE_CHILDS) ) /* parent has hide_childs */
	  set(next, FR_NODEBUG);
	next->context = FR->context;
	{ Procedure proc = (Procedure) *PC++;
	  SAVE_REGISTERS(qid);
	  DEF = getProcDefinedDefinition(&next, PC, proc PASS_LD);
	  LOAD_REGISTERS(qid);
	}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This is the common part of the call variations.  By now the following is
true:

  - arguments, nodebug		filled
  - context			filled with context for
				transparent predicate
  - DEF				filled
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

      normal_call:
	requireStack(local, (size_t)argFrameP((LocalFrame)NULL, MAXARITY));

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Initialise those slots of the frame that are common to Prolog predicates
and foreign ones.  There might be some possibilities for optimisation by
delaying these initialisations till they are really  needed  or  because
the information they are calculated from is destroyed.  This probably is
not worthwile.

Note: we are working above `lTop' here!!   We restore this as quickly as
possible to be able to call-back to Prolog.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	next->parent         = FR;
	next->predicate	     = DEF;		/* TBD */
	next->programPointer = PC;		/* save PC in child */
	next->clause         = NULL;		/* for save atom-gc */
	environment_frame = FR = next;		/* open the frame */

      depart_continue:
	incLevel(FR);
#ifdef O_LOGICAL_UPDATE
	FR->generation     = GD->generation;
#endif
      retry_continue:
        lTop = (LocalFrame) argFrameP(FR, DEF->functor->arity);

#ifdef O_DEBUGLOCAL
      {	Word ap = argFrameP(FR, DEF->functor->arity);
	int n;
	
	for(n=50; --n; )
	  *ap++ = (word)(((char*)ATOM_nil) + 1);
      }
#endif

	clear(FR, FR_SKIPPED|FR_WATCHED|FR_CATCHED);

	if ( is_signalled(PASS_LD1) )
	{ SAVE_REGISTERS(qid);
	  PL_handle_signals();
	  LOAD_REGISTERS(qid);
	  if ( exception_term )
	  { CL = NULL;

	    enterDefinition(DEF);
					/* The catch is not yet installed, */
					/* so we ignore it */
	    if ( FR->predicate == PROCEDURE_catch3->definition )
	      set(FR, FR_CATCHED);

	    goto b_throw;
	  }
	}

#if O_ASYNC_HOOK			/* Asynchronous hooks */
	if ( async.hook &&
	     !((++LD->statistics.inferences & async.mask)) )
	  (*async.hook)();		/* check the hook */
	else
#endif
	  LD->statistics.inferences++;


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Undefined predicate detection and handling.   trapUndefined() takes care
of linking from the public modules or calling the exception handler.

Note that DEF->definition is  a  union   of  the  clause  or C-function.
Testing is suffices to find out that the predicate is defined.

Logical-update: note that trapUndefined() may add  clauses and we should
be able to access these!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

	if ( !DEF->definition.clauses && false(DEF, PROC_DEFINED) &&
	     true(DEF->module, UNKNOWN_ERROR) )
	{ FR->clause = NULL;
	  if ( exception_term )		/* left by trapUndefined() */
	  { enterDefinition(DEF);	/* will be left in exception code */
	    goto b_throw;
	  }
	}

#ifdef O_PROFILE	
	if ( LD->profile.active )
	  FR->prof_node = profCall(DEF PASS_LD);
	else
	  FR->prof_node = NULL;
#endif

	if ( false(DEF, METAPRED) )
	  FR->context = DEF->module;
	if ( false(DEF, HIDE_CHILDS) )	/* was SYSTEM */
	  clear(FR, FR_NODEBUG);

#if O_DYNAMIC_STACKS
	if ( gc_status.requested )
	{ garbageCollect(FR, BFR);
	}
#else /*O_DYNAMIC_STACKS*/
#if O_SHIFT_STACKS
      { int gshift = narrowStack(global);
	int lshift = narrowStack(local);
	int tshift = narrowStack(trail);

	if ( gshift || lshift || tshift )
	{ if ( gshift || tshift )
	  { intptr_t gused = usedStack(global);
	    intptr_t tused = usedStack(trail);

	    garbageCollect(FR, BFR);
	    DEBUG(1, Sdprintf("\tgshift = %d; tshift = %d", gshift, tshift));
	    if ( gshift )
	      gshift = ((2 * usedStack(global)) > gused);
	    if ( tshift )
	      tshift = ((2 * usedStack(trail)) > tused);
	    DEBUG(1, Sdprintf(" --> gshift = %d; tshift = %d\n",
			    gshift, tshift));
	  }

	  if ( gshift || tshift || lshift )
	  { SAVE_REGISTERS(qid);
	    growStacks(FR, BFR, NULL, lshift, gshift, tshift);
	    LOAD_REGISTERS(qid);
	  }
	}
      }
#else /*O_SHIFT_STACKS*/
	if ( narrowStack(global) || narrowStack(trail) )
	  garbageCollect(FR);
#endif /*O_SHIFT_STACKS*/
#endif /*O_DYNAMIC_STACKS*/

	if ( LD->outofstack )
	{ enterDefinition(DEF);		/* exception will lower! */
	  outOfStack(LD->outofstack, STACK_OVERFLOW_RAISE);
	  goto b_throw;
	}

	if ( true(DEF, FOREIGN) )
	{ int rval;

	  SAVE_REGISTERS(qid);
	  FR->clause = NULL;
	  END_PROF();
	  START_PROF(PROF_FOREIGN, "PROF_FOREIGN");
	  rval = callForeign(FR, FRG_FIRST_CALL PASS_LD);
	  LOAD_REGISTERS(qid);

	  if ( rval )
	    goto exit_builtin;

#if O_CATCHTHROW
	  if ( exception_term )
	  { goto b_throw;
	  }
#endif

	  goto frame_failed;
	} 

#if O_DEBUGGER
	if ( debugstatus.debugging )
	{ CL = DEF->definition.clauses;
	  set(FR, FR_INBOX);
	  switch(tracePort(FR, BFR, CALL_PORT, NULL PASS_LD))
	  { case ACTION_FAIL:	goto frame_failed;
	    case ACTION_IGNORE: goto exit_builtin;
	  }
	}
#endif /*O_DEBUGGER*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Call a normal Prolog predicate.  Just   load  the machine registers with
values found in the clause,  give  a   reference  to  the clause and set
`lTop' to point to the first location after the current frame.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
	ARGP = argFrameP(FR, 0);
	enterDefinition(DEF);

#ifdef O_LIMIT_DEPTH
      { uintptr_t depth = levelFrame(FR);

	if ( depth > depth_reached )
	  depth_reached = depth;
	if ( depth > depth_limit )
	{ DEBUG(2, Sdprintf("depth-limit\n"));

	  if ( debugstatus.debugging )
	    newChoice(CHP_DEBUG, FR PASS_LD);
	  FRAME_FAILED;
	}
      }
#endif
	DEBUG(9, Sdprintf("Searching clause ... "));

      { ClauseRef nextcl;
	Clause clause;

	lTop = (LocalFrame) argFrameP(FR, DEF->functor->arity);
	if ( !(CL = firstClause(ARGP, FR, DEF, &nextcl PASS_LD)) )
	{ DEBUG(9, Sdprintf("No clause matching index.\n"));
	  if ( debugstatus.debugging )
	    newChoice(CHP_DEBUG, FR PASS_LD);

	  FRAME_FAILED;
	}
	DEBUG(9, Sdprintf("Clauses found.\n"));

	clause = CL->clause;
	PC = clause->codes;
	lTop = (LocalFrame)(ARGP + clause->variables);

	if ( nextcl )
	{ Choice ch = newChoice(CHP_CLAUSE, FR PASS_LD);
	  ch->value.clause = nextcl;
	} else if ( debugstatus.debugging )
	  newChoice(CHP_DEBUG, FR PASS_LD);

			/* require space for the args of the next frame */
	requireStack(local, (size_t)argFrameP((LocalFrame)NULL, MAXARITY));
      }

	SECURE(
	int argc; int n;
	argc = DEF->functor->arity;
	for(n=0; n<argc; n++)
	  checkData(argFrameP(FR, n));
	);

	NEXT_INSTRUCTION;
      }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Leave the clause:

  - update reference of current clause
    If there are no alternatives left and BFR  <=  frame  we  will
    never  return  at  this clause and can decrease the reference count.
    If BFR > frame the backtrack frame is a child of  this  frame, 
    so  this frame can become active again and we might need to continue
    this clause.

  - update BFR
    `BFR' will become the backtrack frame of other childs  of  the
    parent  frame  in which we are going to continue.  If this frame has
    alternatives and is newer than the old backFrame `BFR'  should
    become this frame.

    If there are no alternatives and  the  BFR  is  this  one  the
    BFR can become this frame's backtrackframe.

  - Update `lTop'.
    lTop can be set to this frame if there are no alternatives  in  this
    frame  and  BFR  is  older  than this frame (e.g. there are no
    frames with alternatives that are newer).

  - restore machine registers from parent frame
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
      {				MARK(I_EXIT);
    exit_builtin:
#ifdef O_ATTVAR
      if ( *valTermRef(LD->attvar.head) ) /* can be faster */
      { static code cexit;

	cexit = encode(I_EXIT);
	PC = &cexit;
	goto wakeup;
      }
#endif
      goto exit_builtin_cont;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
i_exitfact is generated to close a fact. The reason for not generating a
plain I_EXIT is first of all that the actual sequence should be I_ENTER,
I_EXIT,  and  just  optimising   to    I_EXIT   looses   the  unify-port
interception. Second, there should be some room for optimisation here.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

    VMI(I_EXITFACT) MARK(EXITFACT);
#if O_DEBUGGER
	if ( debugstatus.debugging )
	{ switch(tracePort(FR, BFR, UNIFY_PORT, PC PASS_LD))
	  { case ACTION_RETRY:
	      goto retry;
	  }
	}
#endif /*O_DEBUGGER*/
#ifdef O_ATTVAR
	if ( *valTermRef(LD->attvar.head) ) /* can be faster */
	{ static code exit;

	  exit = encode(I_EXIT);
	  PC = &exit;
	  ARGP = argFrameP(lTop, 0);	    /* needed? */
	  goto wakeup;
	}
#endif

        /* FALLTHROUGH*/
    VMI(I_EXIT) MARK(EXIT);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
First, call the tracer. Basically,  the   current  frame is garbage, but
given that the tracer might need to print the variables, we have to be a
bit more careful.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
#if O_DEBUGGER
	if ( debugstatus.debugging )
        { int action = tracePort(FR, BFR, EXIT_PORT, PC PASS_LD);

	  switch( action )
	  { case ACTION_RETRY:
	      goto retry;
	    case ACTION_FAIL:
	      discardChoicesAfter(FR PASS_LD);
	      FRAME_FAILED;
	  }

	  if ( BFR && BFR->type == CHP_DEBUG && BFR->frame == FR )
	    BFR = BFR->parent;
	}
#endif /*O_DEBUGGER*/

    exit_builtin_cont:			/* tracer already by callForeign() */
	if ( (void *)BFR <= (void *)FR ) /* deterministic */
	{ if ( false(DEF, FOREIGN) )
	  { FR->clause = NULL;		/* leaveDefinition() destroys clause */
	    leaveDefinition(DEF);
	  }
	  lTop = FR;
	  DEBUG(3, Sdprintf("Deterministic exit of %s, lTop = #%ld\n",
			    predicateName(FR->predicate), loffset(lTop)));
	} else
	{ clear(FR, FR_INBOX);
	}

	if ( !FR->parent )		/* query exit */
	{ QF = QueryFromQid(qid);	/* may be shifted: recompute */
	  QF->solutions++;

	  assert(FR == &QF->frame);

	  if ( BFR == &QF->choice )	/* No alternatives */
	  { set(QF, PL_Q_DETERMINISTIC);
	    lTop = (LocalFrame)argFrameP(FR, DEF->functor->arity);

	    if ( true(FR, FR_WATCHED) )
	      frameFinished(FR, FINISH_EXIT PASS_LD);
	  }

#ifdef O_PROFILE
          if ( LD->profile.active )
          { LocalFrame parent = parentFrame(FR);

	    if ( parent )
	      profExit(parent->prof_node PASS_LD);
	    else
	      profExit(NULL PASS_LD);
	  }
#endif
 	  QF->foreign_frame = PL_open_foreign_frame();

	  succeed;
	}

      {
#if O_DEBUGGER
	LocalFrame leave;

	leave = (true(FR, FR_WATCHED) && FR == lTop) ? FR : NULL;
#endif

        SECURE(assert(onStackArea(local, FR->parent)));

	PC = FR->programPointer;
	environment_frame = FR = FR->parent;
	DEF = FR->predicate;
	ARGP = argFrameP(lTop, 0);
	Profile(profExit(FR->prof_node PASS_LD));

#if O_DEBUGGER
	if ( leave )
	  frameFinished(leave, FINISH_EXIT PASS_LD);
#endif
      }
	NEXT_INSTRUCTION;
      }	  
  }

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
			TRACER RETRY ACTION

By default, retries the  current  frame.  If   another  frame  is  to be
retried, place the frame-reference, which  should   be  a  parent of the
current frame, in debugstatus.retryFrame and jump to this label. This is
implemented by returning retry(Frame) of the prolog_trace_interception/3
hook.

First, the system will leave any parent  frames. Next, it will undo back
to the call-port and finally, restart the clause.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if O_DEBUGGER
retry:					MARK(RETRY);
{ LocalFrame rframe0, rframe = debugstatus.retryFrame;
  mark m;
  Choice ch;

  if ( !rframe )
    rframe = FR;
  debugstatus.retryFrame = NULL;
  rframe0 = rframe;

  m.trailtop = tTop;
  m.globaltop = gTop;
  for( ; rframe; rframe = rframe->parent )
  { if ( (ch = findStartChoice(rframe, BFR)) )
    { m = ch->mark;
      goto do_retry;
    }
  }
  Sdprintf("[Could not find retry-point]\n");
  pl_abort(ABORT_NORMAL);		/* dubious */

do_retry:
  if ( rframe0 != rframe )
    Sdprintf("[No retry-information for requested frame]\n");

  Sdprintf("[Retrying frame %d running %s]\n",
	   (Word)rframe - (Word)lBase,
	   predicateName(rframe->predicate));

  discardChoicesAfter(rframe PASS_LD);
  environment_frame = FR = rframe;
  DEF = FR->predicate;
  Undo(m);
  exception_term = 0;
#ifdef O_LOGICAL_UPDATE
  if ( false(DEF, DYNAMIC) )
    FR->generation = GD->generation;
#endif

  goto retry_continue;
}
#endif /*O_DEBUGGER*/

		 /*******************************
		 *	   BACKTRACKING		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The rest of this giant procedure handles   backtracking. This used to be
very complicated, but as of pl-3.3.6, choice-points are explicit objects
and life is a lot easier. In the old days we distinquished between three
cases to get here. We leave that   it for documentation purposes as well
as to investigate optimisation in the future.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

				MARK(BKTRK);
clause_failed:				/* shallow backtracking */
{ Choice ch = BFR;

  if ( FR == ch->frame && ch->type == CHP_CLAUSE )
  { ClauseRef next;
    Undo(ch->mark);
    aTop = aFloor;

    ARGP = argFrameP(FR, 0);
    if ( !(CL = findClause(ch->value.clause, ARGP, FR, DEF, &next PASS_LD)) )
      FRAME_FAILED;			/* should not happen */
    PC = CL->clause->codes;

    if ( ch == (Choice)argFrameP(FR, CL->clause->variables) )
    { if ( next )
      { ch->value.clause = next;
	lTop = addPointer(ch, sizeof(*ch));
	NEXT_INSTRUCTION;
      } else if ( debugstatus.debugging )
      { ch->type = CHP_DEBUG;
	lTop = addPointer(ch, sizeof(*ch));
	NEXT_INSTRUCTION;
      }

      BFR = ch->parent;
      lTop = (LocalFrame)ch;
      NEXT_INSTRUCTION;
    } else
    { BFR = ch->parent;
      lTop = (LocalFrame)argFrameP(FR, CL->clause->variables);
      
      if ( next )
      { ch = newChoice(CHP_CLAUSE, FR PASS_LD);
	ch->value.clause = next;
      } else if ( debugstatus.debugging )
      { ch = newChoice(CHP_DEBUG, FR PASS_LD);
      }

      requireStack(local, (size_t)argFrameP((LocalFrame)NULL, MAXARITY));
      NEXT_INSTRUCTION;
    }
  }  
}

body_failed:
frame_failed:

{
#ifdef O_DEBUGGER
  Choice ch0 = BFR;
#endif
  Choice ch;
  LocalFrame fr0;

  DEBUG(3, Sdprintf("BACKTRACKING\n"));

  if ( is_signalled(PASS_LD1) )
  { SAVE_REGISTERS(qid);
    PL_handle_signals();
    LOAD_REGISTERS(qid);
    if ( exception_term )
      goto b_throw;
  }

next_choice:
  ch = BFR;
  fr0 = FR;
					/* leave older frames */
#ifdef O_DEBUGGER
  if ( debugstatus.debugging )
  { for(; (void *)FR > (void *)ch; FR = FR->parent)
    { if ( false(FR->predicate, FOREIGN) && /* done by callForeign() */
	   false(FR, FR_NODEBUG) )
      { Choice sch = findStartChoice(FR, ch0);

	if ( sch )
	{ Undo(sch->mark);

	  switch( tracePort(FR, BFR, FAIL_PORT, NULL PASS_LD) )
	  { case ACTION_RETRY:
	      environment_frame = FR;
	      DEF = FR->predicate;
#ifdef O_LOGICAL_UPDATE
	      if ( false(DEF, DYNAMIC) )
		FR->generation = GD->generation;
#endif
	      clear(FR, FR_CATCHED);
	      goto retry_continue;
	  }
	} else
	{ DEBUG(2, Sdprintf("Cannot trace FAIL [%d] %s\n",
			    levelFrame(FR), predicateName(FR->predicate)));
	}
      }

      /*Profile(FR->predicate->profile_fails++);*/
      leaveFrame(FR PASS_LD);
      if ( exception_term )
	goto b_throw;
    }
  } else
#endif /*O_DEBUGGER*/
  { for(; (void *)FR > (void *)ch; FR = FR->parent)
    { /*Profile(FR->predicate->profile_fails++);*/
      leaveFrame(FR PASS_LD);
      if ( exception_term )
	goto b_throw;
    }
  }

  environment_frame = FR = ch->frame;
  Undo(ch->mark);
  aTop = aFloor;			/* reset to start, for interrupts */
  DEF  = FR->predicate;

  switch(ch->type)
  { case CHP_JUMP:
      DEBUG(3, Sdprintf("    REDO #%ld: Jump in %s\n",
			loffset(FR),
			predicateName(DEF)));
      PC   = ch->value.PC;
      BFR  = ch->parent;
      Profile(profRedo(ch->prof_node PASS_LD));
      lTop = (LocalFrame)ch;
      ARGP = argFrameP(lTop, 0);
      NEXT_INSTRUCTION;
    case CHP_CLAUSE:			/* try next clause */
    { ClauseRef next;
      Clause clause;

      DEBUG(3, Sdprintf("    REDO #%ld: Clause in %s\n",
			loffset(FR),
			predicateName(DEF)));
      ARGP = argFrameP(FR, 0);
      BFR = ch->parent;
      if ( !(CL = findClause(ch->value.clause, ARGP, FR, DEF, &next PASS_LD)) )
	goto next_choice;		/* should not happen */

#ifdef O_DEBUGGER
      if ( debugstatus.debugging && !debugstatus.suspendTrace  )
      { LocalFrame fr;

	if ( !SYSTEM_MODE )		/* find user-level goal to retry */
	{ for(fr = FR; fr && true(fr, FR_NODEBUG); fr = fr->parent)
	    ;
	} else
	  fr = FR;

	if ( fr &&
	     (false(fr->predicate, HIDE_CHILDS) ||
	      false(fr, FR_INBOX)) )
	{ switch( tracePort(fr, BFR, REDO_PORT, NULL PASS_LD) )
	  { case ACTION_FAIL:
	      FRAME_FAILED;
	    case ACTION_IGNORE:
	      goto exit_builtin;
	    case ACTION_RETRY:
#ifdef O_LOGICAL_UPDATE
	      if ( false(DEF, DYNAMIC) )
		FR->generation = GD->generation;
#endif
	      goto retry_continue;
	  }
	  set(fr, FR_INBOX);
	}
      }
#endif

      clause = CL->clause;
      PC     = clause->codes;
      Profile(profRedo(ch->prof_node PASS_LD));
      lTop   = (LocalFrame)argFrameP(FR, clause->variables);

      if ( next )
      { ch = newChoice(CHP_CLAUSE, FR PASS_LD);
	ch->value.clause = next;
      } else if ( debugstatus.debugging )
      { if ( false(FR, FR_NODEBUG) && true(FR->predicate, HIDE_CHILDS) )
	  ch = newChoice(CHP_DEBUG, FR PASS_LD);
      }

			/* require space for the args of the next frame */
      requireStack(local, (size_t)argFrameP((LocalFrame)NULL, MAXARITY));
      NEXT_INSTRUCTION;
    }
    case CHP_FOREIGN:
    { int rval;

      DEBUG(3, Sdprintf("    REDO #%ld: Foreign %s, ctx = 0x%x\n",
			loffset(FR),
			predicateName(DEF),
		        ch->value.foreign));
      BFR  = ch->parent;
      Profile(profRedo(ch->prof_node PASS_LD));
      lTop = (LocalFrame)ch;

      SAVE_REGISTERS(qid);
      rval = callForeign(FR, FRG_REDO PASS_LD);
      LOAD_REGISTERS(qid);

      if ( rval )
	goto exit_builtin;
      if ( exception_term )
	goto b_throw;

      FRAME_FAILED;
    }
    case CHP_TOP:			/* Query toplevel */
    { Profile(profRedo(ch->prof_node PASS_LD));
      QF = QueryFromQid(qid);
      set(QF, PL_Q_DETERMINISTIC);
      QF->foreign_frame = PL_open_foreign_frame();
      fail;
    }
    case CHP_CATCH:			/* catch/3 */
      Undo(ch->mark);
      callCleanupHandler(ch->frame, FINISH_FAIL PASS_LD);
    case CHP_DEBUG:			/* Just for debugging purposes */
    case CHP_NONE:			/* used for C_SOFTCUT */
      BFR  = ch->parent;
#if 0
      for(; (void *)FR > (void *)ch; FR = FR->parent)
      { /*Profile(FR->predicate->profile_fails++);*/
	leaveFrame(FR PASS_LD);
	if ( exception_term )
	  goto b_throw;
      }
#endif
      goto next_choice;
  }
}
  assert(0);
  return FALSE;
} /* end of PL_next_solution() */


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(wam)
EndPredDefs

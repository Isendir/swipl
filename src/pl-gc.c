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

#ifdef SECURE_GC
#define O_DEBUG 1
#define O_SECURE 1
#endif
#include "pl-incl.h"

#define O_CLEAR_UNUSED 1

#ifndef HAVE_MEMMOVE			/* Note order!!!! */
#define memmove(dest, src, n) bcopy(src, dest, n)
#endif
#undef ulong
#define ulong unsigned long

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module is based on

    Karen Appleby, Mats Carlsson, Seif Haridi and Dan Sahlin
    ``Garbage Collection for Prolog Based on WAM''
    Communications of the ACM, June 1988, vol. 31, No. 6, pages 719-741.

Garbage collection is invoked if the WAM  interpreter  is  at  the  call
port.   This  implies  the current environment has its arguments filled.
For the moment we assume the other  reachable  environments  are  filled
completely.   There  is  room  for some optimisations here.  But we will
exploit these later.

The sole fact that the garbage collector can  only  be  invoked  if  the
machinery  is  in a well known phase of the execution is irritating, but
sofar I see no solutions around this, nor have had any indications  from
other  Prolog implementors or the literature that this was feasible.  As
a consequence however, we should start the garbage collector well before
the system runs out of memory.

In theory, we could have the compiler calculating the maximum amount  of
global   stack   data  created  before  the  next  `save  point'.   This
unfortunately is not possible for the trail stack, which  also  benifits
from  a  garbage  collection pass.  Furthermore, there is the problem of
foreign code creating global stack data (=../2, name/2, read/1, etc.).


		  CONSEQUENCES FOR THE VIRTUAL MACHINE

The virtual machine interpreter now should   ensure the stack frames are
in a predictable state. For the moment,   this  implies that all frames,
except for the current one (which only  has its arguments filled) should
be initialised fully. I'm not yet sure   whether we can't do better, but
this is simple and safe and  allows   us  to debug the garbage collector
first before starting on the optimisations.


		CONSEQUENCES FOR THE DATA REPRESENTATION

The garbage collector needs two bits on each cell of `Prolog  data'.   I
decided  to  use the low order two bits for this.  The advantage of this
that pointers to word aligned data are not affected (at least on 32 bits
machines.  Unfortunately, you will have to use 4 bytes alignment  on  16
bits  machines  now  as  well).   This demand only costs us two bits for
integers, which are now shifted two bits to the left when stored on  the
stack.   The  normal  Prolog machinery expects the lower two bits of any
Prolog data object to be zero.  The  garbage  collection  part  must  be
carefull to strip of these two bits before operating on the data.

Finally, for the compacting phase we should be able to scan  the  global
stack  both  upwards  and downwards while identifying the objects in it.
This implies reals are  now  packed  into  two  words  and  strings  are
surrounded by a word at the start and end, indicating their length.

			      DEBUGGING

Debugging a garbage collector is a difficult job.  Bugs --like  bugs  in
memory  allocation--  usually  cause  crashes  long  after  the  garbage
collection has finished.   To  simplify  debugging  a  large  number  of
actions  are  counted  during garbage collection.  At regular points the
consistency between these counts  is  verified.   This  causes  a  small
performance degradation, but for the moment is worth this I think.

If the O_SECURE cpp flag is set  some  additional  expensive  consistency
checks  that need considerable amounts of memory and cpu time are added.
Garbage collection gets about 3-4 times as slow.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Marking, testing marks and extracting values from GC masked words.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define GC_MASK		(MARK_MASK|FIRST_MASK)
#define VALUE_MASK	(~GC_MASK)

#if O_SECURE
char tmp[256];				/* for calling print_val(), etc. */
#define check_relocation(p) do_check_relocation(p, __FILE__, __LINE__ PASS_LD)
#define recordMark(p)   { if ( (p) < gLimit ) \
			  { assert(onStack(global, p)); \
			    *mark_top++ = (p); \
			  } \
			}
#else
#define recordMark(p)
#define needsRelocation(p) { needs_relocation++; }
#define check_relocation(p)
#endif

#define ldomark(p)	{ *(p) |= MARK_MASK; }
#define domark(p)	{ if ( is_marked(p) ) \
			    sysError("marked twice: %p (*= 0x%lx), gTop = %p", p, *(p), gTop); \
			  *(p) |= MARK_MASK; \
			  total_marked++; \
			  recordMark(p); \
			  DEBUG(4, Sdprintf("is_marked(%p)\n", p)); \
			}
#define unmark(p)	(*(p) &= ~MARK_MASK)

#define mark_first(p)	(*(p) |= FIRST_MASK)
#define unmark_first(p)	(*(p) &= ~FIRST_MASK)
#define is_ref(w)	isRef(w)

#define get_value(p)	(*(p) & VALUE_MASK)
#define set_value(p, w)	{ *(p) &= GC_MASK; *(p) |= w; }
#define val_ptr2(w, s)	((Word)((ulong)valPtr2((w), (s)) & ~0x3L))
#define val_ptr(w)	val_ptr2((w), storage(w))

#define inShiftedArea(area, shift, ptr) \
	((char *)ptr >= (char *)LD->stacks.area.base + shift && \
	 (char *)ptr <  (char *)LD->stacks.area.max + shift )
#define topPointerOnStack(name, addr) \
	((char *)(addr) >= (char *)LD->stacks.name.base && \
	 (char *)(addr) <  (char *)LD->stacks.name.max)

#define onGlobal(p)	onStackArea(global, p) /* onStack()? */
#define onLocal(p)	onStackArea(local, p)
#define onTrail(p)	topPointerOnStack(trail, p)

#ifndef offset
#define offset(s, f) ((int)(&((struct s *)NULL)->f))
#endif

#define ttag(x)		(((word)(x))&TAG_TRAILMASK)

		 /*******************************
		 *     FUNCTION PROTOTYPES	*
		 *******************************/

forwards void		mark_variable(Word ARG_LD);
forwards void		sweep_foreign(void);
static void		sweep_global_mark(Word *m ARG_LD);
forwards QueryFrame	mark_environments(LocalFrame, Code PC);
forwards void		update_relocation_chain(Word, Word ARG_LD);
forwards void		into_relocation_chain(Word, int stg ARG_LD);
forwards void		alien_into_relocation_chain(void *addr,
						    int orgst, int stg
						    ARG_LD);
forwards void		compact_trail(void);
forwards void		sweep_mark(mark * ARG_LD);
forwards void		sweep_trail(void);
forwards bool		is_downward_ref(Word ARG_LD);
forwards bool		is_upward_ref(Word ARG_LD);
forwards void		compact_global(void);

#if O_SECURE
forwards int		cmp_address(const void *, const void *);
forwards void		do_check_relocation(Word, char *file, int line ARG_LD);
forwards void		needsRelocation(void *);
/*forwards bool		scan_global(int marked);*/
forwards void		check_mark(mark *m);
static int		check_marked(const char *s);
#endif

		/********************************
		*           GLOBALS             *
		*********************************/

#define	total_marked	   (LD->gc._total_marked)
#define	trailcells_deleted (LD->gc._trailcells_deleted)
#define	relocation_chains  (LD->gc._relocation_chains)
#define	relocation_cells   (LD->gc._relocation_cells)
#define	relocated_cells	   (LD->gc._relocated_cells)
#define	needs_relocation   (LD->gc._needs_relocation)
#define	local_marked	   (LD->gc._local_marked)
#define	marks_swept	   (LD->gc._marks_swept)
#define	marks_unswept	   (LD->gc._marks_unswept)
#define	alien_relocations  (LD->gc._alien_relocations)
#define local_frames	   (LD->gc._local_frames)
#define choice_count	   (LD->gc._choice_count)
#if O_SECURE
#define trailtops_marked   (LD->gc._trailtops_marked)
#define mark_base	   (LD->gc._mark_base)
#define mark_top	   (LD->gc._mark_top)
#define check_table	   (LD->gc._check_table)
#endif

#undef LD
#define LD LOCAL_LD

#if O_SECURE
		/********************************
		*           DEBUGGING           *
		*********************************/

static void
needsRelocation(void *addr)
{ GET_LD

  needs_relocation++;

  addHTable(check_table, addr, (Void) TRUE);
}


static char *
print_adr(Word adr, char *buf)
{ GET_LD
  char *name;
  Word base;

  if ( onGlobal(adr) )
  { name = "global";
    base = gBase;
  } else if ( onLocal(adr) )
  { name = "local";
    base = (Word) lBase;
  } else if ( onTrail(adr) )
  { name = "trail";
    base = (Word) tBase;
  } else
  { Ssprintf(buf, "%p", adr);
    return buf;
  }

  Ssprintf(buf, "%p=%s(%d)", adr, name, adr-base);
  return buf;
}


static char *
print_val(word val, char *buf)
{ char *tag_name[] = { "var", "float", "int", "atom",
		       "string", "list", "term", "ref" };
  char *stg_name[] = { "static/inline/trail", "global", "local", "reserved" };

  Ssprintf(buf, "%s at %s(%ld)",
	   tag_name[tag(val)],
	   stg_name[storage(val) >> 3],
	   val >> LMASK_BITS);
  if ( val & MARK_MASK )
    strcat(buf, "M");
  if ( val & FIRST_MASK )
    strcat(buf, "F");

  return buf;
}

static void
do_check_relocation(Word addr, char *file, int line ARG_LD)
{ Symbol s;

  if ( !(s=lookupHTable(check_table, addr)) )
  { char buf1[256];
    char buf2[256];
    sysError("%s:%d: Address %s (%s) was not supposed to be relocated",
	     file, line, print_adr(addr, buf1), print_val(*addr, buf2));
    return;
  }

  if ( !s->value )
  { sysError("%s:%d: Relocated twice: 0x%lx", file, line, addr);
    return;
  }

  s->value = FALSE;
}

#endif /* O_SECURE */

		/********************************
		*          UTILITIES            *
		*********************************/

static QueryFrame
queryOfFrame(LocalFrame fr)
{ QueryFrame qf;

  assert(!fr->parent);

  qf = (QueryFrame)addPointer(fr, -offset(queryFrame, top_frame));
  assert(qf->magic = QID_MAGIC);

  return qf;
}


static inline int
isGlobalRef(word w)
{ return storage(w) == STG_GLOBAL;
}


static inline int
offset_cell(Word p)
{ word m = *p;				/* was get_value(p) */
  int offset;

  if ( storage(m) == STG_LOCAL )
    offset = wsizeofInd(m) + 1;
  else
    offset = 0;

  return offset;
}


static inline Word
previous_gcell(Word p)
{ p--;
  return p - offset_cell(p);
}


static inline word
makePtr(Word ptr, int tag ARG_LD)
{ int stg;

  if ( onStackArea(global, ptr) )
    stg = STG_GLOBAL;
  else if ( onStackArea(local, ptr) )
    stg = STG_LOCAL;
  else
  { assert(onStackArea(trail, ptr));
    stg = STG_TRAIL;
  }

  return consPtr(ptr, tag|stg);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Clear the FR_MARKED flags left after traversing all reachable frames.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static QueryFrame
unmark_environments(PL_local_data_t *ld, LocalFrame fr)
{ if ( fr == NULL )
    return NULL;

  for(;;)
  { if ( false(fr, FR_MARKED) )
      return NULL;
    clear(fr, FR_MARKED);
    ld->gc._local_frames--;
    
    if ( fr->parent )
      fr = fr->parent;
    else				/* Prolog --> C --> Prolog calls */
      return queryOfFrame(fr);
  }
}


static void
unmark_choicepoints(PL_local_data_t *ld, Choice ch)
{ for( ; ch; ch = ch->parent )
  { ld->gc._choice_count--;
    unmark_environments(ld, ch->frame);
  }
}


static void
unmark_stacks(PL_local_data_t *ld, LocalFrame fr, Choice ch)
{ QueryFrame qf;

  for( ; fr; fr = qf->saved_environment, ch = qf->saved_bfr )
  { qf = unmark_environments(ld, fr);
    assert(qf->magic == QID_MAGIC);
    unmark_choicepoints(ld, ch);
  }
}


		/********************************
		*            MARKING            *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void mark_variable(start)
     Word start;

After the marking phase has been completed, the following statements are
supposed to hold:

    - All non-garbage cells on the local- and global stack are
      marked.
    - `total_marked' equals the size of the global stack AFTER
      compacting (e.i. the amount of non-garbage) in words.
    - `needs_relocation' holds the total number of references from the
      argument- and local variable fields of the local stack and the
      internal global stack references that need be relocated. This
      number is only used for consistency checking with the relocation
      statistic obtained during the compacting phase.

The marking algorithm forms a two-state machine. While going deeper into
the reference tree, the pointers are reversed  and the FIRST_MASK is set
to indicate the choice points created by   complex terms with arity > 1.
Also the actual mark bit is set on the   cells. If a leaf is reached the
process reverses, restoring the  old  pointers.   If  a  `first' mark is
reached we are either finished, or have reached a choice point, in which
case  the  alternative  is  the  cell   above  (structures  are  handled
last-argument-first).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define FORWARD		goto forward
#define BACKWARD	goto backward

static void
mark_variable(Word start ARG_LD)
{ Word current;				/* current cell examined */
  word val;				/* old value of current cell */
  Word next;				/* cell to be examined */

  DEBUG(3, Sdprintf("marking %p\n", start));

  if ( is_marked(start) )
    sysError("Attempt to mark twice");

  local_marked++;
  current = start;
  mark_first(current);
  val = get_value(current);  
  total_marked--;			/* do not count local stack cell */
  FORWARD;

forward:				/* Go into the tree */
  if ( is_marked(current) )		/* have been here */
    BACKWARD;
  domark(current);

  switch(tag(val))
  { case TAG_REFERENCE:
    { next = unRef(val);		/* address pointing to */
      SECURE(assert(onStack(global, next)));
      needsRelocation(current);
      if ( is_first(next) )		/* ref to choice point. we will */
        BACKWARD;			/* get there some day anyway */
      val  = get_value(next);		/* invariant */
      set_value(next, makeRef(current));/* create backwards pointer */
      DEBUG(5, Sdprintf("Marking REF from %p to %p\n", current, next));
      current = next;			/* invariant */
      FORWARD;
    }
#ifdef O_ATTVAR
    case TAG_ATTVAR:
    { SECURE(assert(storage(val) == STG_GLOBAL));
      next = valPtr2(val, STG_GLOBAL);
      SECURE(assert(onStack(global, next)));
      needsRelocation(current);
      if ( is_marked(next) )
	BACKWARD;			/* term has already been marked */
      val  = get_value(next);		/* invariant */
					/* backwards pointer */
      set_value(next, makePtr(current, TAG_ATTVAR PASS_LD));
      DEBUG(5, Sdprintf("Marking ATTVAR from %p to %p\n", current, next));
      current = next;			/* invariant */
      FORWARD;
    }
#endif
    case TAG_COMPOUND:
    { int args;

      SECURE(assert(storage(val) == STG_GLOBAL));
      next = valPtr2(val, STG_GLOBAL);
      SECURE(assert(onStack(global, next)));
      needsRelocation(current);
      if ( is_marked(next) )
	BACKWARD;			/* term has already been marked */
      args = arityFunctor(((Functor)next)->definition) - 1;
      DEBUG(5, Sdprintf("Marking TERM %s/%d at %p\n",
			stringAtom(nameFunctor(((Functor)next)->definition)),
			args+1, next));
      domark(next);
      for( next += 2; args > 0; args--, next++ )
      { SECURE(assert(!is_first(next)));
	mark_first(next);
      }
      next--;				/* last cell of term */
      val = get_value(next);		/* invariant */
					/* backwards pointer (NO ref!) */
      set_value(next, makePtr(current, TAG_COMPOUND PASS_LD));
      current = next;
      FORWARD;
    }
    case TAG_INTEGER:
      if ( storage(val) == STG_INLINE )
	BACKWARD;
    case TAG_STRING:
    case TAG_FLOAT:			/* indirects */
    { next = valPtr2(val, STG_GLOBAL);

      SECURE(assert(storage(val) == STG_GLOBAL));
      SECURE(assert(onStack(global, next)));
      needsRelocation(current);
      if ( is_marked(next) )		/* can be referenced from multiple */
        BACKWARD;			/* places */
      domark(next);
      DEBUG(5, Sdprintf("Marked indirect data type, size = %ld\n",
			offset_cell(next) + 1));
      total_marked += offset_cell(next);
    }
  }
  BACKWARD;

backward:  				/* reversing backwards */
  while( !is_first(current) )
  { word w = get_value(current);
    int t = tag(w);

    assert(onStack(global, current));

    next = valPtr(w);
    set_value(current, val);
    switch(t)
    { case TAG_REFERENCE:
	val = makeRef(current);
        break;
      case TAG_COMPOUND:
	val = makePtr(current-1, t PASS_LD);
        break;
      case TAG_ATTVAR:
	val = makePtr(current, t PASS_LD);
        break;
      default:
	assert(0);
    }
    current= next;
  }

  unmark_first(current);
  if ( current == start )
    return;
 
  SECURE(assert(onStack(global, current)));
  { word tmp;

    tmp = get_value(current);
    set_value(current, val);		/* restore old value */
    current--;
    val = get_value(current);		/* invariant */
    set_value(current, tmp);
    FORWARD;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
References from foreign code.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
mark_term_refs()
{ GET_LD
  FliFrame fr = fli_context;

  for( ; fr; fr = fr->parent )
  { Word sp = refFliP(fr, 0);
    int n = fr->size;

    assert(fr->magic == FLI_MAGIC);
    for( ; n-- > 0; sp++ )
    { SECURE(assert(!is_marked(sp)));

      if ( isGlobalRef(*sp) )
      { mark_variable(sp PASS_LD);
      } else
      { ldomark(sp);      
      }
    }

    SECURE(check_marked("After marking foreign frame"));
  }
}


#ifdef O_GVAR

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Dealing  with  nb_setval/2  and   nb_getval/2  non-backtrackable  global
variables as defined  in  pl-gvar.c.  We   cannot  mark  and  sweep  the
hash-table itself as the  reversed   pointers  cannot  address arbitrary
addresses returned by allocHeap(). Therefore we   turn all references to
the global stack  into  term-references  and   reply  on  the  available
mark-and-sweep for foreign references.

If none of the global  variable  refers   to  the  global stack we could
`unfreeze' the global stack, except  we   may  have used nb_setarg/3. We
could enhance on this by introducing  a   `melt-bar'  set  to the lowest
location which we assigned using nb_setarg/3.   If backtracking takes us
before  that  point  we  safely  know  there  are  no  terms  left  with
nb_setarg/3  assignments.  As  the  merged   backtrackable  global  vars
implementation also causes freezing of the  stacks it it uncertain there
is much to gain with this approach.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static fid_t
gvars_to_term_refs(Word **saved_bar_at)
{ GET_LD
  fid_t fid;
  Word *sb;

  *saved_bar_at = NULL;

  if ( LD->gvar.nb_vars && LD->gvar.grefs > 0 )
  { TableEnum e = newTableEnum(LD->gvar.nb_vars);
    int found = 0;
    Symbol s;

    fid = PL_open_foreign_frame();
    while( (s=advanceTableEnum(e)) )
    { Word p = (Word)&s->value;

      if ( isGlobalRef(*p) )
      { term_t t = PL_new_term_ref();

	*valTermRef(t) = *p;
	found++;
      }
    }

    freeTableEnum(e);
    assert(LD->gvar.grefs == found);

    DEBUG(0, Sdprintf("Found %d global vars on global stack. "
		      "stored in frame %p\n", found, fli_context));
  } else
    fid = 0;

  if ( LD->frozen_bar )
  { sb = (Word*)lTop;
    lTop = (LocalFrame)(sb+1);
    *sb = LD->frozen_bar;
    *saved_bar_at = sb;
  }

  return fid;
}


static void
term_refs_to_gvars(fid_t fid, Word *saved_bar_at)
{ GET_LD

  if ( saved_bar_at )
  { assert((void *)(saved_bar_at+1) == (void*)lTop);
    LD->frozen_bar = valPtr2((word)*saved_bar_at, STG_GLOBAL);

    assert(onStack(global, LD->frozen_bar) || LD->frozen_bar == gTop);
    lTop = (LocalFrame) saved_bar_at;
  }

  if ( fid )
  { FliFrame fr = (FliFrame) valTermRef(fid);
    Word fp = (Word)(fr+1);
    TableEnum e = newTableEnum(LD->gvar.nb_vars);
    int found = 0;
    Symbol s;

    while( (s=advanceTableEnum(e)) )
    { Word p = (Word)&s->value;

      if ( isGlobalRef(*p) )
      { *p = *fp++;
	found++;
      }
    }
    assert(found == fr->size);

    freeTableEnum(e);
    PL_close_foreign_frame(fid);
  }
}

#else /*O_GVAR*/

#define gvars_to_term_refs() 0
#define term_refs_to_gvars(f) (void)0

#endif /*O_GVAR*/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
clearUninitialisedVarsFrame(LocalFrame fr, Code PC, bitmask *vm);

Assuming the clause associated will resume   execution  at PC, determine
the variables that are not yet initialised and set them to be variables.
This  avoids  the  garbage  collector    considering  the  uninitialised
variables.

[Q] wouldn't it be better to track  the variables that *are* initialised
and consider the others to be not?  Might   take more time, but might be
more reliable and simpler.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
clearUninitialisedVarsFrame(LocalFrame fr, Code PC)
{ if ( PC != NULL )
  { code c;

    for( ; ; PC = stepPC(PC))
    { c = decode(*PC);

    again:
      switch( c )
      {
#if O_DEBUGGER
	case D_BREAK:
	  c = decode(replacedBreak(PC));
	  goto again;
#endif
					/* terminate code list */
	case I_EXIT:
	case I_EXITFACT:
	case I_EXITCATCH:
	case I_EXITQUERY:
	case I_FEXITDET:
	case I_FEXITNDET:
	case I_FREDO:
	case S_TRUSTME:
	case S_LIST:
	  return;
					/* jump */
	case C_JMP:
	  PC += (int)PC[1]+2;
	  c = decode(*PC);
	  goto again;
					/* Firstvar assignments */
	case B_FIRSTVAR:
	case B_ARGFIRSTVAR:
	case A_FIRSTVAR_IS:
	case H_FIRSTVAR:
	case B_UNIFY_FIRSTVAR:
	case C_VAR:
#if O_SECURE	  
	  if ( varFrameP(fr, PC[1]) <
	       argFrameP(fr, fr->predicate->functor->arity) )
	    sysError("Reset instruction on argument");
	  assert(varFrame(fr, PC[1]) != QID_MAGIC);
#endif
	  setVar(varFrame(fr, PC[1]));
	  break;
	case H_LIST_FF:
	case B_UNIFY_FF:
	  setVar(varFrame(fr, PC[1]));
	  setVar(varFrame(fr, PC[2]));
	  break;
	case B_UNIFY_FV:
	case B_UNIFY_FC:
	  setVar(varFrame(fr, PC[1]));
	  break;
      }
    }
  }
}


#ifdef O_CLEAR_UNUSED
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
usedVarsFrame() creates a bitmap of variables that are still required if
PC is at the given location in a particular frame. 

NOTE: Unlike in clearUninitialisedVarsFrame(), we cannot  jump on a jump
instruction as we  need  to  examine   all  branches.  As  all  branches
initialise the same variabless  clearUninitialisedVarsFrame() can affort
to examine only one of the branches.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define BITSPERLONG (sizeof(long)*8)
#define VM_FAST 1

typedef struct
{ int size;				/* highest bit */
  int isize;				/* size in integers */
  long *entry;				/* entries in table */
  long fast[VM_FAST];			/* initial fast table */
} bitmask;


static void
init_bitmask(bitmask *vm)
{ vm->size  = 0;
  vm->isize = VM_FAST;
  vm->entry = vm->fast;
  vm->fast[0] = 0L;			/* fix if VM_FAST != 1 */
}


static void
free_bitmask(bitmask *vm)
{ if ( vm->entry != vm->fast )
    PL_free(vm->entry);
}


static void
set_bit(bitmask *vm, int i)
{ int is = i / BITSPERLONG;

  if ( i > vm->size )
  { while ( is >= vm->isize )
    { DEBUG(1, Sdprintf("Var %d: resizing\n", i));

      if ( vm->entry == vm->fast )
      { int bytes = sizeof(vm->fast);

	vm->entry = PL_malloc(bytes*2);
	vm->isize = VM_FAST*2;
	memcpy(vm->entry, vm->fast, bytes);
	memset((char*)vm->entry+bytes, 0, bytes);
      } else
      { int bytes = vm->isize*sizeof(long);

	vm->isize *= 2;
	vm->entry = PL_realloc(vm->entry, vm->isize*sizeof(long));
	memset((char*)vm->entry+bytes, 0, bytes);
      }    
    }
    vm->size = i;
  }

  vm->entry[is] |= 1<<(i%BITSPERLONG);
}


static int
test_bit(bitmask *vm, int i)
{ int is = i / BITSPERLONG;

  return vm->entry[is] & (1<<(i%BITSPERLONG));
}


static void
usedVarsFrame(LocalFrame fr, Code PC, bitmask *vm)
{ if ( PC != NULL )
  { code c;

    for( ; ; PC = stepPC(PC))
    { c = decode(*PC);

    again:
      switch( c )
      {
#if O_DEBUGGER
	case D_BREAK:
	  c = decode(replacedBreak(PC));
	  goto again;
#endif
					/* terminate code list */
	case I_EXIT:
	case I_EXITFACT:
	case I_EXITCATCH:
	case I_EXITQUERY:
	case I_FEXITDET:
	case I_FEXITNDET:
	case S_TRUSTME:
	case S_LIST:
	  return;
					/* supervisor redo */
	case I_FREDO:
	case S_NEXTCLAUSE:
	  goto next_clause;
					/* Mark referenced vars */
	case A_VAR0:
	case B_VAR0:
	  set_bit(vm, VAROFFSET(0));
	  break;
	case A_VAR1:
	case B_VAR1:
	  set_bit(vm, VAROFFSET(1));
	  break;
	case A_VAR2:
	case B_VAR2:
	  set_bit(vm, VAROFFSET(2));
	  break;
	case H_VAR:
	case B_VAR:
	case A_VAR:
	case B_ARGVAR:
        case B_UNIFY_VAR:
	case B_UNIFY_VC:
	case B_EQ_VC:
	  set_bit(vm, PC[1]);
	  break;
	case B_UNIFY_VV:
	case B_EQ_VV:
	  set_bit(vm, PC[1]);
	  set_bit(vm, PC[2]);
	  break;
	case B_UNIFY_FV:
	  set_bit(vm, PC[2]);
	  break;
      }
    }
  } else
  { int i, arity;

  next_clause:
    arity = fr->predicate->functor->arity;
    for(i=0; i<arity; i++)
      set_bit(vm, VAROFFSET(i));
  }
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
mark_local_variable() marks a chain of references   on  the local stack.
This prevents parents from reclaiming  variables referenced only through
a reference chain.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static inline void
mark_local_variable(Word sp ARG_LD)
{ while(isRef(*sp))
  { Word k = unRef(*sp);
    
    ldomark(sp);
    if ( is_marked(k) || isGlobalRef(*k) )
      return;
    assert(onStack(local, k));
    sp = k;
  }
  ldomark(sp);
}

#endif /*O_CLEAR_UNUSED*/


static inline int
slotsInFrame(LocalFrame fr, Code PC)
{ Definition def = fr->predicate;

  if ( !PC || true(def, FOREIGN) || !fr->clause )
    return def->functor->arity;

  return fr->clause->clause->prolog_vars;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Mark environments and all data that can   be  reached from them. Returns
the QueryFrame that started this environment,  which provides use access
to the parent `foreign' environment.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define VARNUM(i) ((i) - (ARGOFFSET / (int) sizeof(word)))

static QueryFrame
mark_environments(LocalFrame fr, Code PC)
{ GET_LD
  if ( !fr )
    return NULL;

  for( ; ; )
  { 
#ifdef O_CLEAR_UNUSED
    int i;
    bitmask vars;

    if ( false(fr, FR_MARKED) )
      clearUninitialisedVarsFrame(fr, PC);
    
    DEBUG(1, Sdprintf("Marking [%ld] %s%s\n",
		      levelFrame(fr), predicateName(fr->predicate),
		      true(fr, FR_MARKED) ? " (from choice)" : ""));

    init_bitmask(&vars);
    usedVarsFrame(fr, PC, &vars);

    for(i=VAROFFSET(0); i<=vars.size; i++ )
    { if ( test_bit(&vars, i) )
      { Word sp = varFrameP(fr, i);

	DEBUG(1, Sdprintf("\tvar %d\n", VARNUM(i)));
	if ( !is_marked(sp) )
	{ if ( isGlobalRef(*sp) )
	    mark_variable(sp PASS_LD);
	  else 
	    mark_local_variable(sp PASS_LD);
	}
      }
    }
    free_bitmask(&vars);
    set(fr, FR_MARKED);

#else /*O_CLEAR_UNUSED*/
    int slots;
    Word sp;

    if ( true(fr, FR_MARKED) )
      return NULL;                      /* from choicepoints only */
    set(fr, FR_MARKED);

    DEBUG(3, Sdprintf("Marking [%ld] %s\n",
                      levelFrame(fr), predicateName(fr->predicate)));

    clearUninitialisedVarsFrame(fr, PC);

    slots  = slotsInFrame(fr, PC);
    sp = argFrameP(fr, 0);
    for( ; slots-- > 0; sp++ )
    { if ( !is_marked(sp) )
      { if ( isGlobalRef(*sp) )
          mark_variable(sp PASS_LD);
        else
          ldomark(sp);
      }
    }
#endif /*O_CLEAR_UNUSED*/

    PC = fr->programPointer;
    if ( fr->parent != NULL )
      fr = fr->parent;
    else
      return queryOfFrame(fr);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
If multiple TrailAssignment() calls happen on  the same address within a
choicepoint we only need to keep the  first. Therefore we scan the trail
for this choicepoint from the mark to the  top and mark (using the FIRST
mark) the (global stack) addresses trailed. If we find one marked we can
delete the trail entry. To  avoid  a   second  scan  we store the marked
addresses on the argument stack.

Note that this additional scan of a section   of the trail stack is only
required if there are  at  least   two  trailed  assignments  within the
trail-ranged described by the choicepoint.

As far as I can  see  the  only   first-marks  in  use  at this time are
references to the trail-stack and we use   the first marks on the global
stack.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if O_DESTRUCTIVE_ASSIGNMENT
static void
push_marked(Word p ARG_LD)
{ requireStack(argument, sizeof(Word));
  *aTop++ = p;
}

static void
popall_marked(Word *base ARG_LD)
{ Word *p = aTop;

  while(p>base)
  { p--;
    unmark_first(*p);
  }

  aTop = base;
}

static void
mergeTrailedAssignments(GCTrailEntry top, GCTrailEntry mark,
			int assignments ARG_LD)
{ GCTrailEntry te;
  Word *m = aTop;

  DEBUG(2, Sdprintf("Scanning %d trailed assignments\n", assignments));

#if O_SECURE
  for(te=mark; te <= top; te++)
  { if ( ttag(te[1].address) == TAG_TRAILVAL )
    { Word p = val_ptr(te->address);
      assert(!is_first(p));
    }
  }
#endif

  for(te=mark; te <= top; te++)
  { if ( ttag(te[1].address) == TAG_TRAILVAL )
    { Word p = val_ptr(te->address);

      assignments--;
      if ( is_first(p) )
      {	DEBUG(3, Sdprintf("Delete duplicate trailed assignment at %p\n", p));
	te->address = 0;
	te[1].address = 0;
	trailcells_deleted += 2;
      } else
      { mark_first(p);
	push_marked(p PASS_LD);
      }
    }
  }

  popall_marked(m PASS_LD);
  assert(assignments == 0);
}
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Mark the choicepoints. This function walks   along the environments that
can be reached from  the  choice-points.   In  addition,  it deletes all
trail-references  that  will   be   overruled    by   the   choice-point
stack-reference anyway.

When using setarg/3 (O_DESTRUCTIVE_ASSIGNMENT),  destructive assignments
are stored on the trail-stack as  two   entries.  The first entry is the
normal trail-pointer, while the  second   is  flagged  with TAG_TRAILVAL
(0x1). When undoing, the tail is scanned backwards and if a tagged value
is encountered, this value is restored  at   the  location  of the first
trail-cell.

If the trail cell has become garbage,   we  can destroy both cells. Note
that mark_trail() already has marked the   replaced  value, so that will
not be garbage collected during this pass. We cannot reverse marking the
stacks and the trail-stack as one trailed   value might make another one
non-garbage. It isn't too bad however as the   next GC will take care of
the term.

Early reset of trailed  assignments  is   another  issue.  If  a trailed
location has not yet been  marked  it   can  only  be accessed by frames
*after* the undo to this choicepoint took   place.  Hence, we can do the
undo now and remove  the  cell   from  the  trailcell, saving trailstack
space. For a trailed assignment this means   we should restore the value
with the trailed value. Note however that  the trailed value has already
been marked. We however can remove  this   mark  as it will be re-marked
should it be accessible and otherwise it really is garbage. 
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static GCTrailEntry
early_reset_vars(mark *m, Word top, GCTrailEntry te ARG_LD)
{ GCTrailEntry tm = (GCTrailEntry)m->trailtop;
  GCTrailEntry te0 = te;
  int assignments = 0;

  for( ; te >= tm; te-- )		/* early reset of vars */
  { 
#if O_DESTRUCTIVE_ASSIGNMENT
    if ( isTrailVal(te->address) )
    { Word tard = val_ptr(te[-1].address);

      if ( tard >= top )
      { te->address = 0;
	te--;
	te->address = 0;
	trailcells_deleted += 2;
      } else if ( is_marked(tard) )
      { assignments++;
      } else
      { Word gp;
	DEBUG(3, Sdprintf("Early reset of assignment at %p (*=0x%lx)\n",
		 tard, *tard));
	gp = val_ptr(te->address);
	assert(onGlobal(gp));
	*tard = *gp;
	unmark(tard);

	te->address = 0;
	te--;
	te->address = 0;
	trailcells_deleted += 2;
      }
    } else
#endif
    { Word tard = val_ptr(te->address);

      if ( tard >= top )		/* above local stack */
      { SECURE(assert(ttag(te[1].address) != TAG_TRAILVAL));
	te->address = 0;
	trailcells_deleted++;
      } else if ( !is_marked(tard) )	/* garbage */
      { setVar(*tard);
	DEBUG(3, Sdprintf("Early reset at %p\n", tard));
	te->address = 0;
	trailcells_deleted++;
      }
    }
  }
  
#if O_DESTRUCTIVE_ASSIGNMENT
  if ( assignments >= 2 )
    mergeTrailedAssignments(te0, tm, assignments PASS_LD);
#endif

  return te;
}


static GCTrailEntry
mark_foreign_frame(FliFrame fr, GCTrailEntry te)
{ GET_LD

  SECURE(assert(fr->magic == FLI_MAGIC));

  te = early_reset_vars(&fr->mark, (Word)fr, te PASS_LD);

  DEBUG(3, Sdprintf("Marking foreign frame %p\n", fr));
  needsRelocation(&fr->mark.trailtop);
  alien_into_relocation_chain(&fr->mark.trailtop,
			      STG_TRAIL, STG_LOCAL PASS_LD);

  return te;
}


static GCTrailEntry
mark_choicepoints(Choice ch, GCTrailEntry te, FliFrame *flictx)
{ GET_LD

  for( ; ch; ch = ch->parent )
  { LocalFrame fr = ch->frame;
    Word top;

    while((char*)*flictx > (char*)ch)
    { FliFrame fr = *flictx;

      te = mark_foreign_frame(fr, te);
      *flictx = fr->parent;
    }

    if ( ch->type == CHP_CLAUSE )
      top = argFrameP(fr, fr->predicate->functor->arity);
    else
    { assert(ch->type == CHP_TOP || (void *)ch > (void *)fr);
      top = (Word)ch;
    }

    te = early_reset_vars(&ch->mark, top, te PASS_LD);

    needsRelocation(&ch->mark.trailtop);
    alien_into_relocation_chain(&ch->mark.trailtop,
				STG_TRAIL, STG_LOCAL PASS_LD);
    SECURE(trailtops_marked--);

    mark_environments(fr, ch->type == CHP_JUMP ? ch->value.PC : NULL);
  }

  return te;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
mark_stacks() marks all  data  that  is   reachable  from  any  frame or
choicepoint. In addition, it  will  do   `early  reset'  on variables of
choicepoints that will be  reset  anyway   if  backtracking  reaches the
choicepoint. Also, it  will  insert  all   trailtops  of  marks  in  the
relocation chains. A small problem is  the   top-goal  of  a query, This
frame may not be a  choicepoint,  but   its  mark  is  needed anyhow for
PL_close_query(), so it has to be relocated.  `te' in the function below
has to be updated as none of these variables should be reset
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
mark_stacks(LocalFrame fr, Choice ch)
{ GET_LD
  QueryFrame query;
  GCTrailEntry te = (GCTrailEntry)tTop - 1;
  FliFrame flictx = fli_context;

  trailcells_deleted = 0;

  for( ; fr; fr = query->saved_environment, ch = query->saved_bfr )
  { query = mark_environments(fr, NULL);
    te    = mark_choicepoints(ch, te, &flictx);

    assert(query->magic == QID_MAGIC);
  }
  
  for( ; flictx; flictx = flictx->parent)
    te = mark_foreign_frame(flictx, te);

  DEBUG(2, Sdprintf("Trail stack garbage: %ld cells\n", trailcells_deleted));
}


#ifdef O_DESTRUCTIVE_ASSIGNMENT
static void
mark_trail()
{ GET_LD
  GCTrailEntry te = (GCTrailEntry)tTop - 1;

  for( ; te >= (GCTrailEntry)tBase; te-- )
  { Word gp;

    if ( ttag(te->address) == TAG_TRAILVAL )
    { gp = val_ptr(te->address);

      DEBUG(3, Sdprintf("mark_trail(): trailed value from %p at %p (*=%p)\n",
			&te->address, gp, *gp));

      assert(onGlobal(gp));
      if ( !is_marked(gp) )
      {	total_marked++;			/* fix counters */
	local_marked--;

	mark_variable(gp PASS_LD);
	assert(is_marked(gp));
      }
    }
  }
}
#endif /*O_DESTRUCTIVE_ASSIGNMENT*/


#if O_SECURE
static int
cmp_address(const void *vp1, const void *vp2)
{ Word p1 = *((Word *)vp1);
  Word p2 = *((Word *)vp2);

  return p1 > p2 ? 1 : p1 == p2 ? 0 : -1;
}
#endif


static void
mark_phase(LocalFrame fr, Choice ch)
{ GET_LD
  total_marked = 0;

  SECURE(check_marked("Before mark_term_refs()"));
  mark_term_refs();
#ifdef O_DESTRUCTIVE_ASSIGNMENT
  mark_trail();
#endif
  mark_stacks(fr, ch);
#if O_SECURE
  if ( !scan_global(TRUE) )
    sysError("Global stack corrupted after GC mark-phase");
  qsort(mark_base, mark_top - mark_base, sizeof(Word), cmp_address);
#endif

  DEBUG(2, { long size = gTop - gBase;
	     Sdprintf("%ld referenced cell; %ld garbage (gTop = %p)\n",
		      total_marked, size - total_marked, gTop);
	   });
}


		/********************************
		*          COMPACTING           *
		*********************************/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Relocation chain management

A relocation chain is a linked chain of cells, whose elements all should
point to `dest' after it is unwound.  SWI-Prolog knows about a number of
different pointers.  This routine is supposed  to  restore  the  correct
pointer.  The following types are identified:

    source	types
    local	address values (gTop references)
    		term, reference and indirect pointers
    trail	address values (reset addresses)
    global	term, reference and indirect pointers

To do this, a pointer of the same  type  is  stored  in  the  relocation
chain.

    update_relocation_chain(current, dest)
	This function checks whether current is the head of a relocation
	chain.  As we know `dest' is the place  `current'  is  going  to
	move  to,  we  can reverse the chain and have all pointers in it
	pointing to `dest'.

	We must clear the `first' bit on the field.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
update_relocation_chain(Word current, Word dest ARG_LD)
{ Word head = current;
  word val = get_value(current);

  DEBUG(3, Sdprintf("unwinding relocation chain at %p to %p\n",
		    current, dest));

  do
  { int tag;

    unmark_first(current);
    current = valPtr(val);
    tag = tag(val);
    val = get_value(current);
    DEBUG(3,
	  { FliFrame f;

	    f = addPointer(current, - offset(fliFrame, mark.trailtop));
	    if ( onStack(local, f) && f->magic == FLI_MAGIC )
	      Sdprintf("Updating trail-mark of foreign frame at %p\n", f);
	  })
    set_value(current, makePtr(dest, tag PASS_LD));
    relocated_cells++;
  } while( is_first(current) );

  set_value(head, val);
  relocation_chains--;
}


static void
into_relocation_chain(Word current, int stg ARG_LD)
{ Word head;
  word val = get_value(current);
  
  head = valPtr(val);			/* FIRST/MASK already gone */
  set_value(current, get_value(head));
  set_value(head, consPtr(current, stg|tag(val)));

  DEBUG(2, Sdprintf("Into relocation chain: %p (head = %p)\n",
		    current, head));

  if ( is_first(head) )
    mark_first(current);
  else
  { mark_first(head);
    relocation_chains++;
  }

  relocation_cells++;
}


static void
alien_into_relocation_chain(void *addr, int orgst, int stg ARG_LD)
{ void **ptr = (void **)addr;
  
  *ptr = (void *)consPtr(*ptr, orgst);
  into_relocation_chain(addr, stg PASS_LD);

  alien_relocations++;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Trail stack compacting.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
compact_trail(void)
{ GET_LD
  GCTrailEntry dest, current;
  
	/* compact the trail stack */
  for( dest = current = (GCTrailEntry)tBase; current < (GCTrailEntry)tTop; )
  { if ( is_first(&current->address) )
      update_relocation_chain(&current->address, &dest->address PASS_LD);
#if O_SECURE
    else
    { Symbol s;
      if ( (s=lookupHTable(check_table, current)) != NULL &&
	   s->value == (void *)TRUE )
        sysError("%p was supposed to be relocated (*= %p)",
		 current, current->address);
    }
#endif

    if ( current->address )
      *dest++ = *current++;
    else
      current++;
  }
  if ( is_first(&current->address) )
    update_relocation_chain(&current->address, &dest->address PASS_LD);

  tTop = (TrailEntry)dest;

  if ( relocated_cells != relocation_cells )
    sysError("After trail: relocation cells = %ld; relocated_cells = %ld\n",
	     relocation_cells, relocated_cells);
} 


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
{tag,untag}_trail() are used to turn  the   native  pointers used on the
trail-stack into tagged ones as  used  on   the  other  stacks,  to make
pointer reversal in the relocation chains uniform.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
tag_trail()
{ GET_LD
  TrailEntry te;

  for( te = tTop; --te >= tBase; )
  { Word p = te->address;
    int stg;

    if ( isTrailVal(p) )
    { Word p2 = trailValP(p);

      SECURE(assert(onStack(global, p2)));
      te->address = (Word)consPtr(p2, STG_GLOBAL|TAG_TRAILVAL);
      te--;
    }

    if ( onLocal(te->address) )
    { stg = STG_LOCAL;
    } else
    { SECURE(assert(onStackArea(global, te->address)));
      stg = STG_GLOBAL;
    }

    te->address = (Word)consPtr(te->address, stg);
  }
}


static void
untag_trail()
{ GET_LD
  TrailEntry te;

  for(te = tBase; te < tTop; te++)
  { if ( te->address )
    { word mask = ttag(te->address);

      te->address = (Word)((word)valPtr((word)te->address)|mask);
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Sweep a mark. This is a bit tricky as the global-stack pointer may point
to  a  garbage  global  cell.  Therefore  we  have  to  find  the  first
non-garbage one. Unfortunately, the cell may  already be in a relocation
chain (in which case `first' is true). In  this case it is not a garbage
cell. Hence the `goto found'.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
sweep_global_mark(Word *m ARG_LD)
{ Word gm, prev;

  SECURE(assert(onStack(local, m)));
  gm = *m;

  for(;;)
  { if ( gm == gBase )
    { *m = (Word)consPtr(gm, STG_GLOBAL);
      alien_relocations++;
      break;
    }
    if ( is_first(gm-1) )
      goto found;
    prev = previous_gcell(gm);
    if ( is_marked(prev) )
    {
    found:
      *m = gm;
      DEBUG(3, Sdprintf("gTop mark from choice point: "));
      needsRelocation(m);
      alien_into_relocation_chain(m, STG_GLOBAL, STG_LOCAL PASS_LD);
      break;
    }
    gm = prev;
  }
}


static inline void
sweep_mark(mark *m ARG_LD)
{ marks_swept++;
  sweep_global_mark(&m->globaltop PASS_LD);
}


static void
sweep_foreign()
{ GET_LD
  FliFrame fr = fli_context;

  for( ; fr; fr = fr->parent )
  { Word sp = refFliP(fr, 0);
    int n = fr->size;

    sweep_mark(&fr->mark PASS_LD);
    for( ; n-- > 0; sp++ )
    { if ( is_marked(sp) )
      {	unmark(sp);
	if ( isGlobalRef(get_value(sp)) )
	{ local_marked--;
	  check_relocation(sp);
	  into_relocation_chain(sp, STG_LOCAL PASS_LD);
	}
      }
    }
  }
}


static void
unsweep_mark(mark *m ARG_LD)
{ m->trailtop  = (TrailEntry)valPtr2((word)m->trailtop,  STG_TRAIL);
  m->globaltop = valPtr2((word)m->globaltop, STG_GLOBAL);

  SECURE(check_mark(m));

  marks_unswept++;
}


static void
unsweep_foreign(ARG1_LD)
{ FliFrame fr = fli_context;

  for( ; fr; fr = fr->parent )
    unsweep_mark(&fr->mark PASS_LD);
}


static void
unsweep_choicepoints(Choice ch ARG_LD)
{ for( ; ch ; ch = ch->parent)
    unsweep_mark(&ch->mark PASS_LD);
}


static QueryFrame
unsweep_environments(LocalFrame fr)
{ while(fr->parent)
    fr = fr->parent;

  return queryOfFrame(fr);
}


static void
unsweep_stacks(LocalFrame fr, Choice ch ARG_LD)
{ QueryFrame query;

  for( ; fr; fr = query->saved_environment, ch = query->saved_bfr )
  { query = unsweep_environments(fr);
    unsweep_choicepoints(ch PASS_LD);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Sweeping the local and trail stack to insert necessary pointers  in  the
relocation chains.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
sweep_trail(void)
{ GET_LD
  GCTrailEntry te = (GCTrailEntry)tTop - 1;

  for( ; te >= (GCTrailEntry)tBase; te-- )
  { if ( te->address )
    {
#ifdef O_DESTRUCTIVE_ASSIGNMENT
      if ( ttag(te->address) == TAG_TRAILVAL )
      { needsRelocation(&te->address);
	into_relocation_chain(&te->address, STG_TRAIL PASS_LD);
      } else
#endif
      if ( storage(te->address) == STG_GLOBAL )
      { needsRelocation(&te->address);
	into_relocation_chain(&te->address, STG_TRAIL PASS_LD);
      }
    }
  }
}


static QueryFrame
sweep_environments(LocalFrame fr, Code PC)
{ GET_LD

  if ( !fr )
    return NULL;

  for( ; ; )
  { int slots;
    Word sp;

    if ( false(fr, FR_MARKED) )
      return NULL;
    clear(fr, FR_MARKED);

    slots = slotsInFrame(fr, PC);

    sp = argFrameP(fr, 0);
    for( ; slots > 0; slots--, sp++ )
    { if ( is_marked(sp) )
      { unmark(sp);
	if ( isGlobalRef(get_value(sp)) )
	{ local_marked--;
	  check_relocation(sp);
	  into_relocation_chain(sp, STG_LOCAL PASS_LD);
	}
#ifdef O_CLEAR_UNUSED
      } else if ( !isVar(*sp) )
      { DEBUG(1, Sdprintf("[%d] %s: GC var %d\n",
			  levelFrame(fr),
			  predicateName(fr->predicate),
			  sp-argFrameP(fr, 0)));
	*sp = ATOM_gc_reclaimed;
#endif /*O_CLEAR_UNUSED*/
      }
    }

    PC = fr->programPointer;
    if ( fr->parent != NULL )
      fr = fr->parent;
    else
      return queryOfFrame(fr);
  }
}


static void
sweep_choicepoints(Choice ch ARG_LD)
{ for( ; ch ; ch = ch->parent)
  { sweep_environments(ch->frame,
		       ch->type == CHP_JUMP ? ch->value.PC : NULL);
    sweep_mark(&ch->mark PASS_LD);
  }
}


static void
sweep_stacks(LocalFrame fr, Choice ch)
{ GET_LD
  QueryFrame query;
  
  for( ; fr; fr = query->saved_environment, ch = query->saved_bfr )
  { query = sweep_environments(fr, NULL);
    sweep_choicepoints(ch PASS_LD);

    if ( !query )			/* we've been here */
      break;
  }

  if ( local_marked != 0 )
    sysError("local_marked = %ld", local_marked);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
All preparations have been made now, and the actual  compacting  of  the
global  stack  may  start.   The  marking phase has calculated the total
number of words (cells) in the global stack that are non-garbage.

In the first phase, we will  walk  along  the  global  stack  from  it's
current  top towards the bottom.  During this phase, `current' refers to
the current element we are processing, while `dest' refers to the  place
this  element  will  be  after  the compacting phase is completed.  This
invariant is central and should be maintained carefully while processing
alien objects as strings and reals, which happen to have a  non-standard
size.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static bool
is_downward_ref(Word p ARG_LD)
{ word val = get_value(p);

  switch(tag(val))
  { case TAG_INTEGER:
      if ( storage(val) == STG_INLINE )
	fail;
    case TAG_ATTVAR:
    case TAG_STRING:
    case TAG_FLOAT:
    case TAG_REFERENCE:
    case TAG_COMPOUND:
    { Word d = val_ptr(val);

      SECURE(assert(d >= gBase));

      return d < p;
    }
  }

  fail;
}


static bool
is_upward_ref(Word p ARG_LD)
{ word val = get_value(p);

  switch(tag(val))
  { case TAG_INTEGER:
      if ( storage(val) == STG_INLINE )
	fail;
    case TAG_ATTVAR:
    case TAG_STRING:
    case TAG_FLOAT:
    case TAG_REFERENCE:
    case TAG_COMPOUND:
    { Word d = val_ptr(val);

      SECURE(assert(d < gTop));

      return d > p;
    }
  }

  fail;
}


#if O_SECURE

static int
check_marked(const char *s)
{ GET_LD
  long m = 0;
  Word current;
  long cells = 0;

  for( current = gBase; current < gTop; current += (offset_cell(current)+1) )
  { cells++;
    if ( is_marked(current) )
    { m += (offset_cell(current)+1);
    }
  }

  if ( m == total_marked )
    return TRUE;

  if ( m != total_marked )
    Sdprintf("**** ERROR: size: %ld != %ld (%s) ****\n",
	     m, total_marked, s);

  return FALSE;
}

#endif /*O_SECURE*/


static void
compact_global(void)
{ GET_LD
  Word dest, current;
#if O_SECURE
  Word *v = mark_top;
#endif

  DEBUG(2, Sdprintf("Scanning global stack downwards\n"));

  dest = gBase + total_marked;			/* first FREE cell */
  for( current = gTop; current >= gBase; current-- )
  { 
    if ( is_marked(current) )
    { marked_large_cell:
#if O_SECURE
      if ( current != *--v )
        sysError("Marked cell at %p (*= %p); gTop = %p; should be %p",
		 current, *current, gTop, *v);
#endif
      dest--;
      DEBUG(3, Sdprintf("Marked cell at %p (dest = %p)\n", current, dest));
      if ( is_first(current) )
	update_relocation_chain(current, dest PASS_LD);
      if ( is_downward_ref(current PASS_LD) )
      { check_relocation(current);
	into_relocation_chain(current, STG_GLOBAL PASS_LD);
      }
    } else if ( is_first(current) )
    { first_large_cell:
      update_relocation_chain(current, dest PASS_LD);	/* gTop refs from marks */
    } else if ( storage(*current) == STG_LOCAL ) /* large cell */
    { long offset = offset_cell(current);

      assert(offset > 0);
      current -= offset;		/* start large cell */
      if ( is_marked(current) )
      { dest -= offset;
	goto marked_large_cell;
      } else if ( is_first(current) )
      { goto first_large_cell;
      }					/* else: garbage large cell */
    }					/* else: garbage */
  }

#if O_SECURE
  if ( v != mark_base )
  { for( v--; v >= mark_base; v-- )
    { Sdprintf("Expected marked cell at %p, (*= 0x%lx)\n", *v, **v);
    }
    sysError("v = %p; mark_base = %p", v, mark_base);
  }
#endif

  if ( dest != gBase )
    sysError("Mismatch in down phase: dest = %p, gBase = %p\n",
	     dest, gBase);
  if ( relocation_cells != relocated_cells )
    sysError("After down phase: relocation_cells = %ld; relocated_cells = %ld",
	     relocation_cells, relocated_cells);

  SECURE(check_marked("Before up"));

  DEBUG(2, Sdprintf("Scanning global stack upwards\n"));
  dest = gBase;
  for(current = gBase; current < gTop; )
  { if ( is_marked(current) )
    { long l, n;

      if ( is_first(current) )
	update_relocation_chain(current, dest PASS_LD);

      if ( (l = offset_cell(current)) == 0 )	/* normal cells */
      { *dest = *current;
        if ( is_upward_ref(current PASS_LD) )
	{ check_relocation(current);
          into_relocation_chain(dest, STG_GLOBAL PASS_LD);
	}
	unmark(dest);
	dest++;
	current++;
      } else					/* indirect values */
      { Word cdest, ccurrent;

	l++;
	
	for( cdest=dest, ccurrent=current, n=0; n < l; n++ )
	  *cdest++ = *ccurrent++;
      
	unmark(dest);
	dest += l;
	current += l;
      }

    } else
      current += offset_cell(current) + 1;
  }

  if ( dest != gBase + total_marked )
    sysError("Mismatch in up phase: dest = %p, gBase+total_marked = %p\n",
	     dest, gBase + total_marked );

  DEBUG(3, { Word p = dest;		/* clear top of stack */
	     while(p < gTop)
	       *p++ = 0xbfbfbfbfL;
	   });

  gTop = dest;
}

static void
collect_phase(LocalFrame fr, Choice ch, Word *saved_bar_at)
{ GET_LD
 
  SECURE(check_marked("Start collect"));

  DEBUG(2, Sdprintf("Sweeping foreign references\n"));
  sweep_foreign();
  DEBUG(2, Sdprintf("Sweeping trail stack\n"));
  sweep_trail();
  DEBUG(2, Sdprintf("Sweeping local stack\n"));
  sweep_stacks(fr, ch);
  if ( saved_bar_at )
  { DEBUG(2, Sdprintf("Sweeping frozen bar\n"));
    sweep_global_mark(saved_bar_at PASS_LD);
  }
  DEBUG(2, Sdprintf("Compacting global stack\n"));
  compact_global();

  unsweep_foreign(PASS_LD1);
  unsweep_stacks(fr, ch PASS_LD);

  if ( relocation_chains != 0 )
    sysError("relocation chains = %ld", relocation_chains);
  if ( relocated_cells != relocation_cells ||
       relocated_cells != needs_relocation )
    sysError("relocation cells = %ld; relocated_cells = %ld, "
	     "needs_relocation = %ld\n\t",
	     relocation_cells, relocated_cells, needs_relocation);
}


		/********************************
		*             MAIN              *
		*********************************/

static void
gcRequest(Stack s, int req)
{ GET_LD
  if ( gc_status.requested != req )
  { gc_status.requested = req;
    updateAlerted(LD);
    DEBUG(1, if ( req )
	     { Sdprintf("%s overflow: Posted garbage collect request\n",
			s->name);
	     });
  }
}

#if O_DYNAMIC_STACKS
void
considerGarbageCollect(Stack s)
{ GET_LD
  if ( s->gc && trueFeature(GC_FEATURE) && !gc_status.requested )
  { long used  = (char *)s->top   - (char *)s->base;
    long free  = (char *)s->limit - (char *)s->top;
    long limit = (char *)s->limit - (char *)s->base;
    int req = FALSE;

    if ( used > s->factor*s->gced_size + s->small )
    { DEBUG(2, Sdprintf("GC: request on %s, factor=%d, last=%ld, small=%ld\n",
			s->name, s->factor, s->gced_size, s->small));
      req = TRUE;
    } else if ( free < limit/8 && used > s->gced_size + limit/32 ) 
    { DEBUG(2, Sdprintf("GC: request on low free\n"));
      req = TRUE;
    }

    if ( req )
      gcRequest(s, req);
  }
}
#endif /* O_DYNAMIC_STACKS */


#if O_SECURE || O_DEBUG || defined(O_MAINTENANCE)
bool
scan_global(int marked)
{ GET_LD
  Word current, next;
  int errors = 0;
  long cells = 0;

  for( current = gBase; current < gTop; current += (offset_cell(current)+1) )
  { int offset;

    cells++;

    if ( (!marked && is_marked(current)) || is_first(current) )
    { warning("!Illegal cell in global stack (up) at %p (*= %p)",
	      current, *current);
      if ( isAtom(*current) )
	warning("!%p is atom %s", current, stringAtom(*current));
      if ( isTerm(*current) )
	warning("!%p is term %s/%d",
		current,
		stringAtom(nameFunctor(functorTerm(*current))),
		arityTerm(*current));
      if ( ++errors > 10 )
      { Sdprintf("...\n");
        break;
      }
    }

    offset = offset_cell(current);
    next = current+offset+1;
    if ( offset > 0 )
    { if ( offset_cell(next-1) != offset )
      { errors++;
	Sdprintf("ERROR: Illegal indirect cell on global stack at %p-%p\n"
		 "       tag=%d, offset=%d\n",
		 current, next, tag(*current), offset);
	trap_gdb();
      }
    }
  }

  for( current = gTop - 1; current >= gBase; current-- )
  { cells--;
    current -= offset_cell(current);
    if ( (!marked && is_marked(current)) || is_first(current) )
    { warning("!Illegal cell in global stack (down) at %p (*= %p)",
	      current, *current);
      if ( ++errors > 10 )
      { Sdprintf("...\n");
        break;
      }
    }
  }

  if ( !errors && cells != 0 )
    sysError("Different count of cells upwards and downwards: %ld\n", cells);

  return errors == 0;
}


static void
check_mark(mark *m)
{ GET_LD

  assert(onStackArea(trail,  m->trailtop));
  assert(onStackArea(global, m->globaltop));
}


static QueryFrame
check_environments(LocalFrame fr, Code PC, Word key)
{ GET_LD

  if ( fr == NULL )
    return NULL;

  for(;;)
  { int slots, n;
    Word sp;

    if ( true(fr, FR_MARKED) )
      return NULL;			/* from choicepoints only */
    set(fr, FR_MARKED);
    local_frames++;
    clearUninitialisedVarsFrame(fr, PC);

    assert(onStack(local, fr));

    DEBUG(3, Sdprintf("Check [%ld] %s:",
		      levelFrame(fr),
		      predicateName(fr->predicate)));

    slots = slotsInFrame(fr, PC);
    sp = argFrameP(fr, 0);
    for( n=0; n < slots; n++ )
    { *key += checkData(&sp[n]);
    }
    DEBUG(3, Sdprintf(" 0x%lx\n", key));

    PC = fr->programPointer;
    if ( fr->parent )
      fr = fr->parent;
    else
    { QueryFrame qf = queryOfFrame(fr);
      DEBUG(3, Sdprintf("*** Query %s\n", predicateName(qf->frame.predicate)));
      return qf;
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Unfortunately the key returned by check_choicepoints() is not constant
due to `early reset' optimisation.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static word
check_choicepoints(Choice ch)
{ GET_LD
  word key = 0L;

  for( ; ch; ch = ch->parent )
  { if ( !ch->parent )
      assert(ch->type == CHP_TOP);
    choice_count++;
    check_mark(&ch->mark);
    check_environments(ch->frame,
		       ch->type == CHP_JUMP ? ch->value.PC : NULL,
		       &key);
  }

  return key;
}


word
check_foreign()
{ GET_LD
  FliFrame ff;
  word key = 0L;

  for(ff = fli_context; ff; ff = ff->parent )
  { Word sp = refFliP(ff, 0);
    int n = ff->size;

    assert(ff->magic == FLI_MAGIC);
    assert(ff->parent < ff);

    for(n=0 ; n < ff->size; n++ )
      key += checkData(&sp[n]);

    check_mark(&ff->mark);
  }

  return key;
}


#ifdef O_DESTRUCTIVE_ASSIGNMENT
static word
check_trail()
{ GET_LD
  TrailEntry te = tTop - 1;
  word key = 0;

  for( ; te >= tBase; te-- )
  { Word gp;

    if ( isTrailVal(te->address) )
    { gp = trailValP(te->address);

      assert(onGlobal(gp));
      key += checkData(gp);
#ifdef O_SECURE
    } else
    { if ( onStackArea(global, te->address) )
	assert(onStack(global, te->address));
#endif
    }
  }

  return key;
}
#endif /*O_DESTRUCTIVE_ASSIGNMENT*/


word
checkStacks(LocalFrame frame, Choice choice)
{ GET_LD
  LocalFrame fr;
  Choice ch;
  QueryFrame qf;
  word key = 0L;

  if ( !frame )
    frame = environment_frame;
  if ( !choice )
    choice = LD->choicepoints;

  local_frames = 0;
  choice_count = 0;

  for( fr = frame, ch=choice;
       fr;
       fr = qf->saved_environment, ch = qf->saved_bfr )
  { qf = check_environments(fr, NULL, &key);
    assert(qf->magic == QID_MAGIC);

    DEBUG(3, Sdprintf("%ld\n", key));
    /*key += */check_choicepoints(ch);		/* See above */
  }

  SECURE(trailtops_marked = choice_count);

  unmark_stacks(LD, frame, choice);

  assert(local_frames == 0);
  assert(choice_count == 0);

  key += check_foreign();
  DEBUG(3, Sdprintf("Foreign: %ld\n", key));
#ifdef O_DESTRUCTIVE_ASSIGNMENT
  /*key +=*/ check_trail();
#endif

  DEBUG(2, Sdprintf("Final: %ld\n", key));
  return key;
}

#endif /*O_SECURE || O_DEBUG*/

#ifdef O_PLMT

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
About synchronisation with atom-gc (AGC). GC can run fully concurrent in
different threads as it only  affects   the  runtime stacks. AGC however
must sweep the other threads. It can only do so if these are in a fairly
sane state, which isn't the case during GC.  So:

We keep the number of threads doing GC in GD->gc.active, a variable that
is incremented and decremented using  the   L_GC  mutex. This same mutex
guards AGC as a whole. This  means  that   if  AGC  is working, GC can't
start. If AGC notices at the start  a   GC  is working, it sets the flag
GD->gc.agc_waiting and returns. If the last   GC  stops, and notices the
system wants to do AGC it raised a request for AGC.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
enterGC()
{ PL_LOCK(L_GC);
  GD->gc.active++;
  PL_UNLOCK(L_GC);
}

static void
leaveGC()
{ PL_LOCK(L_GC);
  if ( --GD->gc.active == 0 && GD->gc.agc_waiting )
  { GD->gc.agc_waiting = FALSE;
    PL_raise(SIG_ATOM_GC);
  }
  PL_UNLOCK(L_GC);
}

#else

#define enterGC() (void)0
#define leaveGC() (void)0

#endif /*O_PLMT*/


void
garbageCollect(LocalFrame fr, Choice ch)
{ GET_LD
  long tgar, ggar;
  double t = CpuTime(CPU_USER);
  int verbose = trueFeature(TRACE_GC_FEATURE);
  sigset_t mask;
  fid_t fid;
  Word *saved_bar_at;
#ifdef O_SECURE
  word key;
#endif

  DEBUG(0, verbose = TRUE);

  if ( gc_status.blocked || !trueFeature(GC_FEATURE) )
    return;

  enterGC();
  blockSignals(&mask);
  blockGC(PASS_LD1);			/* avoid recursion due to */
  gcRequest(NULL, FALSE);		/* printMessage() */

  gc_status.active = TRUE;
  if ( verbose )
    printMessage(ATOM_informational,
		 PL_FUNCTOR_CHARS, "gc", 1,
		   PL_CHARS, "start");

#ifdef O_PROFILE
  if ( LD->profile.active )
    profCall(GD->procedures.dgarbage_collect1->definition PASS_LD);
#endif

#if O_SECURE
  if ( !scan_global(FALSE) )
    sysError("Stack not ok at gc entry");

  key = checkStacks(fr, ch);

  if ( check_table == NULL )
    check_table = newHTable(256);
  else
    clearHTable(check_table);

  mark_base = mark_top = malloc(usedStack(global));
#endif

  needs_relocation  = 0;
  relocation_chains = 0;
  relocation_cells  = 0;
  relocated_cells   = 0;
  local_marked	    = 0;

  requireStack(global, sizeof(word));
  requireStack(trail, sizeof(struct trail_entry));
  setVar(*gTop);
  tTop->address = 0;

  fid = gvars_to_term_refs(&saved_bar_at);
  tag_trail();
  mark_phase(fr, ch);
  tgar = trailcells_deleted * sizeof(struct trail_entry);
  ggar = (gTop - gBase - total_marked) * sizeof(word);
  gc_status.trail_gained  += tgar;
  gc_status.global_gained += ggar;
  gc_status.collections++;

  DEBUG(2, Sdprintf("Compacting trail ... "));
  compact_trail();

  collect_phase(fr, ch, saved_bar_at);
  untag_trail();
  term_refs_to_gvars(fid, saved_bar_at);
#if O_SECURE
  assert(trailtops_marked == 0);
  if ( !scan_global(FALSE) )
    sysError("Stack not ok after gc; gTop = %p", gTop);
  free(mark_base);
#endif

  t = CpuTime(CPU_USER) - t;
  gc_status.time += t;
  trimStacks(PASS_LD1);
  LD->stacks.global.gced_size = usedStack(global);
  LD->stacks.trail.gced_size  = usedStack(trail);
  gc_status.active = FALSE;

#ifdef O_CLEAR_UNUSED
  SECURE(checkStacks(fr, ch));
#else
  SECURE(if ( checkStacks(fr, ch) != key )
	 { sysError("ERROR: Stack checksum failure\n");
	   trap_gdb();
	 });
#endif

  if ( verbose )
    printMessage(ATOM_informational,
		 PL_FUNCTOR_CHARS, "gc", 1,
		   PL_FUNCTOR_CHARS, "done", 7,
		     PL_LONG, ggar,
		     PL_LONG, tgar,
		     PL_DOUBLE, (double)t,
		     PL_LONG, usedStack(global),
		     PL_LONG, usedStack(trail),
		     PL_LONG, roomStack(global),
		     PL_LONG, roomStack(trail));

#ifdef O_PROFILE
  if ( fr && LD->profile.active )
    profExit(fr->prof_node PASS_LD);
#endif

  unblockGC(PASS_LD1);
  unblockSignals(&mask);
  leaveGC();
}

word
pl_garbage_collect(term_t d)
{ GET_LD
  LocalFrame fr = environment_frame;
  Choice ch = LD->choicepoints;

#if O_DEBUG
  int ol = GD->debug_level;
  int nl;

  if ( d )
  { if ( !PL_get_integer_ex(d, &nl) )
      fail;
    GD->debug_level = nl;
  }
#endif
  garbageCollect(fr, ch);
#if O_DEBUG
  GD->debug_level = ol;
#endif
  succeed;
}


void
blockGC(ARG1_LD)
{ gc_status.blocked++;
#if O_SHIFT_STACKS
  shift_status.blocked++;
#endif
}


void
unblockGC(ARG1_LD)
{ gc_status.blocked--;
#if O_SHIFT_STACKS
  shift_status.blocked--;
#endif
}


#if O_SHIFT_STACKS

		 /*******************************
		 *	   STACK-SHIFTER	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Update the Prolog runtime stacks presuming they have shifted by the
the specified offset.

Memory management description.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef O_DEBUG
extern char *chp_chars(Choice ch);
#endif

static inline void
update_pointer(void *p, long offset)
{ char **ptr = (char **)p;

  if ( *ptr )
    *ptr += offset;
}


		 /*******************************
		 *	   LOCAL STACK		*
		 *******************************/

static void
update_mark(mark *m, long gs, long ts)
{ if ( ts ) update_pointer(&m->trailtop, ts);
  if ( gs ) update_pointer(&m->globaltop, gs);
}


/* Update pointer if it contains a pointer in the local stack.  Used for
   updating PC, as this might point to a locally compiled clause by
   I_USERCALL0.
*/

static inline void
update_local_pointer(void *p, long ls)
{ GET_LD
  char **ptr = (char **)p;

  if ( onStackArea(local, *ptr) )
  { DEBUG(2, Sdprintf(" (local ptr %p)", *ptr));
    update_pointer(p, ls);
  }
}


static QueryFrame
update_environments(LocalFrame fr, Code PC, long ls, long gs, long ts)
{ GET_LD
  if ( fr == NULL )
    return NULL;

  for(;;)
  { assert(inShiftedArea(local, ls, fr));

    if ( true(fr, FR_MARKED) )
      return NULL;			/* from choicepoints only */
    set(fr, FR_MARKED);
    local_frames++;
    
    DEBUG(2,
	  Sdprintf("Shifting frame %p [%ld] %s ... ",
		 fr, levelFrame(fr), predicateName(fr->predicate)));

    if ( ls )				/* update frame pointers */
    { update_pointer(&fr->parent, ls);
      clearUninitialisedVarsFrame(fr, PC);

      DEBUG(2, Sdprintf("PC=%p ", fr->programPointer));
      update_local_pointer(&fr->programPointer, ls);
					/* I_USERCALL0 compiled clause */
      if ( fr->predicate == PROCEDURE_dcall1->definition )
      { update_pointer(&fr->clause, ls);
	update_pointer(&fr->clause->clause, ls);
	update_pointer(&fr->clause->clause->codes, ls);
      }

					/* update saved BFR's from C_IFTHEN */
      if ( PC && false(fr->predicate, FOREIGN) )
      { Clause cl = fr->clause->clause;
	unsigned int marks;

	if ( (marks = cl->marks) )
	{ Word sp = argFrameP(fr, cl->prolog_vars);
	
	  DEBUG(2, Sdprintf(" (%d marks)", marks));

	  for( ; marks-- > 0; sp++ )
	    update_pointer(sp, ls);
	}
      }

      DEBUG(2, Sdprintf("ok\n"));
    }


    PC = fr->programPointer;
    if ( fr->parent )
      fr = fr->parent;
    else				/* Prolog --> C --> Prolog calls */
    { QueryFrame query = queryOfFrame(fr);

      if ( ls )
      { update_pointer(&query->saved_bfr, ls);
	update_pointer(&query->saved_environment, ls);
	update_pointer(&query->registers.fr, ls);
      }
      
      return query;
    }
  }
}


static void
update_choicepoints(Choice ch, long ls, long gs, long ts)
{ GET_LD

  for( ; ch; ch = ch->parent )
  { DEBUG(1, Sdprintf("Updating choicepoint %s for %s ... ",
		      chp_chars(ch),
		      predicateName(ch->frame->predicate)));

    if ( ls )
    { update_pointer(&ch->frame, ls);
      update_pointer(&ch->parent, ls);
      if ( ch->type == CHP_JUMP )
	update_local_pointer(&ch->value.PC, ls);
    }
    update_mark(&ch->mark, gs, ts);
    update_environments(ch->frame,
		        ch->type == CHP_JUMP ? ch->value.PC : NULL,
			ls, gs, ts);
    choice_count++;
    DEBUG(1, Sdprintf("ok\n"));
  }
}


		 /*******************************
		 *	  ARGUMENT STACK	*
		 *******************************/

static void
update_argument(long ls, long gs)
{ GET_LD
  Word *p = aBase;
  Word *t = aTop;

  for( ; p < t; p++ )
  { if ( onGlobal(*p) )
    { *p = addPointer(*p, gs);
    } else
    { assert(onLocal(*p));
      *p = addPointer(*p, ls);
    }
  }
}


		 /*******************************
		 *	  TRAIL STACK	*
		 *******************************/

static void
update_trail(long ls, long gs)
{ GET_LD
  TrailEntry p = tBase;
  TrailEntry t = tTop;

  for( ; p < t; p++ )
  { if ( onGlobal(trailValP(p->address)) )
    { update_pointer(&p->address, gs);
    } else
    { assert(onLocal(p->address));
      update_pointer(&p->address, ls);
    }
  }
}


		 /*******************************
		 *	  FOREIGN FRAMES	*
		 *******************************/

static void
update_foreign(long ts, long ls, long gs)
{ GET_LD
  FliFrame fr = addPointer(fli_context, ls);

  for( ; fr; fr = fr->parent )
  { update_mark(&fr->mark, gs, ts);
    update_pointer(&fr->parent, ls);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Entry-point.   Update the  stacks to  reflect  their current  positions.
This function should be called *after*  the  stacks have been relocated.
Note that these functions are  only used  if  there is no virtual memory
way to reach at dynamic stacks.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define updateStackHeader(name, offset) \
	{ LD->stacks.name.base  = addPointer(LD->stacks.name.base,  offset); \
	  LD->stacks.name.top   = addPointer(LD->stacks.name.top,   offset); \
	  LD->stacks.name.max   = addPointer(LD->stacks.name.max,   offset); \
	  LD->stacks.name.limit = addPointer(LD->stacks.name.limit, offset); \
	}


static void
update_stacks(LocalFrame frame, Choice choice, Code PC,
	      void *lb, void *gb, void *tb)
{ GET_LD
  long ls, gs, ts;

  ls = (long) lb - (long) lBase;
  gs = (long) gb - (long) gBase;
  ts = (long) tb - (long) tBase;

  DEBUG(2, Sdprintf("update_stacks(): ls+gs+ts = %ld %ld %ld\n", ls, gs, ts));

  if ( ls || gs || ts )
  { LocalFrame fr;
    Choice ch;
    QueryFrame qf;

    local_frames = 0;
    choice_count = 0;
    
    update_local_pointer(&PC, ls);

    for( fr = addPointer(frame, ls),
	 ch = addPointer(choice, ls)
       ; fr
       ; fr = qf->saved_environment,
	 ch = qf->saved_bfr,
	 PC = NULL
       )
    { qf = update_environments(fr, PC, ls, gs, ts);

      update_choicepoints(ch, ls, gs, ts);
    }

    DEBUG(2, Sdprintf("%d frames, %d choice-points ...",
		      local_frames, choice_count));

    frame  = addPointer(frame, ls);
    choice = addPointer(choice, ls);
    unmark_stacks(LD, frame, choice);

    assert(local_frames == 0);
    assert(choice_count == 0);

    if ( gs || ls )
    { update_argument(ls, gs);
      update_trail(ls, gs);
    }
    update_foreign(ts, ls, gs);

    updateStackHeader(local,  ls);
    updateStackHeader(global, gs);
    updateStackHeader(trail,  ts);

    base_addresses[STG_LOCAL]  = (unsigned long)lBase;
    base_addresses[STG_GLOBAL] = (unsigned long)gBase;
    base_addresses[STG_TRAIL]  = (unsigned long)tBase;
  }

  if ( ls )
  { update_pointer(&environment_frame, ls);
    update_pointer(&fli_context, ls);
    update_pointer(&LD->choicepoints, ls);
  }
  if ( gs )
  { update_pointer(&LD->mark_bar, gs);
  } 
}


static long
nextStackSize(Stack s, long minfree)
{ long size  = diffPointers(s->max, s->base);
  long limit = diffPointers(s->limit, s->base);
  long grow;

  grow = ((minfree+8192) > size/2 ? minfree+8192 : size/2);
  grow = ROUND(grow, 8192);

  if ( size + grow > limit )
  { if ( size + grow > (limit*3)/2 )
      outOfStack(s, STACK_OVERFLOW_THROW);

    outOfStack(s, STACK_OVERFLOW_SIGNAL);
    grow = limit - size;
    if ( grow < 32 * 1024 )
      grow = 32 * 1024;
    s->limit = addPointer(s->base, size+grow);
  }

  size += grow;

  return size;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Entry point from interpret()
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define GL_SEPARATION sizeof(word)

int
growStacks(LocalFrame fr, Choice ch, Code PC, long l, long g, long t)
{ GET_LD
  
  if ( !fr )
    fr = environment_frame;
  if ( !ch )
    ch = LD->choicepoints;

  if ( PC != NULL )			/* for now, only at the call-port */
    fail;

  if ( (l || g || t) && !shift_status.blocked )
  { TrailEntry tb = tBase;
    Word gb = gBase;
    LocalFrame lb = lBase;
    long lsize = sizeStack(local);
    long gsize = sizeStack(global);
    long tsize = sizeStack(trail);
    double time = CpuTime(CPU_USER);
    int verbose = trueFeature(TRACE_GC_FEATURE);
    
    DEBUG(0, verbose = TRUE);

    if ( verbose )
    { int i = 0;

      Sdprintf("Expanding ");
      if ( l ) Sdprintf("%s%s", i++ ? "and " : "", "local ");
      if ( g ) Sdprintf("%s%s", i++ ? "and " : "", "global ");
      if ( t ) Sdprintf("%s%s", i++ ? "and " : "", "trail ");
      Sdprintf("stacks ");
    }

    if ( !fr )
      fr = environment_frame;
    if ( !ch )
      ch = LD->choicepoints;

    SECURE(if ( !scan_global(FALSE) ) sysError("Stack not ok at shift entry"));
    SECURE(key = checkStacks(fr, ch));

    if ( t )
    { tsize = nextStackSize((Stack) &LD->stacks.trail, t);
      tb = PL_realloc(tb, tsize);
      shift_status.trail_shifts++;
    }

    if ( g || l )
    { long loffset = gsize + GL_SEPARATION;
      assert(lb == addPointer(gb, loffset));	

      if ( g )
      { gsize = nextStackSize((Stack) &LD->stacks.global, g);
	shift_status.global_shifts++;
      }
      if ( l )
      { lsize = nextStackSize((Stack) &LD->stacks.local, l);
	shift_status.local_shifts++;
      }

      gb = xrealloc(gb, lsize + gsize + GL_SEPARATION);
      lb = addPointer(gb, gsize + GL_SEPARATION);
      if ( g )				/* global enlarged; move local */
	memmove(lb,   addPointer(gb, loffset), lsize);
	     /* dest, src,                     size */
    }
      
    if ( verbose )
    { Sdprintf("to (l+g+t) = %d+%d+%d Kbytes ... ",
	   lsize / 1024,
	   gsize / 1024,
	   tsize / 1024);
    }

#define PrintStackParms(stack, name, newbase, newsize) \
	{ Sdprintf("%6s: 0x%08lx ... 0x%08lx --> 0x%08lx ... 0x%08lx\n", \
		 name, \
		 (unsigned long) LD->stacks.stack.base, \
		 (unsigned long) LD->stacks.stack.max, \
		 (unsigned long) newbase, \
		 (unsigned long) addPointer(newbase, newsize)); \
	}


    DEBUG(0, { Sputchar('\n');
	       PrintStackParms(global, "global", gb, gsize);
	       PrintStackParms(local, "local", lb, lsize);
	       PrintStackParms(trail, "trail", tb, tsize);
	     });
		    
    DEBUG(1, Sdprintf("Updating stacks ..."));
    update_stacks(fr, ch, PC, lb, gb, tb);

    LD->stacks.local.max  = addPointer(LD->stacks.local.base,  lsize);
    LD->stacks.global.max = addPointer(LD->stacks.global.base, gsize);
    LD->stacks.trail.max  = addPointer(LD->stacks.trail.base,  tsize);

    SetHTop(LD->stacks.local.max);
    SetHTop(LD->stacks.trail.max);

    time = CpuTime(CPU_USER) - time;
    shift_status.time += time;
    SECURE(if ( checkStacks(NULL, NULL) != key )
	   { Sdprintf("Stack checksum failure\n");
	     trap_gdb();
	   });
    if ( verbose )
    { Sdprintf("%.2f sec.\n", time);
    }

    succeed;
  }

  fail;
}

#endif /*O_SHIFT_STACKS*/

#ifdef O_ATOMGC

		 /*******************************
		 *	      ATOM-GC		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The  routine  markAtomsOnStacks(PL_local_data_t  *ld)  marks  all  atoms
reachable  from  the  global  stack,    environments,  choicepoints  and
term-references  using  markAtom().  It  is    designed   to  allow  for
asynchronous calling, even from different   threads (hence the argument,
although the thread examined should be stopped). 

Asynchronous calling is in general not  possible,   but  here we make an
exception. markAtom() is supposed  to  test   for  and  silently  ignore
non-atoms. Basically, this implies we can   mark a few atoms incorrectly
from the interrupted frame, but in   the context of multi-threading this
is a small price to pay.

Otherwise  this  routine  is  fairly  trivial.   It  is  modelled  after
checkStacks(), a simple routine for  checking stack-consistency that has
to walk along all reachable data as well.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef O_DEBUG_ATOMGC
extern IOSTREAM * atomLogFd;		/* for error messages */

static long
loffset(void *p)
{ GET_LD
  if ( p == NULL )
    return 0;

  assert((long)p % sizeof(word) == 0);
  return (Word)p-(Word)lBase;
}
#endif

static void
markAtomsOnGlobalStack(PL_local_data_t *ld)
{ Word gbase = ld->stacks.global.base;
  Word gtop  = ld->stacks.global.top;
  Word current;

  for(current = gbase; current < gtop; current += (offset_cell(current)+1) )
  { if ( isAtom(*current) )
      markAtom(*current);
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This is much like  check_environments(),  but   as  we  might  be called
asynchronously, we have to be a bit careful about the first frame (if PC
== NULL). The interpreter will  set  the   clause  field  to NULL before
opening the frame, and we only have   to  consider the arguments. If the
frame has a clause we must consider all variables of this clause.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static QueryFrame
mark_atoms_in_environments(PL_local_data_t *ld, LocalFrame fr)
{ Code PC = NULL;

  if ( fr == NULL )
    return NULL;

  for(;;)
  { int slots, n;
    Word sp;

    if ( true(fr, FR_MARKED) )
      return NULL;			/* from choicepoints only */
    set(fr, FR_MARKED);
#ifdef O_DEBUG_ATOMGC
    if ( atomLogFd )
      Sfprintf(atomLogFd,
	       "Marking atoms from [%d] %s\n",
	       levelFrame(fr),
	       predicateName(fr->predicate));
#endif
    ld->gc._local_frames++;
    clearUninitialisedVarsFrame(fr, PC);

    if ( true(fr->predicate, FOREIGN) ||
	 !fr->clause )
      slots = fr->predicate->functor->arity;
    else
      slots = fr->clause->clause->prolog_vars;

    sp = argFrameP(fr, 0);
    for( n=0; n < slots; n++, sp++ )
    { if ( isAtom(*sp) )
	markAtom(*sp);
    }

    PC = fr->programPointer;
    if ( fr->parent )
      fr = fr->parent;
    else
      return queryOfFrame(fr);
  }
}


static void
markAtomsInTermReferences(PL_local_data_t *ld)
{ FliFrame   ff = ld->foreign_environment;
  
  for(; ff; ff = ff->parent )
  { Word sp = refFliP(ff, 0);
    int n = ff->size;

    for(n=0 ; n < ff->size; n++ )
    { if ( isAtom(sp[n]) )
	markAtom(sp[n]);
    }
  }
}


static void
markAtomsInEnvironments(PL_local_data_t *ld)
{ QueryFrame qf;
  LocalFrame fr;
  Choice ch;

  ld->gc._local_frames = 0;

  for( fr = ld->environment,
       ch = ld->choicepoints
     ; fr
     ; fr = qf->saved_environment,
       ch = qf->saved_bfr
     )
  { qf = mark_atoms_in_environments(ld, fr);
    assert(qf->magic == QID_MAGIC);

    for(; ch; ch = ch->parent)
    {
#ifdef O_DEBUG_ATOMGC
      if ( atomLogFd )
	Sfprintf(atomLogFd, "Marking atoms from choicepoint #%ld on %s\n",
		 loffset(ch), predicateName(ch->frame->predicate));
#endif
      mark_atoms_in_environments(ld, ch->frame);
    }
  }

  unmark_stacks(ld, ld->environment, ld->choicepoints);

  assert(ld->gc._local_frames == 0);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The BACKTRACE code below can only  be   compiled  on systems using glibc
(the GNU C-library). It saves the  stack-trace   of  the  latest call to
markAtomsOnStacks()  to  help  identifying  problems.    You   can  call
print_backtrace() from GDB to find the last stack-trace.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define BACKTRACE 0

#if BACKTRACE
#include <execinfo.h>
#include <string.h>
static char **mark_backtrace;
size_t trace_frames;

static void
save_backtrace (void)
{ void *array[100];
     
  trace_frames = backtrace(array, sizeof(array)/sizeof(void *));
  if ( mark_backtrace )
    free(mark_backtrace);
  mark_backtrace = backtrace_symbols(array, trace_frames);
}

void
print_backtrace()
{ int i;

  for(i=0; i<trace_frames; i++)
    Sdprintf("[%d] %s\n", i, mark_backtrace[i]);
}

#endif /*BACKTRACE*/


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
markAtomsOnStacks()  is  called   asynchronously    (Unix)   or  between
SuspendThread()/ResumeThread() from another thread in  Windows. Its task
is to mark all atoms that  are   references  from  the Prolog stacks. It
should not make any assumptions  on   the  initialised  variables in the
stack-frames, but it is allowed to mark atoms from uninitialised data as
this causes some atoms not to  be   GC-ed  this  time (maybe better next
time).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

void
markAtomsOnStacks(PL_local_data_t *ld)
{ assert(!ld->gc.status.active);

#if BACKTRACE
  save_backtrace();
#endif

  markAtomsOnGlobalStack(ld);
  markAtomsInEnvironments(ld);
  markAtomsInTermReferences(ld);
}

#endif /*O_ATOMGC*/

#ifdef O_CLAUSEGC
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This is much like  check_environments(),  but   as  we  might  be called
asynchronously, we have to be a bit careful about the first frame (if PC
== NULL). The interpreter will  set  the   clause  field  to NULL before
opening the frame, and we only have   to  consider the arguments. If the
frame has a clause we must consider all variables of this clause.

This  routine  is  used   by    garbage_collect_clauses/0   as  well  as
start_consult/1. In the latter case, only  predicates in this sourcefile
are marked.

Predicates marked with P_FOREIGN_CREF are   foreign  predicates that use
the frame->clause choicepoint info for  storing the clause-reference for
the next clause. Amoung these are retract/1, clause/2, etc.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static QueryFrame
mark_predicates_in_environments(PL_local_data_t *ld, LocalFrame fr)
{ if ( fr == NULL )
    return NULL;

  for(;;)
  { Definition def;

    if ( true(fr, FR_MARKED) )
      return NULL;			/* from choicepoints only */
    set(fr, FR_MARKED);
    ld->gc._local_frames++;

    if ( true(fr->predicate, P_FOREIGN_CREF) && fr->clause )
    { ClauseRef cref = (ClauseRef)fr->clause;

      def = getProcDefinition(cref->clause->procedure);
    } else
      def = fr->predicate;

    if ( def &&
	 false(def, DYNAMIC) && 
	 def->references == 0 )		/* already done */
    { if ( GD->procedures.reloading )
      { ListCell cell;			/* startConsult() */

	for(cell=GD->procedures.reloading->procedures; cell; cell=cell->next)
	{ Procedure proc = cell->value;

	  if ( proc->definition == def )
	  { DEBUG(2, Sdprintf("Marking %s\n", predicateName(def)));
	    def->references++;
	    GD->procedures.active_marked++;
	    break;
	  }
	}
      } else				/* pl_garbage_collect_clauses() */
      { if ( true(def, NEEDSCLAUSEGC) )
	{ DEBUG(2, Sdprintf("Marking %s\n", predicateName(def)));
	  def->references++;
	}
      }
    }

    if ( fr->parent )
      fr = fr->parent;
    else
      return queryOfFrame(fr);
  }
}


void
markPredicatesInEnvironments(PL_local_data_t *ld)
{ QueryFrame qf;
  LocalFrame fr;
  Choice ch;

  ld->gc._local_frames = 0;

  for( fr = ld->environment,
       ch = ld->choicepoints
     ; fr
     ; fr = qf->saved_environment,
       ch = qf->saved_bfr
     )
  { qf = mark_predicates_in_environments(ld, fr);
    assert(qf->magic == QID_MAGIC);

    for(; ch; ch = ch->parent)
    { mark_predicates_in_environments(ld, ch->frame);
    }
  }

  unmark_stacks(ld, ld->environment, ld->choicepoints);

  assert(ld->gc._local_frames == 0);
}


#endif /*O_CLAUSEGC*/

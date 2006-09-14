/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2002, University of Amsterdam

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

/*#define O_DEBUG 1*/			/* include crash/0 */
#include "pl-incl.h"
#include "pl-ctype.h"

#if O_DEBUG
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
See how the system reacts on segmentation faults.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static word
pl_crash()
{ long *lp = NULL;

  Sdprintf("You asked for it ... Writing to address 0\n");

  *lp = 5;

  Sdprintf("Oops, this doesn't appear to be a secure OS\n");

  fail;
}
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Link all foreign language predicates.  The arguments to FRG are:

	FRG(name, arity, function, flags).

Flags almost always is TRACE_ME.  Additional common flags:

	METAPRED		Predicate is module transparent
	NONDETERMINISTIC	Predicate can be resatisfied
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define NOTRACE PL_FA_NOTRACE
#define META    PL_FA_TRANSPARENT
#define NDET	PL_FA_NONDETERMINISTIC
#define VA	PL_FA_VARARGS
#define CREF	PL_FA_CREF

#define FRG(n, a, f, flags) { n, a, f, flags }

static const PL_extension foreigns[] = {
#if O_DEBUG
  FRG("crash",			0, pl_crash,			0),
#endif
  FRG("nl",			0, pl_nl,			0),
  FRG("get_single_char",	1, pl_get_single_char,		0),
  FRG("$push_input_context",	0, pl_push_input_context,	0),
  FRG("$pop_input_context",	0, pl_pop_input_context,	0),
  FRG("seeing",			1, pl_seeing,			0),
  FRG("telling",		1, pl_telling,			0),
  FRG("seen",			0, pl_seen,			0),
  FRG("tab",			1, pl_tab,			0),
  FRG("tmp_file",		2, pl_tmp_file,			0),
  FRG("delete_file",		1, pl_delete_file,		0),
  FRG("delete_directory",	1, pl_delete_directory,		0),
  FRG("make_directory",		1, pl_make_directory,		0),
  FRG("access_file",		2, pl_access_file,		0),
  FRG("read_link",		3, pl_read_link,		0),
  FRG("exists_file",		1, pl_exists_file,		0),
  FRG("exists_directory",	1, pl_exists_directory,		0),
  FRG("rename_file",		2, pl_rename_file,		0),
  FRG("same_file",		2, pl_same_file,		0),
  FRG("time_file",		2, pl_time_file,		0),
  FRG("told",			0, pl_told,			0),
  FRG("see",			1, pl_see,			0),
  FRG("tell",			1, pl_tell,			0),
  FRG("append",			1, pl_append,			0),
  FRG("ttyflush",		0, pl_ttyflush,			0),
  FRG("flush_output",		0, pl_flush_output,		0),
  FRG("prompt",			2, pl_prompt,			0),
  FRG("prompt1",		1, pl_prompt1,			0),
  FRG("expand_file_name",	2, pl_expand_file_name,		0),
  FRG("$absolute_file_name",	2, pl_absolute_file_name,	0),
  FRG("is_absolute_file_name",	1, pl_is_absolute_file_name,	0),
  FRG("file_base_name",		2, pl_file_base_name,		0),
  FRG("file_directory_name",	2, pl_file_dir_name,		0),
  FRG("file_name_extension",	3, pl_file_name_extension,	0),
  FRG("prolog_to_os_filename",	2, pl_prolog_to_os_filename,	0),
  FRG("$mark_executable",	1, pl_mark_executable,		0),
#ifdef __WIN32__
  FRG("win_exec",		2, pl_win_exec,			0),
  FRG("win_module_file",	2, pl_win_module_file,		0),
#endif

  FRG("$on_signal",		4, pl_on_signal,	     META),

  FRG("fileerrors",		2, pl_fileerrors,		0),
  FRG("working_directory",	2, pl_working_directory,	0),

  FRG("halt",			1, pl_halt,			0),
  FRG("$shell",			2, pl_shell,			0),
  FRG("getenv",			2, pl_getenv,			0),
  FRG("setenv",			2, pl_setenv,			0),
  FRG("unsetenv",		1, pl_unsetenv,			0),
  FRG("wildcard_match",		2, pl_wildcard_match,		0),
  FRG("$apropos_match",		2, pl_apropos_match,		0),
  FRG("sub_atom",		5, pl_sub_atom,		     NDET),
  FRG("sleep",			1, pl_sleep,			0),
  FRG("break",			0, pl_break,			0),
  FRG("$break",			1, pl_break1,			0),
  FRG("notrace",		1, pl_notrace1,		     META),

  FRG("write_canonical",	1, pl_write_canonical,		0),
  FRG("write_term",		2, pl_write_term,		0),
  FRG("write_term",		3, pl_write_term3,		0),
  FRG("write",			1, pl_write,			0),
  FRG("writeq",			1, pl_writeq,			0),
  FRG("print",			1, pl_print,			0),

  FRG("read_term",		2, pl_read_term,		0),
  FRG("read_term",		3, pl_read_term3,		0),
  FRG("read",			1, pl_read,			0),
  FRG("$raw_read",		1, pl_raw_read,			0),
  FRG("$raw_read",		2, pl_raw_read2,		0),
  FRG("current_op",		3, pl_current_op,	NDET|META),
  FRG("$local_op",		3, pl_local_op,	        NDET|META),
  FRG("$builtin_op",		3, pl_builtin_op,	     NDET),
  FRG("current_functor",	2, pl_current_functor,	     NDET),
  FRG("$complete_atom",		3, pl_complete_atom,		0),
  FRG("$atom_completions",	2, pl_atom_completions,		0),
  FRG("op",			3, pl_op,		     META),
  FRG("char_conversion",	2, pl_char_conversion,		0),
  FRG("current_char_conversion",2, pl_current_char_conversion, NDET),

  FRG("!",			0, pl_metacut,			0),
  FRG("=..",			2, pl_univ,			0),
  FRG("name",			2, pl_name,			0),
  FRG("atom_chars",		2, pl_atom_chars,		0),
  FRG("atom_codes",		2, pl_atom_codes,		0),
  FRG("number_chars",		2, pl_number_chars,		0),
  FRG("number_codes",		2, pl_number_codes,		0),
  FRG("char_code",		2, pl_char_code,		0),
  FRG("downcase_atom",		2, pl_downcase_atom,		0),
  FRG("upcase_atom",		2, pl_upcase_atom,		0),
  FRG("atom_concat",		3, pl_atom_concat,	     NDET),
  FRG("$concat_atom",		2, pl_concat_atom,		0),
  FRG("concat_atom",		3, pl_concat_atom3,		0),
  FRG("atom_length",		2, pl_atom_length,		0),
  FRG("atom_to_term",		3, pl_atom_to_term,		0),
  FRG("$e_free_variables",	2, pl_e_free_variables,		0),

  FRG("$open_wic",		1, pl_open_wic,			0),
  FRG("$close_wic",		0, pl_close_wic,		0),
  FRG("$add_directive_wic",	1, pl_add_directive_wic,	0),
  FRG("$import_wic",		2, pl_import_wic,		0),

  FRG("$rc_handle",		1, pl_rc_handle,		0),
  FRG("$rc_members",		2, pl_rc_members,		0),
  FRG("$rc_open",		5, pl_rc_open,			0),
  FRG("$rc_open_archive",	2, pl_rc_open_archive,		0),
  FRG("$rc_close_archive",	1, pl_rc_close_archive,		0),
  FRG("$rc_save_archive",	2, pl_rc_save_archive,		0),
  FRG("$rc_append_file",	5, pl_rc_append_file,		0),
  FRG("copy_stream_data",	2, pl_copy_stream_data2,	0),
  FRG("copy_stream_data",	3, pl_copy_stream_data3,	0),

  FRG("$qlf_start_module",	1, pl_qlf_start_module,		0),
  FRG("$qlf_start_sub_module",	1, pl_qlf_start_sub_module,	0),
  FRG("$qlf_start_file",	1, pl_qlf_start_file,		0),
  FRG("$qlf_end_part",		0, pl_qlf_end_part,		0),
  FRG("$qlf_open",		1, pl_qlf_open,			0),
  FRG("$qlf_close",		0, pl_qlf_close,		0),
  FRG("$qlf_load",		2, pl_qlf_load,		     META),
  FRG("$qlf_assert_clause",	2, pl_qlf_assert_clause,	0),

  FRG("abolish",    		1, pl_abolish1,		     META),
  FRG("abolish",    		2, pl_abolish,		     META),
  FRG("clause",    		2, pl_clause2,	        NDET|META|CREF),
  FRG("clause",    		3, pl_clause3,	        NDET|META|CREF),
  FRG("$clause",	        4, pl_clause4,	        NDET|META|CREF),
  FRG("nth_clause", 		3, pl_nth_clause,       NDET|META|CREF),
  FRG("retract",    		1, pl_retract,          NDET|META|CREF),
  FRG("retractall",		1, pl_retractall,	     META),
#if O_DEBUGGER
  FRG("$xr_member",		2, pl_xr_member,        NDET|META),
  FRG("$clause_term_position",	3, pl_clause_term_position,	0),
  FRG("$break_pc",		3, pl_break_pc,		     NDET),
  FRG("$break_at",		3, pl_break_at,			0),
  FRG("$current_break",		2, pl_current_break,	     NDET),
#endif  
#ifdef O_MAINTENANCE
  FRG("$list_generations",	1, pl_list_generations,	     META),
  FRG("$check_procedure",	1, pl_check_procedure,	     META),
#endif

  FRG("recorda",		3, pl_recorda,			0),
  FRG("recordz",		3, pl_recordz,			0),
  FRG("recorded",		3, pl_recorded,		     NDET),
  FRG("erase",			1, pl_erase,			0),
  FRG("$term_complexity",	3, pl_term_complexity,		0),
  FRG("redefine_system_predicate", 1, pl_redefine_system_predicate,
							     META),

  FRG("$c_current_predicate",	2, pl_current_predicate,  NDET|META),
  FRG("current_predicate",	1, pl_current_predicate1, NDET|META),
  FRG("$set_predicate_attribute", 3, pl_set_predicate_attribute,META),
  FRG("$get_predicate_attribute", 3, pl_get_predicate_attribute,META),
  FRG("$get_clause_attribute",  3, pl_get_clause_attribute,	0),
  FRG("$require",		1, pl_require,		     META),
  FRG("source_file",		2, pl_source_file,      NDET|META),
  FRG("$time_source_file",	2, pl_time_source_file,	     NDET),
  FRG("$start_consult",		1, pl_start_consult,		0),
  FRG("$make_system_source_files",0,pl_make_system_source_files,0),
  FRG("$default_predicate",	2, pl_default_predicate,     META),
  FRG("$clause_from_source",	3, pl_clause_from_source,	0),

  FRG("unify_with_occurs_check",2, pl_unify_with_occurs_check,  0),
  FRG("\\=",			2, pl_notunify,			0),

  FRG("repeat",			0, pl_repeat,		     NDET),
  FRG("fail",			0, pl_fail,			0),
  FRG("true",			0, pl_true,			0),
  FRG("$fail",			0, pl_fail,		  NOTRACE),
  FRG("abort",			0, pl_abort,			0),

  FRG("trace",			0, pl_trace,		  NOTRACE),
  FRG("notrace",		0, pl_notrace,		  NOTRACE),
  FRG("tracing",		0, pl_tracing,		  NOTRACE),
  FRG("$spy",			1, pl_spy,		     META),
  FRG("$nospy",			1, pl_nospy,		     META),
  FRG("$leash",			2, pl_leash,		  NOTRACE),
  FRG("$visible",		2, pl_visible,		  NOTRACE),
  FRG("$debuglevel",		2, pl_debuglevel,		0),

#if COUNTING
  FRG("$count",			0, pl_count,			0),
#endif /* COUNTING */

  FRG("prolog_current_frame",	1, pl_prolog_current_frame,	0),
  FRG("prolog_frame_attribute",	3, pl_prolog_frame_attribute,	0),
  FRG("prolog_choice_attribute",3, pl_prolog_choice_attribute,	0),
  FRG("prolog_skip_level",	2, pl_skip_level,	  NOTRACE),

  FRG("dwim_match",		3, pl_dwim_match,		0),
  FRG("$dwim_predicate",	2, pl_dwim_predicate,	     NDET),

  FRG("protocol",		1, pl_protocol,			0),
  FRG("protocola",		1, pl_protocola,		0),
  FRG("noprotocol",		0, pl_noprotocol,		0),
  FRG("protocolling",		1, pl_protocolling,		0),

#ifdef O_PROLOG_HOOK
  FRG("set_prolog_hook",	3, pl_set_prolog_hook,	        0),
#endif
  FRG("$current_module",	2, pl_current_module,	     NDET),
  FRG("$module",		2, pl_module,			0),
  FRG("$set_source_module",	2, pl_set_source_module,	0),
  FRG("$declare_module",	3, pl_declare_module,		0),
  FRG("context_module",		1, pl_context_module,	     META),
  FRG("import",			1, pl_import,		     META),
  FRG("export",			1, pl_export,		     META),
  FRG("$check_export",		0, pl_check_export,	     META),
  FRG("export_list",		2, pl_export_list,		0),
  FRG("index",			1, pl_index,		     META),
  FRG("hash",			1, pl_hash,		     META),
  FRG("$open_shared_object",	3, pl_open_shared_object,    META),
#if defined(HAVE_DLOPEN) || defined(HAVE_SHL_LOAD) || defined(EMULATE_DLOPEN)
  FRG("close_shared_object",	1, pl_close_shared_object,   META),
  FRG("call_shared_object_function",
				2, pl_call_shared_object_function,
							     META),
#endif /*HAVE_DLOPEN*/

#if O_DDE
  FRG("open_dde_conversation",	3, pl_open_dde_conversation,	0),
  FRG("close_dde_conversation",	1, pl_close_dde_conversation,	0),
  FRG("dde_request",		4, pl_dde_request,		0),
  FRG("dde_execute",		3, pl_dde_execute,		0),
  FRG("dde_poke",		4, pl_dde_poke,			0),
  FRG("$dde_register_service",	2, pl_dde_register_service,	0),
#endif /*O_DDE*/

#if O_STRING
  FRG("string_concat",		3, pl_string_concat,	     NDET),
  FRG("string_length",		2, pl_string_length,		0),
  FRG("string_to_atom",		2, pl_string_to_atom,		0),
  FRG("string_to_list",		2, pl_string_to_list,		0),
  FRG("sub_string",		5, pl_sub_string,	     NDET),
#endif /* O_STRING */

  FRG("$length",		2, pl_length,			0),
  FRG("memberchk",		2, pl_memberchk,		0),
  FRG("format",			2, pl_format,		     META),
#ifdef O_DEBUG
  FRG("$check_definition",	1, pl_check_definition,      META),
#endif

  FRG("$atom_hashstat",		2, pl_atom_hashstat,		0),
  FRG("$current_prolog_flag",	5, pl_feature5,		     NDET),
  FRG("current_prolog_flag",	2, pl_feature,		     NDET),
  FRG("set_prolog_flag",	2, pl_set_feature,		0),
  FRG("trim_stacks",		0, pl_trim_stacks,		0),
#if O_SHIFT_STACKS
  FRG("stack_parameter",	4, pl_stack_parameter,		0),
#endif
  FRG("$garbage_collect",	1, pl_garbage_collect,		0),
#ifdef O_ATOMGC
  FRG("garbage_collect_atoms",	0, pl_garbage_collect_atoms,	0),
  FRG("garbage_collect_clauses", 0, pl_garbage_collect_clauses,	0),
#ifdef O_DEBUG_ATOMGC
  FRG("track_atom",		2, pl_track_atom,		0),
#endif
#endif
  FRG("current_key",		1, pl_current_key,	     NDET),
  FRG("current_flag",		1, pl_current_flag,	     NDET),

  FRG("open",			3, pl_open,			0),
  FRG("open",			4, pl_open4,			0),
  FRG("open_null_stream",	1, pl_open_null_stream,		0),
  FRG("close",			1, pl_close,			0),
  FRG("close",			2, pl_close2,			0),
  FRG("stream_property",	2, pl_stream_property,	     NDET),
  FRG("flush_output",		1, pl_flush_output1,		0),
  FRG("set_stream_position",	2, pl_set_stream_position,	0),
  FRG("seek",			4, pl_seek,			0),
  FRG("set_input",		1, pl_set_input,		0),
  FRG("set_output",		1, pl_set_output,		0),
  FRG("set_stream",		2, pl_set_stream,		0),
  FRG("current_input",		1, pl_current_input,		0),
  FRG("current_output",		1, pl_current_output,		0),
  FRG("source_location",	2, pl_source_location,		0),
  FRG("at_end_of_stream",	1, pl_at_end_of_stream1,	0),
  FRG("at_end_of_stream",	0, pl_at_end_of_stream0,	0),

  FRG("nl",			1, pl_nl1,			0),
  FRG("tab",			2, pl_tab2,			0),
  FRG("read",			2, pl_read2,			0),
  FRG("write",			2, pl_write2,			0),
  FRG("writeq",			2, pl_writeq2,			0),
  FRG("print",			2, pl_print2,			0),
  FRG("write_canonical",	2, pl_write_canonical2,		0),
  FRG("format",			3, pl_format3,		     META),

  FRG("tty_get_capability",	3, pl_tty_get_capability,	0),
  FRG("tty_goto",		2, pl_tty_goto,			0),
  FRG("tty_put",		2, pl_tty_put,			0),
  FRG("tty_size",		2, pl_tty_size,			0),
  FRG("format_predicate",	2, pl_format_predicate,	     META),
  FRG("current_format_predicate", 2, pl_current_format_predicate,
						        META|NDET),
  FRG("wait_for_input",		3, pl_wait_for_input,		0),
  FRG("get_time",		1, pl_get_time,			0),
  FRG("size_file",		2, pl_size_file,		0),
  FRG("$size_stream",		2, pl_size_stream,		0),
#if O_PROLOG_FUNCTIONS
  FRG("current_arithmetic_function", 1, pl_current_arithmetic_function,
							NDET|META),
#endif

#ifdef O_PLMT
  FRG("thread_create",		3, pl_thread_create,	     META),
  FRG("thread_join",		2, pl_thread_join,	        0),
  FRG("thread_exit",		1, pl_thread_exit,		0),
  FRG("current_thread",		2, pl_current_thread,        NDET),
  FRG("thread_kill",		2, pl_thread_kill,              0),
  FRG("thread_send_message",	2, pl_thread_send_message,	0),
  FRG("thread_get_message",	1, pl_thread_get_message,	0),
  FRG("thread_peek_message",	1, pl_thread_peek_message,	0),
  FRG("thread_signal",		2, pl_thread_signal,	     META),
  FRG("thread_at_exit",		1, pl_thread_at_exit,	     META),
  FRG("mutex_create",		1, pl_mutex_create,		0),
  FRG("mutex_destroy",		1, pl_mutex_destroy,		0),
  FRG("mutex_lock",		1, pl_mutex_lock,		0),
  FRG("mutex_trylock",		1, pl_mutex_trylock,		0),
  FRG("mutex_unlock",		1, pl_mutex_unlock,		0),
  FRG("mutex_unlock_all",	0, pl_mutex_unlock_all,		0),
  FRG("current_mutex",		3, pl_current_mutex,	     NDET),
  FRG("open_xterm",		4, pl_open_xterm,		0),
#endif

  FRG("thread_self",		1, pl_thread_self,	        0),
  FRG("with_mutex",		2, pl_with_mutex,	     META),
  FRG("$get_pid",		1, pl_get_pid,			0),

  /* DO NOT ADD ENTRIES BELOW THIS ONE */
  FRG((char *)NULL,		0, (Func)NULL,			0)
};


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The extensions chain is used   to allow calling PL_register_extensions()
*before* PL_initialise() to get foreign   extensions in embedded systems
defined before the state is loaded, so executing directives in the state
can use foreign extensions.

If an extension is registered before the  system extension is loaded, it
will be added to the chain. Right  after the system registers the system
predicates, the extensions will be registered.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

struct extension_cell
{ PL_extension *extensions;
  char *module;
  ExtensionCell next;
};

#define ext_head		(GD->foreign._ext_head)
#define ext_tail		(GD->foreign._ext_tail)
#define extensions_loaded	(GD->foreign._loaded)

static char *
dupStr(const char *str)
{ if (str)
  { int len = strlen(str)+1;
    char *m = PL_malloc(len);
    memcpy(m, str, len);
    return m;
  }
  return NULL;
}


static PL_extension *
dupExtensions(const PL_extension *e)
{ int i;
  PL_extension *dup, *o;
  int len = 0;

  while(e[len++].predicate_name)
    ;
  o = dup = PL_malloc(len*sizeof(*e));

  for ( i=0; i<len; i++, o++, e++)
  { o->predicate_name = dupStr(e->predicate_name);
    o->arity = e->arity;
    o->function = e->function;
    o->flags = e->flags;
  }

  return dup;
}


void
rememberExtensions(const char *module, const PL_extension *e)
{ ExtensionCell cell = PL_malloc(sizeof *cell);

  cell->extensions = dupExtensions(e);
  cell->next = NULL;
  cell->module = dupStr(module);
  
  if ( ext_tail )
  { ext_tail->next = cell;
    ext_tail = cell;
  } else
  { ext_head = ext_tail = cell;
  }
}


void
cleanupExtensions(void)
{ ExtensionCell c, next;

  for(c=ext_head; c; c=next)
  { next = c->next;
    if (c->module)
      PL_free(c->module);

    if (c->extensions)
    { PL_extension *e = c->extensions;

      for(;e->predicate_name; e++)
	PL_free(e->predicate_name);

      PL_free(c->extensions);
    }

    PL_free(c);
  }

  ext_head = ext_tail = NULL;
}



static void
registerBuiltins(const PL_extension *f)
{ Module m = MODULE_system;

  for(; f->predicate_name; f++)
  { Definition def;
    atom_t name	= PL_new_atom(f->predicate_name);
    functor_t fdef = lookupFunctorDef(name, f->arity);

    PL_unregister_atom(name);
    def = lookupProcedure(fdef, m)->definition;
    set(def, FOREIGN|SYSTEM|HIDE_CHILDS|LOCKED);

    if ( f->flags & PL_FA_NOTRACE )	     clear(def, TRACE_ME);
    if ( f->flags & PL_FA_TRANSPARENT )	     set(def, METAPRED);
    if ( f->flags & PL_FA_NONDETERMINISTIC ) set(def, NONDETERMINISTIC);
    if ( f->flags & PL_FA_VARARGS )	     set(def, P_VARARG);
    if ( f->flags & PL_FA_CREF )	     set(def, P_FOREIGN_CREF);

    def->definition.function = f->function;
    def->indexPattern = 0;
    def->indexCardinality = 0;
    if ( false(def, NONDETERMINISTIC) && 
	 f->arity <= 2 )
      set(valueFunctor(fdef), INLINE_F);
  }
}


#define DECL_PLIST(id) \
	extern const PL_extension PL_predicates_from_ ## id[]
#define REG_PLIST(id) \
	registerBuiltins(PL_predicates_from_ ## id)

DECL_PLIST(atom);
DECL_PLIST(arith);
DECL_PLIST(bag);
DECL_PLIST(comp);
DECL_PLIST(flag);
DECL_PLIST(list);
DECL_PLIST(module);
DECL_PLIST(prims);
DECL_PLIST(read);
DECL_PLIST(wam);
DECL_PLIST(thread);
DECL_PLIST(profile);
DECL_PLIST(wic);
DECL_PLIST(attvar);
DECL_PLIST(gvar);
DECL_PLIST(win);
DECL_PLIST(file);
DECL_PLIST(btree);
DECL_PLIST(ctype);
DECL_PLIST(tai);

void
initBuildIns(void)
{ ExtensionCell ecell;
  Module m = MODULE_system;

  registerBuiltins(foreigns);
  REG_PLIST(atom);
  REG_PLIST(arith);
  REG_PLIST(bag);
  REG_PLIST(comp);
  REG_PLIST(flag);
  REG_PLIST(list);
  REG_PLIST(module);
  REG_PLIST(prims);
  REG_PLIST(read);
  REG_PLIST(wam);
  REG_PLIST(thread);
  REG_PLIST(profile);
  REG_PLIST(wic);
  REG_PLIST(file);
  REG_PLIST(btree);
  REG_PLIST(ctype);
  REG_PLIST(tai);
#ifdef O_ATTVAR
  REG_PLIST(attvar);
#endif
#ifdef O_GVAR
  REG_PLIST(gvar);
#endif
#ifdef __WIN32__
  REG_PLIST(win);
#endif

  PROCEDURE_garbage_collect0 = lookupProcedure(FUNCTOR_dgarbage_collect1, m);
  PROCEDURE_block3	     = lookupProcedure(FUNCTOR_block3, 		  m);
  PROCEDURE_catch3           = lookupProcedure(FUNCTOR_catch3, 		  m);
  PROCEDURE_true0            = lookupProcedure(FUNCTOR_true0, 		  m);
  PROCEDURE_fail0            = lookupProcedure(FUNCTOR_fail0, 		  m);
  PROCEDURE_print_message2   = lookupProcedure(FUNCTOR_print_message2, 	  m);
  PROCEDURE_dcall1	     = lookupProcedure(FUNCTOR_dcall1,		  m);
  PROCEDURE_call_cleanup3    = lookupProcedure(FUNCTOR_call_cleanup3,	  m); 
  PROCEDURE_dthread_init0    = lookupProcedure(FUNCTOR_dthread_init0,	  m);
  PROCEDURE_dc_call_prolog   = lookupProcedure(FUNCTOR_dc_call_prolog0,   m);
#ifdef O_ATTVAR
  PROCEDURE_dwakeup1	     = lookupProcedure(FUNCTOR_dwakeup1,	  m);
#endif
  PROCEDURE_exception_hook4  = 
	PL_predicate("prolog_exception_hook", 4, "user");
					/* allow debugging in call/1 */
  clear(PROCEDURE_dcall1->definition, HIDE_CHILDS);
  set(PROCEDURE_dcall1->definition, DYNAMIC);

  for( ecell = ext_head; ecell; ecell = ecell->next )
    bindExtensions(ecell->module, ecell->extensions);

  extensions_loaded = TRUE;
}

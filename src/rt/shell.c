//
//  Copyright (C) 2011-2023  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "common.h"
#include "diag.h"
#include "eval.h"
#include "hash.h"
#include "lib.h"
#include "lower.h"
#include "phase.h"
#include "printer.h"
#include "rt/model.h"
#include "rt/structs.h"
#include "scan.h"
#include "shell.h"
#include "tree.h"
#include "type.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <readline/readline.h>
#include <readline/history.h>

#undef DLLEXPORT
#include <tcl.h>

#define TIME_BUFSZ 32

typedef struct {
   const char     *name;
   Tcl_ObjCmdProc *fn;
   const char     *help;
} shell_cmd_t;

typedef struct {
   rt_signal_t  *signal;
   ident_t       name;
   ident_t       path;
   print_func_t *printer;
   rt_watch_t   *watch;
   tcl_shell_t  *owner;
} shell_signal_t;

typedef char *(*get_line_fn_t)(tcl_shell_t *);

typedef struct _tcl_shell {
   char            *prompt;
   Tcl_Interp      *interp;
   shell_cmd_t     *cmds;
   size_t           ncmds;
   size_t           cmdalloc;
   rt_model_t      *model;
   tree_t           top;
   rt_scope_t      *root;
   shell_signal_t  *signals;
   unsigned         nsignals;
   hash_t          *namemap;
   jit_t           *jit;
   int64_t          now_var;
   unsigned         deltas_var;
   printer_t       *printer;
   get_line_fn_t    getline;
   jit_factory_t    make_jit;
   unit_registry_t *registry;
   shell_handler_t  handler;
   bool             quit;
} tcl_shell_t;

static __thread tcl_shell_t *rl_shell = NULL;

__attribute__((format(printf, 2, 3)))
static int tcl_error(tcl_shell_t *sh, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char *buf LOCAL = color_vasprintf(fmt, ap);
   va_end(ap);

   Tcl_SetObjResult(sh->interp, Tcl_NewStringObj(buf, -1));
   return TCL_ERROR;
}

static int syntax_error(tcl_shell_t *sh, Tcl_Obj *const objv[])
{
   return tcl_error(sh, "syntax error, enter $bold$help %s$$ for usage",
                    Tcl_GetString(objv[0]));
}

__attribute__((format(printf, 2, 3)))
static void shell_printf(tcl_shell_t *sh, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   if (sh->handler.stdout_write != NULL) {
      char *buf LOCAL = color_vasprintf(fmt, ap);
      (*sh->handler.stdout_write)(buf, strlen(buf), sh->handler.context);
   }
   else
      wrapped_vprintf(fmt, ap);

   va_end(ap);
}

static bool shell_has_model(tcl_shell_t *sh)
{
   if (sh->model == NULL) {
      tcl_error(sh, "no simulation loaded, try the $bold$elaborate$$ "
                "command first");
      return false;
   }

   return true;
}

static void shell_clear_model(tcl_shell_t *sh)
{
   if (sh->model == NULL)
      return;

   model_free(sh->model);
   hash_free(sh->namemap);

   sh->model = NULL;
   sh->namemap = NULL;

   if (sh->handler.quit_sim != NULL)
      (*sh->handler.quit_sim)(sh->handler.context);
}

static void shell_next_time_step(rt_model_t *m, void *user)
{
   tcl_shell_t *sh = user;
   assert(sh->handler.next_time_step != NULL);

   uint64_t now = model_now(m, NULL);
   (*sh->handler.next_time_step)(now, sh->handler.context);

   model_set_global_cb(sh->model, RT_NEXT_TIME_STEP, shell_next_time_step, sh);
}

static void shell_create_model(tcl_shell_t *sh)
{
   assert(sh->model == NULL);

   sh->model = model_new(sh->top, sh->jit);

   if (sh->handler.next_time_step != NULL)
      model_set_global_cb(sh->model, RT_NEXT_TIME_STEP,
                          shell_next_time_step, sh);

   model_reset(sh->model);

   if ((sh->root = find_scope(sh->model, tree_stmt(sh->top, 0))) == NULL)
      fatal_trace("cannot find root scope");
}

static void shell_update_now(tcl_shell_t *sh)
{
   sh->now_var = model_now(sh->model, &sh->deltas_var);

   Tcl_UpdateLinkedVar(sh->interp, "now");
   Tcl_UpdateLinkedVar(sh->interp, "deltas");
}

static bool shell_get_printer(tcl_shell_t *sh, shell_signal_t *ss)
{
   if (ss->printer == NULL)
      ss->printer = printer_for(sh->printer, tree_type(ss->signal->where));

   if (ss->printer == NULL) {
      tcl_error(sh, "cannot display type %s",
                type_pp(tree_type(ss->signal->where)));
      return false;
   }

   return true;
}

static void shell_add_cmd(tcl_shell_t *sh, const char *name, Tcl_ObjCmdProc fn,
                          const char *help)
{
   shell_cmd_t cmd = { name, fn, help };

   if (sh->cmdalloc == sh->ncmds) {
      sh->cmdalloc = MAX(sh->cmdalloc * 2, 16);
      sh->cmds = xrealloc_array(sh->cmds, sh->cmdalloc, sizeof(shell_cmd_t));
   }

   sh->cmds[sh->ncmds++] = cmd;

   Tcl_CreateObjCommand(sh->interp, name, fn, sh, NULL);
}

static void shell_event_cb(uint64_t now, rt_signal_t *s, rt_watch_t *w,
                           void *user)
{
   shell_signal_t *ss = user;
   shell_handler_t *h = &(ss->owner->handler);

   if (h->signal_update != NULL) {
      const char *enc = print_signal(ss->printer, ss->signal, PRINT_F_ENCODE);
      (*h->signal_update)(ss->path, now, s, enc, h->context);
   }
}

static void recreate_signals(tcl_shell_t *sh, rt_scope_t *scope,
                             shell_signal_t **wptr)
{
   for (list_iter(rt_signal_t *, s, scope->signals)) {
      shell_signal_t *ss = (*wptr)++;
      assert(ss->name == ident_downcase(tree_ident(s->where)));
      ss->signal = s;

      if (ss->watch != NULL)
         ss->watch = model_set_event_cb(ss->owner->model, ss->signal,
                                        shell_event_cb, ss, true);
   }

   for (list_iter(rt_alias_t *, a, scope->aliases)) {
      shell_signal_t *ss = (*wptr)++;
      assert(ss->name == ident_downcase(tree_ident(a->where)));
      ss->signal = a->signal;

      if (ss->watch != NULL)
         ss->watch = model_set_event_cb(sh->model, ss->signal,
                                        shell_event_cb, ss, true);
   }

   for (list_iter(rt_scope_t *, child, scope->children))
      recreate_signals(sh, child, wptr);
}

static const char restart_help[] =
   "Restart the simulation";

static int shell_cmd_restart(ClientData cd, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   if (!shell_has_model(sh))
      return TCL_ERROR;

   model_free(sh->model);
   sh->model = NULL;

   jit_reset(sh->jit);

   shell_create_model(sh);

   shell_signal_t *wptr = sh->signals;
   recreate_signals(sh, sh->root, &wptr);
   assert(wptr == sh->signals + sh->nsignals);

   shell_update_now(sh);

   if (sh->handler.restart_sim != NULL)
      (*sh->handler.restart_sim)(sh->handler.context);

   return TCL_OK;
}

static const char run_help[] =
   "Start or resume the simulation";

static int shell_cmd_run(ClientData cd, Tcl_Interp *interp,
                         int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;
   static bool sim_running = false;

   if (!shell_has_model(sh))
      return TCL_ERROR;
   else if (sim_running)
      return tcl_error(sh, "simulation already running");

   uint64_t stop_time = UINT64_MAX;
   if (objc == 3) {
      Tcl_WideInt base;
      int error = Tcl_GetWideIntFromObj(interp, objv[1], &base);
      if (error != TCL_OK || base <= 0)
         return tcl_error(sh, "invalid time");

      const char *unit = Tcl_GetString(objv[2]);

      uint64_t mult;
      if      (strcmp(unit, "fs") == 0) mult = 1;
      else if (strcmp(unit, "ps") == 0) mult = 1000;
      else if (strcmp(unit, "ns") == 0) mult = 1000000;
      else if (strcmp(unit, "us") == 0) mult = 1000000000;
      else if (strcmp(unit, "ms") == 0) mult = 1000000000000;
      else
         return tcl_error(sh, "invalid time unit %s", unit);

      stop_time = model_now(sh->model, NULL) + (base * mult);
   }
   else if (objc != 1)
      return tcl_error(sh, "usage: $bold$run [time units]$$");


   sim_running = true;
   model_run(sh->model, stop_time);
   sim_running = false;

   shell_update_now(sh);

   return TCL_OK;
}

static const char find_help[] =
   "Find signals and other objects in the design\n"
   "\n"
   "Syntax:\n"
   "  find signals <name>\n"
   "\n"
   "Examples:\n"
   "  find signals /*\tList all signals in the design\n"
   "  find signals /uut/x*\tAll signals in instance UUT that start with X\n";

static int shell_cmd_find(ClientData cd, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   if (objc != 3 || strcmp(Tcl_GetString(objv[1]), "signals") != 0)
      goto usage;
   else if (!shell_has_model(sh))
      return TCL_ERROR;

   const char *glob = Tcl_GetString(objv[2]);
   Tcl_Obj *result = Tcl_NewListObj(0, NULL);

   for (int i = 0; i < sh->nsignals; i++) {
      if (!ident_glob(sh->signals[i].path, glob, -1))
         continue;

      Tcl_Obj *obj = Tcl_NewStringObj(istr(sh->signals[i].path), -1);
      Tcl_ListObjAppendElement(interp, result, obj);
   }

   Tcl_SetObjResult(interp, result);
   return TCL_OK;

 usage:
   return syntax_error(sh, objv);
}

static const char analyse_help[] =
   "Analyse a VHDL source file\n"
   "\n"
   "Syntax:\n"
   "  analyse [options] <file> [<file> ...]\n"
   "\n"
   "Note \"vcom\" is an alias of this command.\n"
   "\n"
   "Examples:\n"
   "  analyse file.vhd\n"
   "  vcom file1.vhd file2.vhd\n";

static int shell_cmd_analyse(ClientData cd, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   int pos = 1;
   for (const char *opt; (opt = Tcl_GetString(objv[pos]))[0] == '-'; pos++)
      goto usage;

   if (pos == objc)
      goto usage;

   reset_error_count();

   for (; pos < objc; pos++) {
      const char *fname = Tcl_GetString(objv[pos]);

      file_info_t info;
      if (access(fname, R_OK) != 0 || !get_file_info(fname, &info))
         return tcl_error(sh, "cannot open %s: %s", fname, strerror(errno));
      else if (info.type == FILE_DIR)
         return tcl_error(sh, "%s is a directory", fname);
      else if (info.type != FILE_REGULAR)
         return tcl_error(sh, "%s is not a regular file", fname);

      analyse_file(fname, sh->jit, sh->registry);
   }

   return error_count() > 0 ? TCL_ERROR : TCL_OK;

 usage:
   return syntax_error(sh, objv);
}

static const char elaborate_help[] =
   "Elaborate a design hierarchy\n"
   "\n"
   "Syntax:\n"
   "  elaborate [options] <toplevel>\n"
   "\n"
   "Note \"vsim\" is an alias of this command.\n"
   "\n"
   "Options:\n"
   "\n"
   "Examples:\n"
   "  elaborate toplevel\n"
   "  vsim toplevel\n";

static int shell_cmd_elaborate(ClientData cd, Tcl_Interp *interp,
                               int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;
   LOCAL_TEXT_BUF tb = tb_new();

   int pos = 1;
   for (const char *opt; (opt = Tcl_GetString(objv[pos]))[0] == '-'; pos++)
      goto usage;

   if (pos + 1 != objc)
      goto usage;

   lib_t work = lib_work();

   tb_istr(tb, lib_name(work));
   tb_append(tb, '.');
   tb_cat(tb, Tcl_GetString(objv[pos]));
   tb_upcase(tb);

   tree_t unit = lib_get(lib_work(), ident_new(tb_get(tb)));
   if (unit == NULL)
      return tcl_error(sh, "cannot find unit %s in library %s",
                       Tcl_GetString(objv[pos]), istr(lib_name(work)));

   shell_clear_model(sh);

   reset_error_count();

   // Recreate the JIT instance and unit registry as it may have
   // references to stale code
   jit_free(sh->jit);
   unit_registry_free(sh->registry);
   sh->registry = unit_registry_new();
   sh->jit = (*sh->make_jit)(sh->registry);

   jit_enable_runtime(sh->jit, false);

   tree_t top = elab(unit, sh->jit, sh->registry, NULL);
   if (top == NULL)
      return TCL_ERROR;

   shell_reset(sh, top);
   return TCL_OK;

 usage:
   return syntax_error(sh, objv);
}

static const char examine_help[] =
   "Display current value of one of more signals\n"
   "\n"
   "Syntax:\n"
   "  examine [options] <name>...\n"
   "\n"
   "Note \"exa\" is an alias of this command.\n"
   "\n"
   "Options:\n"
   "  -radix <type>\tFormat as hexadecimal, decimal, or binary.\n"
   "  -<radix>\tAlias of \"-radix <radix>\".\n"
   "\n"
   "Examples:\n"
   "  examine /uut/foo\n"
   "  exa -hex sig\n";

static bool parse_radix(const char *str, print_flags_t *flags)
{
   if (strcmp(str, "binary") == 0 || strcmp(str, "bin") == 0
       || strcmp(str, "b") == 0) {
      *flags &= ~PRINT_F_RADIX;
      *flags |= PRINT_F_BIN;
      return true;
   }
   else if (strcmp(str, "-hexadecimal") == 0 || strcmp(str, "hex") == 0
            || strcmp(str, "h") == 0) {
      *flags &= ~PRINT_F_RADIX;
      *flags |= PRINT_F_HEX;
      return true;
   }
   else
      return false;
}

const char *next_option(int *pos, int objc, Tcl_Obj *const objv[])
{
   if (*pos >= objc)
      return NULL;

   const char *opt = Tcl_GetString(objv[*pos]);
   if (opt[0] != '-')
      return NULL;

   (*pos)++;
   return opt;
}

static int shell_cmd_examine(ClientData cd, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   if (!shell_has_model(sh))
      return TCL_ERROR;

   print_flags_t flags = 0;
   int pos = 1;
   for (const char *opt; (opt = next_option(&pos, objc, objv)); ) {
      if (parse_radix(opt + 1, &flags))
         continue;
      else if (strcmp(opt, "-radix") == 0 && pos + 1 < objc) {
         const char *arg = Tcl_GetString(objv[pos++]);
         if (!parse_radix(arg, &flags))
            goto usage;
      }
      else
         goto usage;
   }

   if (pos == objc)
      goto usage;

   const int count = objc - pos;
   Tcl_Obj *single[1], **result = single;

   if (count > 1)
      result = xmalloc_array(count, sizeof(Tcl_Obj *));

   for (int i = 0; pos < objc; pos++, i++) {
      const char *name = Tcl_GetString(objv[pos]);
      shell_signal_t *ss = hash_get(sh->namemap, ident_new(name));
      if (ss == NULL)
         return tcl_error(sh, "cannot find name '%s'", name);

      if (!shell_get_printer(sh, ss))
         return TCL_ERROR;

      const char *str = print_signal(ss->printer, ss->signal, flags);
      result[i] = Tcl_NewStringObj(str, -1);
   }

   if (count > 1) {
      Tcl_Obj *list = Tcl_NewListObj(count, result);
      Tcl_SetObjResult(interp, list);
      free(result);
   }
   else
      Tcl_SetObjResult(interp, result[0]);

   return TCL_OK;

 usage:
   return syntax_error(sh, objv);
}

static const char force_help[] =
   "Force the value of a signal\n"
   "\n"
   "Syntax:\n"
   "  force [<signal> <value>]\n"
   "\n"
   "Value can be either an enumeration literal ('1', true), an integer "
   "(42, 0), or a bit string literal (\"10111\") and must be appropriate "
   "for the signal type. Without arguments lists all currently forced "
   "signals.\n"
   "\n"
   "Examples:\n"
   "  force /uut/foo '1'\n"
   "  force /bitvec \"10011\"\n";

static int shell_cmd_force(ClientData cd, Tcl_Interp *interp,
                           int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   if (!shell_has_model(sh))
      return TCL_ERROR;
   else if (objc != 3 && objc != 1)
      return syntax_error(sh, objv);

   if (objc == 1) {
      for (int i = 0; i < sh->nsignals; i++) {
         shell_signal_t *ss = &(sh->signals[i]);
         if (!(ss->signal->nexus.flags & NET_F_FORCED))
            continue;

         if (!shell_get_printer(sh, ss))
            return TCL_ERROR;

         const size_t nbytes = ss->signal->shared.size;
         uint8_t *value LOCAL = xmalloc(nbytes);
         get_forcing_value(ss->signal, value);

         shell_printf(sh, "force %s %s\n", istr(ss->path),
                      print_raw(ss->printer, value, nbytes, 0));
      }

      return TCL_OK;
   }

   const char *signame = Tcl_GetString(objv[1]);
   const char *valstr = Tcl_GetString(objv[2]);

   shell_signal_t *ss = hash_get(sh->namemap, ident_new(signame));
   if (ss == NULL)
      return tcl_error(sh, "cannot find signal '%s'", signame);

   type_t type = tree_type(ss->signal->where);

   parsed_value_t value;
   if (!parse_value(type, valstr, &value))
      return tcl_error(sh, "value '%s' is not valid for type %s",
                       valstr, type_pp(type));

   if (type_is_scalar(type))
      force_signal(sh->model, ss->signal, &value.integer, 0, 1);
   else if (type_is_character_array(type)) {
      const int width = signal_width(ss->signal);
      if (value.enums->count != width) {
         tcl_error(sh, "expected %d elements for signal %s but have %d", width,
                   signame, value.enums->count);
         free(value.enums);
         return TCL_ERROR;
      }

      force_signal(sh->model, ss->signal, value.enums->values, 0, width);
      free(value.enums);
   }
   else
      return tcl_error(sh, "cannot force signals of type %s", type_pp(type));

   return TCL_OK;
}

static const char noforce_help[] =
   "Stop forcing the value of signals\n"
   "\n"
   "Syntax:\n"
   "  noforce <signal>...\n"
   "  noforce *\n"
   "\n"
   "The second form stops forcing all currently forced signals.\n"
   "\n"
   "Examples:\n"
   "  noforce /uut/foo /baz\n";

static int shell_cmd_noforce(ClientData cd, Tcl_Interp *interp,
                             int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   if (!shell_has_model(sh))
      return TCL_ERROR;
   else if (objc == 1)
      return syntax_error(sh, objv);

   for (int i = 1; i < objc; i++) {
      const char *signame = Tcl_GetString(objv[i]);
      if (strcmp(signame, "*") == 0) {
         for (int i = 0; i < sh->nsignals; i++) {
            shell_signal_t *ss = &(sh->signals[i]);
            if (ss->signal->nexus.flags & NET_F_FORCED)
               release_signal(sh->model, ss->signal, 0,
                              signal_width(ss->signal));
         }
      }
      else {
         shell_signal_t *ss = hash_get(sh->namemap, ident_new(signame));
         if (ss == NULL)
            return tcl_error(sh, "cannot find signal '%s'", signame);

         if (!(ss->signal->nexus.flags & NET_F_FORCED))
            return tcl_error(sh, "signal %s is not forced", signame);

         release_signal(sh->model, ss->signal, 0, signal_width(ss->signal));
      }
   }

   return TCL_OK;
}

static const char add_help[] =
   "Add signals and other objects to the display\n"
   "\n"
   "Syntax:\n"
   "  add wave <name>...\n"
   "\n"
   "Examples:\n"
   "  add wave /*\tAdd all signals to waveform\n";

static int shell_cmd_add(ClientData cd, Tcl_Interp *interp,
                         int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   if (objc < 3 || strcmp(Tcl_GetString(objv[1]), "wave") != 0)
      goto usage;
   else if (!shell_has_model(sh))
      return TCL_ERROR;

   const int nglobs = objc - 2;
   char **globs LOCAL = xmalloc_array(nglobs, sizeof(char *));
   for (int i = 0; i < nglobs; i++)
      globs[i] = Tcl_GetString(objv[i + 2]);

   for (int i = 0; i < sh->nsignals; i++) {
      shell_signal_t *ss = &(sh->signals[i]);

      bool match = false;
      for (int j = 0; j < nglobs; j++)
         match |= ident_glob(ss->path, globs[j], -1);

      if (!match)
         continue;
      else if (!shell_get_printer(sh, ss))
         return TCL_ERROR;

      if (sh->handler.add_wave != NULL) {
         const char *enc =
            print_signal(ss->printer, ss->signal, PRINT_F_ENCODE);
         (*sh->handler.add_wave)(ss->path, enc, sh->handler.context);
      }

      if (ss->watch == NULL)
        ss->watch = model_set_event_cb(sh->model, ss->signal,
                                       shell_event_cb, ss, true);
   }

   return TCL_OK;

 usage:
   return syntax_error(sh, objv);
}

static const char quit_help[] =
   "Exit the simulator or unload the current design\n"
   "\n"
   "Syntax:\n"
   "  quit [-sim]\n"
   "\n"
   "Options:\n"
   "  -sim\t\tUnload the current simulation but do not exit the program.\n";

static int shell_cmd_quit(ClientData cd, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   bool quit_sim = false;
   int pos = 1;
   for (const char *opt; (opt = next_option(&pos, objc, objv)); ) {
      if (strcmp(opt, "-sim") == 0)
         quit_sim = true;
      else
         goto usage;
   }

   if (pos != objc)
      goto usage;

   if (quit_sim) {
      if (!shell_has_model(sh))
         return TCL_ERROR;
      else
         shell_clear_model(sh);
   }
   else {
      sh->quit = true;

      if (sh->handler.exit != NULL)
         (*sh->handler.exit)(0, sh->handler.context);
   }

   return TCL_OK;

 usage:
   return syntax_error(sh, objv);
}

static const char exit_help[] =
   "Exit the simulator and return a status code\n"
   "\n"
   "Syntax:\n"
   "  exit [-code <integer>]\n"
   "\n"
   "Options:\n"
   "  -code <integer>\tStatus code to return to shell.\n";

static int shell_cmd_exit(ClientData cd, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   int pos = 1, status = EXIT_SUCCESS;
   for (const char *opt; (opt = next_option(&pos, objc, objv)); ) {
      if (strcmp(opt, "-code") == 0 && pos < objc)
         status = atoi(Tcl_GetString(objv[pos++]));
      else
         goto usage;
   }

   if (pos != objc)
      goto usage;

   if (sh->handler.exit != NULL)
      (*sh->handler.exit)(status, sh->handler.context);

   Tcl_Exit(status);

 usage:
   return syntax_error(sh, objv);
}

static const char help_help[] =
   "Display list of commands or detailed help\n"
   "\n"
   "Use $bold$help <command>$$ to display detailed usage of a particular\n"
   "command.\n";

static int shell_cmd_help(ClientData cd, Tcl_Interp *interp,
                          int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;

   if (objc == 2) {
      const char *which = Tcl_GetString(objv[1]);
      for (int i = 0; i < sh->ncmds; i++) {
         if (strcmp(sh->cmds[i].name, which) == 0) {
            shell_printf(sh, "%s", sh->cmds[i].help);
            return TCL_OK;
         }
      }

      return tcl_error(sh, "invalid command '%s'", which);
   }
   else if (objc != 1)
      return tcl_error(sh, "syntax error, try $bold$help$$");

   shell_printf(sh, "List of supported commands:\n");

   for (shell_cmd_t *c = sh->cmds; c < sh->cmds + sh->ncmds; c++) {
      const int linelen = strchrnul(c->help, '\n') - c->help;
      shell_printf(sh, "  $bold$%-16s$$%.*s\n", c->name, linelen, c->help);
   }

   shell_printf(sh, "\n");
   shell_printf(sh, "Use $bold$help <command>$$ for detailed usage "
                "of a particular command. Standard TCL commands are "
                "also accepted.\n");

   return TCL_OK;
}

static const char copyright_help[] = "Display copyright information";

static int shell_cmd_copyright(ClientData cd, Tcl_Interp *interp,
                               int objc, Tcl_Obj *const objv[])
{
   tcl_shell_t *sh = cd;
   extern char copy_string[];
   shell_printf(sh, "%s\n", copy_string);
   return TCL_OK;
}

static char *shell_list_generator(const char *script, const char *text,
                                  int state, int prefix)
{
   static Tcl_Obj *list = NULL;
   static int index, len, max;

   if (!state) {
      if (Tcl_Eval(rl_shell->interp, script) != TCL_OK)
         return NULL;

      list = Tcl_GetObjResult(rl_shell->interp);

      if (Tcl_ListObjLength(rl_shell->interp, list, &max) != TCL_OK)
         return NULL;

      index = 0;
      len = strlen(text);
   }

   while (index < max) {
      Tcl_Obj *obj;
      if (Tcl_ListObjIndex(rl_shell->interp, list, index++, &obj) != TCL_OK)
         return NULL;

      const char *str = Tcl_GetString(obj);
      if (strncmp(str, text + prefix, len - prefix) == 0) {
         if (prefix == 0)
            return xstrdup(str);
         else {
            assert(len >= prefix);
            const size_t complen = strlen(str);
            char *buf = xmalloc(prefix + complen + 1);
            memcpy(buf, text, prefix);
            memcpy(buf + prefix, str, complen + 1);
            return buf;
         }
      }
   }

   return NULL;
}

static char *shell_command_generator(const char *text, int state)
{
   return shell_list_generator("info commands", text, state, 0);
}

static char *shell_variable_generator(const char *text, int state)
{
   return shell_list_generator("info vars", text, state, 1);
}

static char **shell_tab_completion(const char *text, int start, int end)
{
   rl_attempted_completion_over = 0;

   if (text[0] == '$')
      return rl_completion_matches(text, shell_variable_generator);

   // Determine if we are completing a TCL command or not
   int pos = start - 1;
   for (; pos >= 0 && isspace_iso88591(rl_line_buffer[pos]); pos--);

   if (pos == -1 || rl_line_buffer[pos] == '[')
      return rl_completion_matches(text, shell_command_generator);

   return NULL;
}

static char *shell_completing_get_line(tcl_shell_t *sh)
{
   rl_attempted_completion_function = shell_tab_completion;
   rl_completer_quote_characters = "\"'";
   rl_completer_word_break_characters = " \t\r\n[]{}";
   rl_special_prefixes = "$";
   rl_shell = sh;

   char *buf = readline(sh->prompt);
   if ((buf != NULL) && (*buf != '\0'))
      add_history(buf);

   rl_shell = NULL;
   return buf;
}


static char *shell_raw_get_line(tcl_shell_t *sh)
{
   fputs(sh->prompt, stdout);
   fflush(stdout);

   LOCAL_TEXT_BUF tb = tb_new();

   size_t off = 0;
   for (;;) {
      int ch = fgetc(stdin);
      fputc(ch, stdout);
      switch (ch) {
      case EOF:
         return (off > 0) ? tb_claim(tb) : NULL;
      case '\n':
         return tb_claim(tb);
      default:
         tb_append(tb, ch);
      }
   }
}

void shell_print_banner(tcl_shell_t *sh)
{
   extern const char version_string[];
   shell_printf(sh, "\n");

   if (sh->handler.stdout_write == NULL)
      print_centred(version_string);
   else
      shell_printf(sh, "\t%s", version_string);

   static const char blurb[] =
      "\n\nThis program comes with ABSOLUTELY NO WARRANTY. This is free "
      "software, and you are welcome to redistribute it under certain "
      "conditions; type $bold$copyright$$ for details.\n\n"
      "Type $bold$help$$ for a list of supported commands.\n\n";

   shell_printf(sh, blurb);
}

static int compare_shell_cmd(const void *a, const void *b)
{
   return strcmp(((shell_cmd_t *)a)->name, ((shell_cmd_t *)b)->name);
}

tcl_shell_t *shell_new(jit_factory_t make_jit)
{
   tcl_shell_t *sh = xcalloc(sizeof(tcl_shell_t));
#ifdef RL_VERSION_MAJOR
   sh->prompt   = color_asprintf("\001$+cyan$\002%%\001$$\002 ");
#else
   sh->prompt   = color_asprintf("$+cyan$%%$$ ");
#endif
   sh->interp   = Tcl_CreateInterp();
   sh->make_jit = make_jit;
   sh->registry = unit_registry_new();
   sh->jit      = make_jit ? (*make_jit)(sh->registry) : NULL;
   sh->printer  = printer_new();

   if (isatty(fileno(stdin)))
      sh->getline = shell_completing_get_line;
   else
      sh->getline = shell_raw_get_line;

   Tcl_LinkVar(sh->interp, "now", (char *)&sh->now_var,
               TCL_LINK_WIDE_INT | TCL_LINK_READ_ONLY);
   Tcl_LinkVar(sh->interp, "deltas", (char *)&sh->deltas_var,
               TCL_LINK_UINT | TCL_LINK_READ_ONLY);

   atexit(Tcl_Finalize);

   Tcl_DeleteCommand(sh->interp, "exit");

   shell_add_cmd(sh, "help", shell_cmd_help, help_help);
   shell_add_cmd(sh, "exit", shell_cmd_exit, exit_help);
   shell_add_cmd(sh, "copyright", shell_cmd_copyright, copyright_help);
   shell_add_cmd(sh, "find", shell_cmd_find, find_help);
   shell_add_cmd(sh, "run", shell_cmd_run, run_help);
   shell_add_cmd(sh, "restart", shell_cmd_restart, restart_help);
   shell_add_cmd(sh, "analyse", shell_cmd_analyse, analyse_help);
   shell_add_cmd(sh, "vcom", shell_cmd_analyse, analyse_help);
   shell_add_cmd(sh, "elaborate", shell_cmd_elaborate, elaborate_help);
   shell_add_cmd(sh, "vsim", shell_cmd_elaborate, elaborate_help);
   shell_add_cmd(sh, "examine", shell_cmd_examine, examine_help);
   shell_add_cmd(sh, "exa", shell_cmd_examine, examine_help);
   shell_add_cmd(sh, "add", shell_cmd_add, add_help);
   shell_add_cmd(sh, "quit", shell_cmd_quit, quit_help);
   shell_add_cmd(sh, "force", shell_cmd_force, force_help);
   shell_add_cmd(sh, "noforce", shell_cmd_noforce, noforce_help);

   qsort(sh->cmds, sh->ncmds, sizeof(shell_cmd_t), compare_shell_cmd);

   return sh;
}

void shell_free(tcl_shell_t *sh)
{
   if (sh->model != NULL) {
      model_free(sh->model);
      hash_free(sh->namemap);
      free(sh->signals);
   }

   if (sh->jit != NULL)
      jit_free(sh->jit);

   unit_registry_free(sh->registry);
   printer_free(sh->printer);
   Tcl_DeleteInterp(sh->interp);

   free(sh->prompt);
   free(sh->cmds);
   free(sh);
}

bool shell_eval(tcl_shell_t *sh, const char *script, const char **result)
{
   const int code = Tcl_Eval(sh->interp, script);
   const char *str = Tcl_GetStringResult(sh->interp);

   switch (code) {
   case TCL_OK:
      if (result != NULL)
         *result = str;
      return true;
   case TCL_ERROR:
      if (str != NULL && *str != '\0')
         errorf("%s", str);
      return false;
   default:
      warnf("Tcl_Eval returned unknown code %d", code);
      return false;
   }
}

static int count_signals(rt_scope_t *scope)
{
   int total = list_size(scope->signals) + list_size(scope->aliases);

   list_foreach(rt_scope_t *, child, scope->children)
      total += count_signals(child);

   return total;
}

static void recurse_signals(tcl_shell_t *sh, rt_scope_t *scope,
                            text_buf_t *path, shell_signal_t **wptr)
{
   const int base = tb_len(path);

   list_foreach(rt_signal_t *, s, scope->signals) {
      shell_signal_t *ss = (*wptr)++;
      ss->signal = s;
      ss->name = ident_downcase(tree_ident(s->where));
      ss->owner = sh;

      tb_istr(path, ss->name);
      ss->path = ident_new(tb_get(path));
      tb_trim(path, base);

      hash_put(sh->namemap, ss->path, ss);
   }

   list_foreach(rt_alias_t *, a, scope->aliases) {
      shell_signal_t *ss = (*wptr)++;
      ss->signal = a->signal;
      ss->name = ident_downcase(tree_ident(a->where));
      ss->owner = sh;

      tb_istr(path, ss->name);
      ss->path = ident_new(tb_get(path));
      tb_trim(path, base);

      hash_put(sh->namemap, ss->path, ss);
   }

   list_foreach(rt_scope_t *, child, scope->children) {
      ident_t name = ident_downcase(tree_ident(child->where));

      tb_istr(path, name);
      tb_append(path, '/');
      recurse_signals(sh, child, path, wptr);
      tb_trim(path, base);
   }
}

void shell_reset(tcl_shell_t *sh, tree_t top)
{
   shell_clear_model(sh);

   jit_reset(sh->jit);
   jit_enable_runtime(sh->jit, true);

   sh->top = top;

   vcode_unit_t vu = lib_get_vcode(lib_work(), top);
   if (vu != NULL)
      unit_registry_put_all(sh->registry, vu);

   shell_create_model(sh);

   sh->nsignals = count_signals(sh->root);
   sh->signals = xcalloc_array(sh->nsignals, sizeof(shell_signal_t));
   sh->namemap = hash_new(sh->nsignals * 2);

   text_buf_t *path = tb_new();
   shell_signal_t *wptr = sh->signals;
   tb_cat(path, "/");
   recurse_signals(sh, sh->root, path, &wptr);
   assert(wptr == sh->signals + sh->nsignals);
   tb_free(path);

   shell_update_now(sh);

   if (sh->handler.start_sim != NULL)
      (*sh->handler.start_sim)(tree_ident(top), sh->handler.context);
}

void shell_interact(tcl_shell_t *sh)
{
   shell_print_banner(sh);

   char *line;
   while (!sh->quit && (line = (*sh->getline)(sh))) {
      const char *result = NULL;
      if (shell_eval(sh, line, &result) && *result != '\0')
         color_printf("$+black$%s$$\n", result);

      free(line);
   }
}

bool shell_do(tcl_shell_t *sh, const char *file)
{
   const int code = Tcl_EvalFile(sh->interp, file);

   switch (code) {
   case TCL_OK:
      return true;
   case TCL_ERROR:
      {
         const char *str = Tcl_GetStringResult(sh->interp);
         if (str != NULL && *str != '\0')
            errorf("%s", str);
         return false;
      }
   default:
      warnf("Tcl_Eval returned unknown code %d", code);
      return false;
   }
}

static int shell_redirect_close(ClientData cd, Tcl_Interp *interp)
{
   return EINVAL;
}

static void shell_redirect_watch(ClientData cd, int mask)
{
}

static int shell_redirect_output(ClientData cd, const char *buf, int nchars,
                                 int *error)
{
   tcl_shell_t *sh = untag_pointer(cd, tcl_shell_t);
   if (pointer_tag(cd) == 0)
      (*sh->handler.stdout_write)(buf, nchars, sh->handler.context);
   else
      (*sh->handler.stderr_write)(buf, nchars, sh->handler.context);

   return nchars;
}

static const Tcl_ChannelType redirect_funcs = {
   .typeName = "redirect",
   .version = TCL_CHANNEL_VERSION_4,
   .closeProc = shell_redirect_close,
   .watchProc = shell_redirect_watch,
   .outputProc = shell_redirect_output,
};

void shell_set_handler(tcl_shell_t *sh, const shell_handler_t *h)
{
   sh->handler = *h;

   if (h->stdout_write != NULL) {
      Tcl_Channel chan = Tcl_CreateChannel(&redirect_funcs, "redirect0",
                                           tag_pointer(sh, 0), TCL_WRITABLE);
      Tcl_SetChannelOption(NULL, chan, "-translation", "lf");
      Tcl_SetChannelOption(NULL, chan, "-buffering", "line");
      Tcl_SetChannelOption(NULL, chan, "-encoding", "utf-8");

      Tcl_RegisterChannel(sh->interp, chan);
      Tcl_SetStdChannel(chan, TCL_STDOUT);
   }

   if (h->stderr_write != NULL) {
      Tcl_Channel chan = Tcl_CreateChannel(&redirect_funcs, "redirect1",
                                           tag_pointer(sh, 1), TCL_WRITABLE);
      Tcl_SetChannelOption(NULL, chan, "-translation", "lf");
      Tcl_SetChannelOption(NULL, chan, "-buffering", "none");
      Tcl_SetChannelOption(NULL, chan, "-encoding", "utf-8");

      Tcl_RegisterChannel(sh->interp, chan);
      Tcl_SetStdChannel(chan, TCL_STDERR);
   }
}

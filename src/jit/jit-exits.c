//
//  Copyright (C) 2022-2023  Nick Gasson
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
#include "diag.h"
#include "jit/jit-exits.h"
#include "jit/jit-ffi.h"
#include "jit/jit-priv.h"
#include "jit/jit.h"
#include "lib.h"
#include "object.h"
#include "rt/mspace.h"
#include "rt/rt.h"
#include "type.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void x_file_open(int8_t *status, void **_fp, uint8_t *name_bytes,
                 int32_t name_len, int8_t mode, tree_t where)
{
   FILE **fp = (FILE **)_fp;

   char *fname LOCAL = xmalloc(name_len + 1);
   memcpy(fname, name_bytes, name_len);
   fname[name_len] = '\0';

   const char *mode_str[] = {
      "rb", "wb", "ab"
   };
   assert(mode < ARRAY_LEN(mode_str));

   if (status != NULL)
      *status = OPEN_OK;

   if (*fp != NULL) {
      if (status == NULL)
         jit_msg(tree_loc(where), DIAG_FATAL, "file object already associated "
                 "with an external file");
      else
         *status = STATUS_ERROR;
   }
   else if (name_len == 0) {
      if (status == NULL)
         jit_msg(tree_loc(where), DIAG_FATAL, "empty file name in FILE_OPEN");
      else
         *status = NAME_ERROR;
   }
   else if (strcmp(fname, "STD_INPUT") == 0)
      *fp = stdin;
   else if (strcmp(fname, "STD_OUTPUT") == 0)
      *fp = stdout;
   else {
#ifdef __MINGW32__
      const bool failed = (fopen_s(fp, fname, mode_str[mode]) != 0);
#else
      const bool failed = ((*fp = fopen(fname, mode_str[mode])) == NULL);
#endif
      if (failed) {
         if (status == NULL)
            jit_msg(tree_loc(where), DIAG_FATAL, "failed to open %s: %s", fname,
                    strerror(errno));
         else {
            switch (errno) {
            case ENOENT: *status = NAME_ERROR; break;
            case EPERM:  *status = MODE_ERROR; break;
            default:     *status = NAME_ERROR; break;
            }
         }
      }
   }
}

void x_file_write(void **_fp, uint8_t *data, int32_t len)
{
   FILE **fp = (FILE **)_fp;

   if (*fp == NULL)
      jit_msg(NULL, DIAG_FATAL, "write to closed file");

   fwrite(data, 1, len, *fp);
}

void x_file_read(void **_fp, uint8_t *data, int32_t size, int32_t count,
                 int32_t *out)
{
   FILE **fp = (FILE **)_fp;

   if (*fp == NULL)
      jit_msg(NULL, DIAG_FATAL, "read from closed file");

   size_t n = fread(data, size, count, *fp);
   if (out != NULL)
      *out = n;
}

void x_file_close(void **_fp)
{
   FILE **fp = (FILE **)_fp;

   if (*fp != NULL) {
      fclose(*fp);
      *fp = NULL;
   }
}

int8_t x_endfile(void *_f)
{
   FILE *f = _f;

   if (f == NULL)
      jit_msg(NULL, DIAG_FATAL, "ENDFILE called on closed file");

   int c = fgetc(f);
   if (c == EOF)
      return 1;
   else {
      ungetc(c, f);
      return 0;
   }
}

void x_file_flush(void *_f)
{
   if (_f == NULL)
      jit_msg(NULL, DIAG_FATAL, "FLUSH called on closed file");

   fflush(_f);
}

void x_index_fail(int32_t value, int32_t left, int32_t right, int8_t dir,
                  tree_t where, tree_t hint)
{
   type_t type = tree_type(hint);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_cat(tb, "index ");
   to_string(tb, type, value);
   tb_printf(tb, " outside of %s range ", type_pp(type));
   to_string(tb, type, left);
   tb_cat(tb, dir == RANGE_TO ? " to " : " downto ");
   to_string(tb, type, right);

   jit_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

void x_length_fail(int32_t left, int32_t right, int32_t dim, tree_t where)
{
   const tree_kind_t kind = tree_kind(where);

   LOCAL_TEXT_BUF tb = tb_new();
   if (kind == T_PORT_DECL || kind == T_GENERIC_DECL || kind == T_PARAM_DECL)
      tb_cat(tb, "actual");
   else if (kind == T_CASE || kind == T_MATCH_CASE)
      tb_cat(tb, "expression");
   else if (kind == T_ASSOC)
      tb_cat(tb, "choice");
   else
      tb_cat(tb, "value");
   tb_printf(tb, " length %d", right);
   if (dim > 0)
      tb_printf(tb, " for dimension %d", dim);
   tb_cat(tb, " does not match ");

   switch (kind) {
   case T_PORT_DECL:
      tb_printf(tb, "port %s", istr(tree_ident(where)));
      break;
   case T_PARAM_DECL:
      tb_printf(tb, "parameter %s", istr(tree_ident(where)));
      break;
   case T_GENERIC_DECL:
      tb_printf(tb, "generic %s", istr(tree_ident(where)));
      break;
   case T_VAR_DECL:
      tb_printf(tb, "variable %s", istr(tree_ident(where)));
      break;
   case T_SIGNAL_DECL:
      tb_printf(tb, "signal %s", istr(tree_ident(where)));
      break;
   case T_REF:
      tb_printf(tb, "%s %s", class_str(class_of(where)),
                istr(tree_ident(where)));
      break;
   case T_FIELD_DECL:
      tb_printf(tb, "field %s", istr(tree_ident(where)));
      break;
   case T_ALIAS:
      tb_printf(tb, "alias %s", istr(tree_ident(where)));
      break;
   case T_CASE:
   case T_MATCH_CASE:
      tb_cat(tb, "case choice");
      break;
   case T_ASSOC:
      tb_cat(tb, "expected");
      break;
   default:
      tb_cat(tb, "target");
      break;
   }

   tb_printf(tb, " length %d", left);

   jit_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

void x_range_fail(int64_t value, int64_t left, int64_t right, int8_t dir,
                  tree_t where, tree_t hint)
{
   type_t type = tree_type(hint);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_cat(tb, "value ");
   to_string(tb, type, value);
   tb_printf(tb, " outside of %s range ", type_pp(type));
   to_string(tb, type, left);
   tb_cat(tb, dir == RANGE_TO ? " to " : " downto ");
   to_string(tb, type, right);

   switch (tree_kind(hint)) {
   case T_SIGNAL_DECL:
   case T_CONST_DECL:
   case T_VAR_DECL:
   case T_REF:
      tb_printf(tb, " for %s %s", class_str(class_of(hint)),
                istr(tree_ident(hint)));
      break;
   case T_PORT_DECL:
      tb_printf(tb, " for port %s", istr(tree_ident(hint)));
      break;
   case T_PARAM_DECL:
      tb_printf(tb, " for parameter %s", istr(tree_ident(hint)));
      break;
   case T_GENERIC_DECL:
      tb_printf(tb, " for generic %s", istr(tree_ident(hint)));
      break;
   case T_ATTR_REF:
      tb_printf(tb, " for attribute '%s", istr(tree_ident(hint)));
      break;
   default:
      break;
   }

   jit_msg(tree_loc(where), DIAG_FATAL, "%s", tb_get(tb));
}

void x_exponent_fail(int32_t value, tree_t where)
{
   jit_msg(tree_loc(where), DIAG_FATAL, "negative exponent %d only "
           "allowed for floating-point types", value);
}

void x_overflow(int64_t lhs, int64_t rhs, tree_t where)
{
   LOCAL_TEXT_BUF tb = tb_new();
   if (tree_kind(where) == T_FCALL) {
      switch (tree_subkind(tree_ref(where))) {
      case S_ADD:
         tb_printf(tb, "%"PRIi64" + %"PRIi64, lhs, rhs);
         break;
      case S_MUL:
         tb_printf(tb, "%"PRIi64" * %"PRIi64, lhs, rhs);
         break;
      case S_SUB:
         tb_printf(tb, "%"PRIi64" - %"PRIi64, lhs, rhs);
         break;
      case S_NEGATE:
         tb_printf(tb, "-(%"PRIi64")", lhs);
         break;
      case S_EXP:
         tb_printf(tb, "%"PRIi64" ** %"PRIi64, lhs, rhs);
         break;
      }
   }

   jit_msg(tree_loc(where), DIAG_FATAL,
           "result of %s cannot be represented as %s",
           tb_get(tb), type_pp(tree_type(where)));
}

void x_null_deref(tree_t where)
{
   jit_msg(tree_loc(where), DIAG_FATAL, "null access dereference");
}

void x_div_zero(tree_t where)
{
   jit_msg(tree_loc(where), DIAG_FATAL, "division by zero");
}

int64_t x_string_to_int(const uint8_t *raw_str, int32_t str_len, int32_t *used)
{
   const char *p = (const char *)raw_str;
   const char *endp = p + str_len;

   for (; p < endp && isspace((int)*p); p++)
      ;

   const bool is_negative = p < endp && *p == '-';
   if (is_negative) p++;

   int64_t value = INT64_MIN;
   int num_digits = 0;
   while (p < endp && (isdigit((int)*p) || *p == '_')) {
      if (*p != '_') {
         value *= 10;
         value += (*p - '0');
         num_digits++;
      }
      ++p;
   }

   if (is_negative) value = -value;

   if (num_digits == 0)
      jit_msg(NULL, DIAG_FATAL, "invalid integer value "
              "\"%.*s\"", str_len, (const char *)raw_str);

   if (used != NULL)
      *used = p - (const char *)raw_str;
   else {
      for (; p < endp && *p != '\0'; p++) {
         if (!isspace((int)*p)) {
            jit_msg(NULL, DIAG_FATAL, "found invalid characters \"%.*s\" after "
                    "value \"%.*s\"", (int)(endp - p), p, str_len,
                    (const char *)raw_str);
         }
      }
   }

   return value;
}

double x_string_to_real(const uint8_t *raw_str, int32_t str_len)
{
   char *null LOCAL = xmalloc(str_len + 1);
   memcpy(null, raw_str, str_len);
   null[str_len] = '\0';

   char *p = null;
   for (; p < p + str_len && isspace((int)*p); p++)
      ;

   double value = strtod(p, &p);

   if (*p != '\0' && !isspace((int)*p))
      jit_msg(NULL, DIAG_FATAL, "invalid real value "
              "\"%.*s\"", str_len, (const char *)raw_str);
   else {
      for (; p < null + str_len && *p != '\0'; p++) {
         if (!isspace((int)*p)) {
            jit_msg(NULL, DIAG_FATAL, "found invalid characters \"%.*s\" after "
                    "value \"%.*s\"", (int)(null + str_len - p), p, str_len,
                    (const char *)raw_str);
         }
      }
   }

   return value;
}

ffi_uarray_t x_canon_value(const uint8_t *raw_str, int32_t str_len, char *buf)
{
   char *p = buf;
   int pos = 0;

   for (; pos < str_len && isspace((int)raw_str[pos]); pos++)
      ;

   bool upcase = true;
   for (; pos < str_len && !isspace((int)raw_str[pos]); pos++) {
      if (raw_str[pos] == '\'')
         upcase = !upcase;

      *p++ = upcase ? toupper((int)raw_str[pos]) : raw_str[pos];
   }

   for (; pos < str_len; pos++) {
      if (!isspace((int)raw_str[pos])) {
         jit_msg(NULL, DIAG_FATAL, "found invalid characters \"%.*s\" after "
                 "value \"%.*s\"", (int)(str_len - pos), raw_str + pos, str_len,
                 (const char *)raw_str);
      }
   }

   return ffi_wrap(buf, 1, p - buf);
}

ffi_uarray_t x_int_to_string(int64_t value, char *buf, size_t max)
{
   size_t len = checked_sprintf(buf, max, "%"PRIi64, value);
   return ffi_wrap(buf, 1, len);
}

ffi_uarray_t x_real_to_string(double value, char *buf, size_t max)
{
   size_t len = checked_sprintf(buf, max, "%.*g", DBL_DIG, value);
   return ffi_wrap(buf, 1, len);
}

void x_report(const uint8_t *msg, int32_t msg_len, int8_t severity,
              tree_t where)
{
   assert(severity <= SEVERITY_FAILURE);

   static const char *levels[] = {
      "Note", "Warning", "Error", "Failure"
   };

   const diag_level_t level = diag_severity(severity);

   diag_t *d = diag_new(level, tree_loc(where));
   diag_printf(d, "Report %s: %.*s", levels[severity], msg_len, msg);
   diag_show_source(d, false);
   diag_emit(d);

   if (level == DIAG_FATAL)
      jit_abort(EXIT_FAILURE);
}

void x_assert_fail(const uint8_t *msg, int32_t msg_len, int8_t severity,
                   int64_t hint_left, int64_t hint_right, int8_t hint_valid,
                   tree_t where)
{
   // LRM 93 section 8.2
   // The error message consists of at least
   // a) An indication that this message is from an assertion
   // b) The value of the severity level
   // c) The value of the message string
   // d) The name of the design unit containing the assertion

   assert(severity <= SEVERITY_FAILURE);

   static const char *levels[] = {
      "Note", "Warning", "Error", "Failure"
   };

   const diag_level_t level = diag_severity(severity);

   diag_t *d = diag_new(level, tree_loc(where));
   if (msg == NULL)
      diag_printf(d, "Assertion %s: Assertion violation.", levels[severity]);
   else {
      diag_printf(d, "Assertion %s: %.*s", levels[severity], msg_len, msg);

      // Assume we don't want to dump the source code if the user
      // provided their own message
      diag_show_source(d, false);
   }

   if (hint_valid) {
      assert(tree_kind(where) == T_FCALL);
      type_t p0_type = tree_type(tree_value(tree_param(where, 0)));
      type_t p1_type = tree_type(tree_value(tree_param(where, 1)));

      LOCAL_TEXT_BUF tb = tb_new();
      to_string(tb, p0_type, hint_left);
      switch (tree_subkind(tree_ref(where))) {
      case S_SCALAR_EQ:  tb_cat(tb, " = "); break;
      case S_SCALAR_NEQ: tb_cat(tb, " /= "); break;
      case S_SCALAR_LT:  tb_cat(tb, " < "); break;
      case S_SCALAR_GT:  tb_cat(tb, " > "); break;
      case S_SCALAR_LE:  tb_cat(tb, " <= "); break;
      case S_SCALAR_GE:  tb_cat(tb, " >= "); break;
      default: tb_cat(tb, " <?> "); break;
      }
      to_string(tb, p1_type, hint_right);
      tb_cat(tb, " is false");

      diag_hint(d, tree_loc(where), "%s", tb_get(tb));
   }

   diag_emit(d);

   if (level == DIAG_FATAL)
      jit_abort(EXIT_FAILURE);
}

void *x_mspace_alloc(size_t size)
{
   if (unlikely(size > UINT32_MAX)) {
      jit_msg(NULL, DIAG_FATAL, "attempting to allocate %zu byte object "
              "which is larger than the maximum supported %u bytes",
              size, UINT32_MAX);
      __builtin_unreachable();
   }
   else
      return jit_mspace_alloc(size);
}

void x_elab_order_fail(tree_t where)
{
   assert(tree_kind(where) == T_EXTERNAL_NAME);

   jit_msg(tree_loc(where), DIAG_FATAL, "%s %s has not yet been elaborated",
           class_str(tree_class(where)), istr(tree_ident(tree_ref(where))));
}

void x_unreachable(tree_t where)
{
   if (where != NULL && tree_kind(where) == T_FUNC_BODY)
      jit_msg(tree_loc(where), DIAG_FATAL, "function %s did not return a value",
              istr(tree_ident(where)));
   else
      jit_msg(NULL, DIAG_FATAL, "executed unreachable instruction");
}

void x_func_wait(void)
{
   jit_msg(NULL, DIAG_FATAL, "cannot wait inside function call");
}

////////////////////////////////////////////////////////////////////////////////
// Entry point from interpreter or JIT compiled code

DLLEXPORT
void __nvc_sched_waveform(jit_anchor_t *anchor, jit_scalar_t *args,
                          tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_thread_local();
   thread->anchor = anchor;

   sig_shared_t *shared = args[0].pointer;
   int32_t       offset = args[1].integer;
   int32_t       count  = args[2].integer;
   jit_scalar_t  value  = { .integer = args[3].integer };
   int64_t       after  = args[4].integer;
   int64_t       reject = args[5].integer;
   bool          scalar = args[6].integer;

   if (scalar)
      x_sched_waveform_s(shared, offset, value.integer, after, reject);
   else
      x_sched_waveform(shared, offset, value.pointer, count,
                       after, reject);

   thread->anchor = NULL;
}

DLLEXPORT
void __nvc_test_event(jit_anchor_t *anchor, jit_scalar_t *args, tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_thread_local();
   thread->anchor = anchor;

   sig_shared_t *shared = args[0].pointer;
   int32_t       offset = args[1].integer;
   int32_t       count  = args[2].integer;

   args[0].integer = x_test_net_event(shared, offset, count);

   thread->anchor = NULL;
}

DLLEXPORT
void __nvc_do_exit(jit_exit_t which, jit_anchor_t *anchor, jit_scalar_t *args,
                   tlab_t *tlab)
{
   jit_thread_local_t *thread = jit_thread_local();
   thread->anchor = anchor;

   switch (which) {
   case JIT_EXIT_ASSERT_FAIL:
      {
         uint8_t *msg        = args[0].pointer;
         int32_t  len        = args[1].integer;
         int32_t  severity   = args[2].integer;
         int64_t  hint_left  = args[3].integer;
         int64_t  hint_right = args[4].integer;
         int8_t   hint_valid = args[5].integer;
         tree_t   where      = args[6].pointer;

         x_assert_fail(msg, len, severity, hint_left, hint_right,
                       hint_valid, where);
      }
      break;

   case JIT_EXIT_REPORT:
      {
         uint8_t *msg      = args[0].pointer;
         int32_t  len      = args[1].integer;
         int32_t  severity = args[2].integer;
         tree_t   where    = args[3].pointer;

         x_report(msg, len, severity, where);
      }
      break;

   case JIT_EXIT_INIT_SIGNAL:
      {
         int32_t      count  = args[0].integer;
         int32_t      size   = args[1].integer;
         jit_scalar_t value  = { .integer = args[2].integer };
         int32_t      flags  = args[3].integer;
         tree_t       where  = args[4].pointer;
         int32_t      offset = args[5].integer;
         bool         scalar = args[6].integer;

         sig_shared_t *ss;
         if (!jit_has_runtime(thread->jit))
            ss = NULL;   // Called during constant folding
         else if (scalar)
            ss = x_init_signal_s(count, size, value.integer,
                                 flags, where, offset);
         else
            ss = x_init_signal(count, size, value.pointer,
                               flags, where, offset);

         args[0].pointer = ss;
      }
      break;

   case JIT_EXIT_IMPLICIT_SIGNAL:
      {
         int32_t       count   = args[0].integer;
         int32_t       size    = args[1].integer;
         tree_t        where   = args[2].pointer;
         int32_t       kind    = args[3].integer;
         jit_handle_t  handle  = args[4].integer;
         void         *context = args[5].pointer;

         sig_shared_t *ss;
         if (!jit_has_runtime(thread->jit))
            ss = NULL;   // Called during constant folding
         else {
            ffi_closure_t closure = { handle, context };
            ss = x_implicit_signal(count, size, where, kind, &closure);
         }

         args[0].pointer = ss;
      }
      break;

   case JIT_EXIT_RESOLVE_SIGNAL:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *shared  = args[0].pointer;
         jit_handle_t  handle  = args[1].integer;
         void         *context = args[2].pointer;
         int32_t       ileft   = args[3].integer;
         int32_t       nlits   = args[4].integer;
         int32_t       flags   = args[5].integer;

         x_resolve_signal(shared, handle, context, ileft, nlits, flags);
      }
      break;

   case JIT_EXIT_DRIVE_SIGNAL:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // Called during constant folding

         sig_shared_t *ss     = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         x_drive_signal(ss, offset, count);
      }
      break;

   case JIT_EXIT_MAP_SIGNAL:
      {
         sig_shared_t  *src_ss     = args[0].pointer;
         uint32_t       src_offset = args[1].integer;
         sig_shared_t  *dst_ss     = args[2].pointer;
         uint32_t       dst_offset = args[3].integer;
         uint32_t       src_count  = args[4].integer;
         uint32_t       dst_count  = args[5].integer;
         jit_handle_t   handle     = args[6].integer;
         void          *context    = args[7].pointer;

         if (handle != JIT_HANDLE_INVALID) {
            ffi_closure_t closure = { handle, context };
            x_map_signal(src_ss, src_offset, dst_ss, dst_offset, src_count,
                         dst_count, &closure);
         }
         else
            x_map_signal(src_ss, src_offset, dst_ss, dst_offset, src_count,
                         dst_count, NULL);
      }
      break;

   case JIT_EXIT_MAP_CONST:
      {
         sig_shared_t *dst_ss     = args[0].pointer;
         uint32_t      dst_offset = args[1].integer;
         jit_scalar_t  initval    = { .integer = args[2].integer };
         uint32_t      dst_count  = args[3].integer;
         bool          scalar     = args[4].integer;

         const void *vptr = scalar ? &initval.integer : initval.pointer;

         x_map_const(dst_ss, dst_offset, vptr, dst_count);
      }
      break;

   case JIT_EXIT_SCHED_PROCESS:
      {
         if (!jit_has_runtime(thread->jit))
            return;   // TODO: this should not be necessary

         int64_t after = args[0].integer;
         x_sched_process(after);
      }
      break;

   case JIT_EXIT_SCHED_WAVEFORM:
      __nvc_sched_waveform(anchor, args, tlab);
      break;

   case JIT_EXIT_SCHED_EVENT:
      {
         sig_shared_t *shared  = args[0].pointer;
         int32_t       offset  = args[1].integer;
         int32_t       count   = args[2].integer;
         sig_shared_t *wake    = args[3].pointer;

         x_sched_event(shared, offset, count, wake);
      }
      break;

   case JIT_EXIT_INT_TO_STRING:
      {
         int64_t value = args[0].integer;

         char *buf = jit_mspace_alloc(28);

         ffi_uarray_t u = x_int_to_string(value, buf, 28);
         args[0].pointer = u.ptr;
         args[1].integer = u.dims[0].left;
         args[2].integer = u.dims[0].length;
      }
      break;

   case JIT_EXIT_ALIAS_SIGNAL:
      {
         sig_shared_t *ss    = args[0].pointer;
         tree_t        where = args[1].pointer;

         x_alias_signal(ss, where);
      }
      break;

   case JIT_EXIT_REAL_TO_STRING:
      {
         double value = args[0].real;

         char *buf = jit_mspace_alloc(32);

         ffi_uarray_t u = x_real_to_string(value, buf, 32);
         args[0].pointer = u.ptr;
         args[1].integer = u.dims[0].left;
         args[2].integer = u.dims[0].length;
      }
      break;

   case JIT_EXIT_DISCONNECT:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;
         int64_t       reject = args[3].integer;
         int64_t       after  = args[4].integer;

         x_disconnect(shared, offset, count, after, reject);
      }
      break;

   case JIT_EXIT_ELAB_ORDER_FAIL:
      {
         tree_t where = args[0].pointer;
         x_elab_order_fail(where);
      }
      break;

   case JIT_EXIT_UNREACHABLE:
      {
         tree_t where = args[0].pointer;
         x_unreachable(where);
      }
      break;

   case JIT_EXIT_OVERFLOW:
      {
         int32_t lhs   = args[0].integer;
         int32_t rhs   = args[1].integer;
         tree_t  where = args[2].pointer;

         x_overflow(lhs, rhs, where);
      }
      break;

   case JIT_EXIT_INDEX_FAIL:
      {
         int32_t      value = args[0].integer;
         int32_t      left  = args[1].integer;
         int32_t      right = args[2].integer;
         range_kind_t dir   = args[3].integer;
         tree_t       where = args[4].pointer;
         tree_t       hint  = args[5].pointer;

         x_index_fail(value, left, right, dir, where, hint);
      }
      break;

   case JIT_EXIT_RANGE_FAIL:
      {
         int64_t      value = args[0].integer;
         int64_t      left  = args[1].integer;
         int64_t      right = args[2].integer;
         range_kind_t dir   = args[3].integer;
         tree_t       where = args[4].pointer;
         tree_t       hint  = args[5].pointer;

         x_range_fail(value, left, right, dir, where, hint);
      }
      break;

   case JIT_EXIT_FORCE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;
         jit_scalar_t  value  = { .integer = args[3].integer };
         bool          scalar = args[4].integer;

         if (scalar)
            x_force(shared, offset, count, &value.integer);
         else
            x_force(shared, offset, count, value.pointer);
      }
      break;

   case JIT_EXIT_RELEASE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         x_release(shared, offset, count);
      }
      break;

   case JIT_EXIT_PUSH_SCOPE:
      {
         if (!jit_has_runtime(jit_thread_local()->jit))
            return;   // Called during constant folding

         tree_t  where = args[0].pointer;
         int32_t size  = args[1].integer;

         x_push_scope(where, size);
      }
      break;

   case JIT_EXIT_POP_SCOPE:
      {
         if (!jit_has_runtime(jit_thread_local()->jit))
            return;   // Called during constant folding

         x_pop_scope();
      }
      break;

   case JIT_EXIT_FUNC_WAIT:
      {
         x_func_wait();
      }
      break;

   case JIT_EXIT_CANON_VALUE:
      {
         uint8_t *ptr = args[0].pointer;
         int32_t  len = args[1].integer;

         char *buf = jit_mspace_alloc(len);

         ffi_uarray_t u = x_canon_value(ptr, len, buf);
         args[0].pointer = u.ptr;
         args[1].integer = u.dims[0].left;
         args[2].integer = u.dims[0].length;
      }
      break;

   case JIT_EXIT_STRING_TO_INT:
      {
         uint8_t *ptr  = args[0].pointer;
         int32_t  len  = args[1].integer;
         int32_t *used = args[2].pointer;

         args[0].integer = x_string_to_int(ptr, len, used);
      }
      break;

   case JIT_EXIT_STRING_TO_REAL:
      {
         uint8_t *ptr = args[0].pointer;
         int32_t  len = args[1].integer;

         args[0].real = x_string_to_real(ptr, len);
      }
      break;

   case JIT_EXIT_DIV_ZERO:
      {
         tree_t where = args[0].pointer;
         x_div_zero(where);
      }
      break;

   case JIT_EXIT_LENGTH_FAIL:
      {
         int32_t left  = args[0].integer;
         int32_t right = args[1].integer;
         int32_t dim   = args[2].integer;
         tree_t  where = args[3].pointer;

         x_length_fail(left, right, dim, where);
      }
      break;

   case JIT_EXIT_NULL_DEREF:
      {
         tree_t where = args[0].pointer;
         x_null_deref(where);
      }
      break;

   case JIT_EXIT_EXPONENT_FAIL:
      {
         int32_t value = args[0].integer;
         tree_t  where = args[1].pointer;

         x_exponent_fail(value, where);
      }
      break;

   case JIT_EXIT_FILE_OPEN:
      {
         int8_t   *status     = args[0].pointer;
         void    **_fp        = args[1].pointer;
         uint8_t  *name_bytes = args[2].pointer;
         int32_t   name_len   = args[3].integer;
         int32_t   mode       = args[4].integer;
         tree_t    where      = args[5].pointer;

         x_file_open(status, _fp, name_bytes, name_len, mode, where);
      }
      break;

   case JIT_EXIT_FILE_CLOSE:
      {
         void **_fp = args[0].pointer;
         x_file_close(_fp);
      }
      break;

   case JIT_EXIT_FILE_READ:
      {
         void    **_fp   = args[0].pointer;
         uint8_t  *data  = args[1].pointer;
         int32_t   size  = args[2].integer;
         int32_t   count = args[3].integer;
         int32_t  *out   = args[4].pointer;

         x_file_read(_fp, data, size, count, out);
      }
      break;

   case JIT_EXIT_FILE_WRITE:
      {
         void    **_fp  = args[0].pointer;
         uint8_t  *data = args[1].pointer;
         int32_t   len  = args[2].integer;

         x_file_write(_fp, data, len);
      }
      break;

   case JIT_EXIT_ENDFILE:
      {
         void *_fp = args[0].pointer;
         args[0].integer = x_endfile(_fp);
      }
      break;

   case JIT_EXIT_DEBUG_OUT:
      {
         int64_t value = args[0].integer;
         debugf("DEBUG %"PRIi64, value);
      }
      break;

   case JIT_EXIT_LAST_EVENT:
      {
         sig_shared_t *shared = args[0].pointer;
         uint32_t      offset = args[1].integer;
         uint32_t      count  = args[2].integer;

         args[0].integer = x_last_event(shared, offset, count);
      }
      break;

   case JIT_EXIT_LAST_ACTIVE:
      {
         sig_shared_t *shared = args[0].pointer;
         uint32_t      offset = args[1].integer;
         uint32_t      count  = args[2].integer;

         args[0].integer = x_last_active(shared, offset, count);
      }
      break;

   case JIT_EXIT_TEST_EVENT:
      __nvc_test_event(anchor, args, tlab);
      break;

   case JIT_EXIT_TEST_ACTIVE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         args[0].integer = x_test_net_active(shared, offset, count);
      }
      break;

   case JIT_EXIT_DRIVING:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         args[0].integer = x_driving(shared, offset, count);
      }
      break;

   case JIT_EXIT_DRIVING_VALUE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         args[0].pointer = x_driving_value(shared, offset, count);
      }
      break;

   case JIT_EXIT_CLAIM_TLAB:
      {
         x_claim_tlab(tlab);
      }
      break;

   case JIT_EXIT_COVER_TOGGLE:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t      *mem    = args[1].pointer;

         x_cover_setup_toggle_cb(shared, mem);
      }
      break;

   case JIT_EXIT_PROCESS_INIT:
      {
         jit_handle_t handle = args[0].integer;
         tree_t       where  = args[1].pointer;

         x_process_init(handle, where);
      }
      break;

   case JIT_EXIT_CLEAR_EVENT:
      {
         sig_shared_t *shared = args[0].pointer;
         int32_t       offset = args[1].integer;
         int32_t       count  = args[2].integer;

         x_clear_event(shared, offset, count);
      }
      break;

   default:
      fatal_trace("unhandled exit %s", jit_exit_name(which));
   }

   thread->anchor = NULL;
}

DLLEXPORT
void __nvc_do_fficall(jit_foreign_t *ff, jit_anchor_t *anchor,
                      jit_scalar_t *args)
{
   jit_thread_local_t *thread = jit_thread_local();
   thread->anchor = anchor;

   jit_ffi_call(ff, args);

   thread->anchor = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Entry points from AOT compiled code

DLLEXPORT
void __nvc_flush(FILE *f)
{
   x_file_flush(f);
}

DLLEXPORT
void _debug_out(intptr_t val, int32_t reg)
{
   printf("DEBUG: r%d val=%"PRIxPTR"\n", reg, val);
   fflush(stdout);
}

DLLEXPORT
void _debug_dump(const uint8_t *ptr, int32_t len)
{
   printf("---- %p ----\n", ptr);

   if (ptr != NULL) {
      for (int i = 0; i < len; i++)
         printf("%02x%c", ptr[i], (i % 8 == 7) ? '\n' : ' ');
      if (len % 8 != 0)
         printf("\n");
   }

   fflush(stdout);
}

DLLEXPORT
void *__nvc_mspace_alloc(uintptr_t size, jit_anchor_t *anchor)
{
   jit_thread_local_t *thread = jit_thread_local();
   thread->anchor = anchor;

   void *ptr = x_mspace_alloc(size);

   thread->anchor = NULL;
   return ptr;
}

DLLEXPORT
void __nvc_putpriv(jit_handle_t handle, void *data)
{
   jit_t *j = jit_thread_local()->jit;
   jit_func_t *f = jit_get_func(j, handle);

   store_release(jit_get_privdata_ptr(j, f), data);
}

DLLEXPORT
object_t *__nvc_get_object(const char *unit, ptrdiff_t offset)
{
   return object_from_locus(ident_new(unit), offset,
                            (object_load_fn_t)lib_get_qualified);
}

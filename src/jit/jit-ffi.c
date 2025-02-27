//
//  Copyright (C) 2021-2023  Nick Gasson
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
#include "hash.h"
#include "ident.h"
#include "jit/jit-ffi.h"
#include "jit/jit.h"
#include "option.h"
#include "thread.h"

#include <assert.h>
#include <ctype.h>
#include <ffi.h>
#include <stdlib.h>
#include <string.h>

#ifdef __MINGW32__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct _jit_foreign {
   ffi_cif     cif;
   void       *ptr;
   ident_t     sym;
   ffi_spec_t  spec;
   int         nargs;
   ffi_type   *args[];
} jit_foreign_t;

typedef struct _jit_dll {
   jit_dll_t *next;
   char      *path;
#ifdef __MINGW32__
   HMODULE    handle;
#else
   void      *handle;
#endif
} jit_dll_t;

static hash_t     *cache;
static jit_dll_t  *dlls;
static nvc_lock_t  lock;

jit_foreign_t *jit_ffi_get(ident_t sym)
{
   SCOPED_LOCK(lock);

   if (cache == NULL)
      cache = hash_new(128);

   return hash_get(cache, sym);
}

static ffi_type *libffi_type_for(ffi_type_t type)
{
   switch (type) {
   case FFI_INT8:    return &ffi_type_sint8;
   case FFI_INT16:   return &ffi_type_sint16;
   case FFI_INT32:   return &ffi_type_sint32;
   case FFI_INT64:   return &ffi_type_sint64;
   case FFI_FLOAT:   return &ffi_type_double;
   case FFI_POINTER: return &ffi_type_pointer;
   case FFI_UARRAY:
   case FFI_VOID:
   default:          return &ffi_type_void;
   }
}

jit_foreign_t *jit_ffi_bind(ident_t sym, ffi_spec_t spec, void *ptr)
{
   SCOPED_LOCK(lock);

   if (cache == NULL)
      cache = hash_new(128);
   else {
      jit_foreign_t *exist = hash_get(cache, sym);
      if (exist != NULL)
         return exist;
   }

   ffi_spec_t copy = spec;
   if (spec.count == 0)
      copy.ext = xstrdup(spec.ext);

   int nargs = 0;
   for (int i = 1; ffi_spec_has(spec, i); i++) {
      const ffi_type_t type = ffi_spec_get(spec, i);
      nargs += (type == FFI_UARRAY) ? 3 : 1;
   }

   const ffi_type_t rtype = ffi_spec_get(spec, 0);
   const int adj_nargs = (rtype == FFI_UARRAY) ? nargs + 1 : nargs;

   jit_foreign_t *ff = xcalloc_flex(sizeof(jit_foreign_t),
                                    adj_nargs, sizeof(ffi_type *));
   ff->ptr   = ptr;
   ff->sym   = sym;
   ff->spec  = copy;
   ff->nargs = nargs;

   int wptr = 0;
   for (int i = 1; ffi_spec_has(spec, i); i++) {
      const ffi_type_t type = ffi_spec_get(spec, i);
      if (type == FFI_UARRAY) {
         ff->args[wptr++] = &ffi_type_pointer;
         ff->args[wptr++] = &ffi_type_sint32;   // Left
         ff->args[wptr++] = &ffi_type_sint32;   // Length
      }
      else
         ff->args[wptr++] = libffi_type_for(type);
   }
   if (rtype == FFI_UARRAY)
      ff->args[wptr++] = &ffi_type_pointer;
   assert(wptr == adj_nargs);

   ffi_type *ret = libffi_type_for(rtype);

   if (ffi_prep_cif(&ff->cif, FFI_DEFAULT_ABI, adj_nargs,
                    ret, ff->args) != FFI_OK)
      fatal("ffi_prep_cif failed for %s", istr(sym));

   hash_put(cache, sym, ff);
   return ff;
}

void jit_ffi_call(jit_foreign_t *ff, jit_scalar_t *args)
{
   void *aptrs[ff->nargs + 1];
   for (int i = 0; i < ff->nargs; i++)
      aptrs[i] = &(args[i].integer);

   const ffi_type_t rtype = ffi_spec_get(ff->spec, 0);

   ffi_uarray_t u, *up = &u;
   if (rtype == FFI_UARRAY)
      aptrs[ff->nargs] = &up;

   if (ff->ptr == NULL) {
      LOCAL_TEXT_BUF tb = tb_new();
      tb_istr(tb, ff->sym);

      if ((ff->ptr = ffi_find_symbol(NULL, tb_get(tb))) == NULL)
         jit_msg(NULL, DIAG_FATAL, "foreign function %s not found", tb_get(tb));
   }

   intmax_t result;
   ffi_call(&ff->cif, ff->ptr, &result, aptrs);

   if (rtype == FFI_UARRAY) {
      args[0].pointer = u.ptr;
      args[1].integer = u.dims[0].left;
      args[2].integer = u.dims[0].length;
   }
   else if (ffi_is_integral(rtype))
      args[0].integer = ffi_widen_int(rtype, &result);
   else
      args[0].integer = result;
}

ffi_uarray_t ffi_wrap(void *ptr, int64_t left, int64_t right)
{
   // Assumes ascending range
   const int64_t biased = 1 + (right < left ? 0 : right - left + 1);

   ffi_uarray_t u = {
      .ptr = ptr,
      .dims = { [0] = { .left = left, .length = biased } }
   };
   return u;
}

bool ffi_is_integral(ffi_type_t type)
{
   return type == FFI_INT8 || type == FFI_INT16 || type == FFI_INT32
      || type == FFI_INT64;
}

int64_t ffi_widen_int(ffi_type_t type, const void *input)
{
   switch (type) {
   case FFI_INT8: return *((int8_t *)input);
   case FFI_INT16: return *((int16_t *)input);
   case FFI_INT32: return *((int32_t *)input);
   case FFI_INT64: return *((int64_t *)input);
   default:
      fatal_trace("invalid integer type in ffi_widen_int");
   }
}

void ffi_store_int(ffi_type_t type, uint64_t value, void *output)
{
   switch (type) {
   case FFI_INT8: *(uint8_t *)output = (uint8_t)value; break;
   case FFI_INT16: *(uint16_t *)output = (uint16_t)value; break;
   case FFI_INT32: *(uint32_t *)output = (uint32_t)value; break;
   case FFI_INT64: *(uint64_t *)output = value; break;
   default:
      fatal_trace("invalid integer type in ffi_store_int");
   }
}

static jit_dll_t *ffi_load_exe(void)
{
   if (dlls != NULL)
      return dlls;  // First entry is always the executable

#ifdef __MINGW32__
   HMODULE exe_handle = GetModuleHandle(NULL);
   if (exe_handle == NULL)
      fatal("failed to get handle to main process");
#else
   void *exe_handle = dlopen(NULL, RTLD_NOW);
   if (exe_handle == NULL)
      fatal("%s", dlerror());
#endif

   LOCAL_TEXT_BUF tb = tb_new();
   get_exe_path(tb);

   jit_dll_t *dll = xcalloc(sizeof(jit_dll_t));
   dll->next   = dlls;
   dll->handle = exe_handle;
   dll->path   = tb_claim(tb);

   return (dlls = dll);
}

jit_dll_t *ffi_load_dll(const char *path)
{
   if (path == NULL)
      return ffi_load_exe();

   char *abs = realpath(path, NULL);
   if (abs == NULL)
      fatal_errno("%s", path);

   for (jit_dll_t *it = dlls; it; it = it->next) {
      if (strcmp(it->path, abs) == 0) {
         free(abs);
         return it;
      }
   }

   if (opt_get_verbose(OPT_JIT_VERBOSE, NULL))
      debugf("loading shared library %s", path);

#ifdef __MINGW32__
   HMODULE handle = LoadLibrary(path);
   if (handle == NULL)
      fatal("failed to load %s", path);
#else
   void *handle = dlopen(path, RTLD_LAZY | RTLD_GLOBAL /* XXXX */);
   if (handle == NULL)
      fatal("%s", dlerror());
#endif

   if (dlls == NULL)
      ffi_load_exe();  // First time initialisation

   jit_dll_t *dll = xcalloc(sizeof(jit_dll_t));
   dll->next   = dlls;
   dll->handle = handle;
   dll->path   = abs;

   return (dlls = dll);
}

void ffi_unload_dll(jit_dll_t *dll)
{
#ifdef __MINGW32__
   FreeLibrary(dll->handle);
#else
   dlclose(dll->handle);
#endif

   free(dll->path);

   jit_dll_t **prev;
   for (prev = &dlls; *prev != dll; prev = &((*prev)->next))
      ;

   *prev = dll->next;
   free(dll);
}

void *ffi_find_symbol(jit_dll_t *dll, const char *name)
{
   if (dll == NULL) {
      for (jit_dll_t *it = dlls; it; it = it->next) {
         void *p = ffi_find_symbol(it, name);
         if (p != NULL)
            return p;
      }

      return NULL;
   }
   else {
#ifdef __MINGW32__
      return (void *)(uintptr_t)GetProcAddress(dll->handle, name);
#else
      return dlsym(dll->handle, name);
#endif
   }
}

ident_t ffi_get_sym(jit_foreign_t *ff)
{
   return ff->sym;
}

ffi_spec_t ffi_get_spec(jit_foreign_t *ff)
{
   return ff->spec;
}

ffi_spec_t ffi_spec_new(const ffi_type_t *types, size_t count)
{
   assert(count > 0);

#ifdef DEBUG
   for (int i = 0; i < count; i++)
      assert(islower(types[i]));
#endif

   ffi_spec_t spec = {};
   if (count < 7) {
      memcpy(spec.embed, types, count);
      spec.count = count;
   }
   else
      spec.ext = types;

   return spec;
}

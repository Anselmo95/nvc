//
//  Copyright (C) 2011-2013  Nick Gasson
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
#include "rt.h"
#include "tree.h"
#include "common.h"

#include <time.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#define MAX_VAR_WIDTH  256
#define MAX_TEXT_WIDTH 512

static FILE    *vcd_file = NULL;
static ident_t i_vcd_key = NULL;
static ident_t i_fmt_fn  = NULL;
static ident_t i_fmt_arg = NULL;
static tree_t  vcd_top = NULL;

typedef int (*vcd_fmt_fn_t)(char *buf, size_t max, uint64_t val, void *arg);

static const char *vcd_key_fmt(int key)
{
   static char buf[64];

   char *p = buf;
   do {
      *p++ = 33 + (key % (126 - 33));
      key /= (126 - 33);
   } while (key > 0);
   *p = '\0';

   return buf;
}

static int vcd_fmt_enum(char *buf, size_t max, uint64_t val, void *arg)
{
   return snprintf(buf, max, "%c", ((const char*)arg)[val]);
}

static bool vcd_set_fmt_fn(tree_t decl)
{
   type_t type = tree_type(decl);

   if (type_is_array(type))
      type = type_elem(type);

   type = type_base_recur(type);

   vcd_fmt_fn_t fn = NULL;
   void *arg = NULL;

   switch (type_kind(type)) {
   case T_ENUM:
      {
         ident_t i = type_ident(type);
         if (icmp(i, "STD.STANDARD.BIT"))
            arg = "01xz";
         else if (icmp(i, "IEEE.STD_LOGIC_1164.STD_ULOGIC"))
            arg = "xx01zx01x";

         if (arg != NULL)
            fn = vcd_fmt_enum;
      }
      break;

   default:
      break;
   }

   if (fn == NULL) {
      warnf("cannot format type %s in VCD", istr(type_ident(type)));
      return false;
   }
   else {
      tree_add_attr_ptr(decl, i_fmt_fn, fn);
      tree_add_attr_ptr(decl, i_fmt_arg, arg);
      return true;
   }
}

static const char *vcd_value_fmt(tree_t decl, watch_t *watch)
{
   static char buf[MAX_TEXT_WIDTH];

   vcd_fmt_fn_t fn = tree_attr_ptr(decl, i_fmt_fn);
   void *arg = tree_attr_ptr(decl, i_fmt_arg);

   uint64_t vals[MAX_VAR_WIDTH];
   int w = rt_signal_value(watch, vals, MAX_VAR_WIDTH, false);
   type_t type = tree_type(decl);
   if (type_is_array(type)) {
      char *p = buf;
      const char *end = buf + MAX_TEXT_WIDTH;
      p += snprintf(p, end - p, "b");

      if (type_dim(type, 0).kind == RANGE_DOWNTO) {
         for (int i = w - 1; i >= 0; i--)
            p += (*fn)(p, end - p, vals[i], arg);
      }
      else {
         for (int i = 0; i < w; i++)
            p += (*fn)(p, end - p, vals[i], arg);
      }

      p += snprintf(p, end - p, " ");
   }
   else
      (*fn)(buf, MAX_TEXT_WIDTH, vals[0], arg);

   return buf;
}

static void emit_value(tree_t decl, watch_t *w)
{
   int key = tree_attr_int(decl, i_vcd_key, -1);
   fprintf(vcd_file, "%s%s\n", vcd_value_fmt(decl, w), vcd_key_fmt(key));
}

static void vcd_event_cb(uint64_t now, tree_t decl, watch_t *w, void *user)
{
   static uint64_t last_time = UINT64_MAX;

   if (now != last_time) {
      fprintf(vcd_file, "#%"PRIu64"\n", now);
      last_time = now;
   }

   emit_value(decl, w);
}

static void vcd_emit_header(void)
{
   rewind(vcd_file);

   char tmbuf[64];
   time_t t = time(NULL);
   struct tm *tm = localtime(&t);
   strftime(tmbuf, sizeof(tmbuf), "%a, %d %b %Y %T %z", tm);
   fprintf(vcd_file, "$date\n  %s\n$end\n", tmbuf);

   fprintf(vcd_file, "$version\n  "PACKAGE_STRING"\n$end\n");
   fprintf(vcd_file, "$timescale\n  1 fs\n$end\n");
}

static void vcd_process_signal(tree_t d, int *next_key)
{
   if (!vcd_set_fmt_fn(d))
      return;

   tree_add_attr_int(d, i_vcd_key, *next_key);

   type_t type = tree_type(d);
   int w = 1;
   if (type_is_array(type)) {
      int64_t low, high;
      range_bounds(type_dim(type, 0), &low, &high);
      w = high - low + 1;
   }

   const char *name = strrchr(istr(tree_ident(d)), ':') + 1;
   fprintf(vcd_file, "$var reg %d %s %s $end\n",
           w, vcd_key_fmt(*next_key), name);

   ++(*next_key);
}

void vcd_restart(void)
{
   if (vcd_file == NULL)
      return;

   vcd_emit_header();

   int next_key = 0;
   const int ndecls = tree_decls(vcd_top);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(vcd_top, i);
      switch (tree_kind(d)) {
      case T_HIER:
         fprintf(vcd_file, "$scope module %s $end\n", istr(tree_ident(d)));
         break;
      case T_SIGNAL_DECL:
         vcd_process_signal(d, &next_key);
         break;
      default:
         break;
      }

      int npop = tree_attr_int(d, ident_new("scope_pop"), 0);
      while (npop-- > 0)
         fprintf(vcd_file, "$upscope $end\n");
   }

   fprintf(vcd_file, "$enddefinitions $end\n");

   fprintf(vcd_file, "$dumpvars\n");

   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(vcd_top, i);
      if (tree_kind(d) != T_SIGNAL_DECL)
         continue;

      if (tree_attr_int(d, i_vcd_key, -1) == -1)
         continue;

      watch_t *w = rt_set_event_cb(d, vcd_event_cb, NULL);
      emit_value(d, w);
   }

   fprintf(vcd_file, "$end\n");
}

void vcd_init(const char *filename, tree_t top)
{
   i_vcd_key = ident_new("vcd_key");
   i_fmt_fn  = ident_new("fmt_fn");
   i_fmt_arg = ident_new("fmt_arg");

   vcd_top = top;

   warnf("Use of the VCD file format is discouraged as it cannot fully "
         "represent many VHDL types and the performance is poor for large "
         "designs. If you are using GtkWave the --wave option will generate "
         "an LXT file that overcomes these limitations.");

   vcd_file = fopen(filename, "w");
   if (vcd_file == NULL)
      fatal_errno("failed to open VCD output %s", filename);
}

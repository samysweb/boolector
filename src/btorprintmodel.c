/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2014-2016 Mathias Preiner.
 *  Copyright (C) 2014-2017 Aina Niemetz.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorprintmodel.h"
#include "btormodel.h"
#include "dumper/btordumpsmt.h"
#include "utils/btorutil.h"

/*------------------------------------------------------------------------*/
/* print model                                                            */
/*------------------------------------------------------------------------*/

static void
print_fmt_bv_model_btor (Btor *btor,
                         uint32_t base,
                         const BtorBitVector *assignment,
                         FILE *file)
{
  assert (btor);
  assert (assignment);
  assert (file);

  char *ass;

  if (base == BTOR_OUTPUT_BASE_HEX)
    ass = btor_bv_to_hex_char (btor->mm, assignment);
  else if (base == BTOR_OUTPUT_BASE_DEC)
    ass = btor_bv_to_dec_char (btor->mm, assignment);
  else
    ass = btor_bv_to_char (btor->mm, assignment);
  fprintf (file, "%s", ass);
  btor_mem_freestr (btor->mm, ass);
}

static void
print_fmt_bv_model_tuple_btor (Btor *btor,
                               uint32_t base,
                               const BtorBitVectorTuple *assignments,
                               FILE *file)
{
  assert (btor);
  assert (assignments);
  assert (file);

  uint32_t i;

  if (assignments->arity > 1)
  {
    for (i = 0; i < assignments->arity; i++)
    {
      if (i > 0) fprintf (file, " ");
      print_fmt_bv_model_btor (btor, base, assignments->bv[i], file);
    }
  }
  else
    print_fmt_bv_model_btor (btor, base, assignments->bv[0], file);
}

/*------------------------------------------------------------------------*/

static void
print_bv_model (
    Btor *btor, BtorNode *node, char *format, uint32_t base, FILE *file)
{
  assert (btor);
  assert (format);
  assert (node);
  assert (BTOR_IS_REGULAR_NODE (node));

  int64_t id;
  char *symbol;
  const BtorBitVector *ass;

  ass    = btor_model_get_bv (btor, node);
  symbol = btor_node_get_symbol (btor, node);

  if (!strcmp (format, "btor"))
  {
    id = btor_node_get_btor_id (node);
    fprintf (file, "%ld ", id ? id : btor_node_get_id (node));
    print_fmt_bv_model_btor (btor, base, ass, file);
    fprintf (file, "%s%s\n", symbol ? " " : "", symbol ? symbol : "");
  }
  else
  {
    if (symbol)
      fprintf (file, "%2c(define-fun %s () ", ' ', symbol);
    else
    {
      id = btor_node_get_btor_id (node);
      fprintf (file,
               "%2c(define-fun v%ld () ",
               ' ',
               id ? id : btor_node_get_id (node));
    }

    btor_dumpsmt_dump_sort_node (node, file);
    fprintf (file, " ");
    btor_dumpsmt_dump_const_value (btor, ass, base, file);
    fprintf (file, ")\n");
  }
}

/*------------------------------------------------------------------------*/

static void
print_param_smt2 (char *symbol,
                  uint32_t param_index,
                  BtorSort *sort,
                  FILE *file)
{
  assert (symbol);
  assert (sort);
  assert (file);

  fprintf (file, "(%s_x%u ", symbol, param_index);
  btor_dumpsmt_dump_sort (sort, file);
  fprintf (file, ")");
}

static void
print_fun_model_smt2 (Btor *btor, BtorNode *node, uint32_t base, FILE *file)
{
  assert (btor);
  assert (node);
  assert (BTOR_IS_REGULAR_NODE (node));
  assert (file);

  char *s, *symbol;
  uint32_t i, x, n;
  int64_t id;
  BtorPtrHashTable *fun_model;
  BtorPtrHashTableIterator it;
  BtorBitVectorTuple *args;
  BtorBitVector *assignment, *tmp;
  BtorSortId sort;
  BtorTupleSortIterator iit;

  fun_model = (BtorPtrHashTable *) btor_model_get_fun (
      btor, btor_simplify_exp (btor, node));
  if (!fun_model) return;

  if ((symbol = btor_node_get_symbol (btor, node)))
    s = symbol;
  else
  {
    BTOR_NEWN (btor->mm, s, 40);
    id = btor_node_get_btor_id (node);
    sprintf (s,
             "%s%ld",
             btor_node_is_uf_array (node) ? "a" : "uf",
             id ? id : node->id);
  }

  fprintf (file, "%2c(define-fun %s (", ' ', s);

  /* fun param sorts */
  node = btor_simplify_exp (btor, node);
  assert (BTOR_IS_REGULAR_NODE (node));
  assert (btor_node_is_fun (node));
  btor_iter_tuple_sort_init (
      &iit,
      btor,
      btor_sort_fun_get_domain (btor, btor_node_get_sort_id (node)));
  x = 0;
  while (btor_iter_tuple_sort_has_next (&iit))
  {
    sort = btor_iter_tuple_sort_next (&iit);
    fprintf (file, "\n%3c", ' ');
    print_param_smt2 (s, x, btor_sort_get_by_id (btor, sort), file);
    x++;
  }
  fprintf (file, ") ");
  sort = btor_sort_fun_get_codomain (btor, btor_node_get_sort_id (node));
  btor_dumpsmt_dump_sort (btor_sort_get_by_id (btor, sort), file);
  fprintf (file, "\n");

  /* fun model as ite over args and assignments */
  n          = 0;
  assignment = 0;
  btor_iter_hashptr_init (&it, fun_model);
  while (btor_iter_hashptr_has_next (&it))
  {
    fprintf (file, "%4c(ite ", ' ');
    assignment = it.bucket->data.as_ptr;
    args       = btor_iter_hashptr_next (&it);
    x          = 0;
    if (args->arity > 1)
    {
      fprintf (file, "\n%6c(and ", ' ');
      for (i = 0; i < args->arity; i++, x++)
      {
        fprintf (file, "\n%8c(= %s_x%d ", ' ', s, x);
        btor_dumpsmt_dump_const_value (btor, args->bv[i], base, file);
        fprintf (file, ")%s", i + 1 == args->arity ? "" : " ");
      }
      fprintf (file, ") ");
      fprintf (file, "\n%6c", ' ');
    }
    else
    {
      fprintf (file, "(= %s_x%d ", s, x);
      btor_dumpsmt_dump_const_value (btor, args->bv[0], base, file);
      fprintf (file, ") ");
    }
    btor_dumpsmt_dump_const_value (btor, assignment, base, file);
    fprintf (file, "\n");
    n += 1;
  }

  /* print default value */
  assert (assignment);
  if (assignment) /* get rid of compiler warning */
  {
    fprintf (file, "%6c", ' ');
    tmp = btor_bv_new (btor->mm, assignment->width);
    btor_dumpsmt_dump_const_value (btor, tmp, base, file);
    btor_bv_free (btor->mm, tmp);
  }

  for (i = 0; i < n; i++) fprintf (file, ")");
  fprintf (file, ")\n");

  if (!symbol) BTOR_DELETEN (btor->mm, s, 40);
}

static void
print_fun_model_btor (Btor *btor, BtorNode *node, uint32_t base, FILE *file)
{
  assert (btor);
  assert (node);
  assert (BTOR_IS_REGULAR_NODE (node));
  assert (file);

  char *symbol;
  int64_t id;
  BtorBitVector *assignment;
  BtorBitVectorTuple *args;
  BtorPtrHashTable *fun_model;
  BtorPtrHashTableIterator it;

  fun_model = (BtorPtrHashTable *) btor_model_get_fun (
      btor, btor_simplify_exp (btor, node));
  if (!fun_model) return;

  symbol = btor_node_get_symbol (btor, node);
  id     = btor_node_get_btor_id (node);

  btor_iter_hashptr_init (&it, fun_model);
  while (btor_iter_hashptr_has_next (&it))
  {
    assignment = it.bucket->data.as_ptr;
    args       = btor_iter_hashptr_next (&it);
    // TODO: distinguish between functions and arrays (ma)
    //       needs proper sort handling
    fprintf (file, "%ld[", id ? id : node->id);
    print_fmt_bv_model_tuple_btor (btor, base, args, file);
    fprintf (file, "] ");
    print_fmt_bv_model_btor (btor, base, assignment, file);
    fprintf (file, "%s%s\n", symbol ? " " : "", symbol ? symbol : "");
  }
}

static void
print_fun_model (
    Btor *btor, BtorNode *node, char *format, uint32_t base, FILE *file)
{
  assert (btor);
  assert (node);
  assert (format);
  assert (file);
  assert (BTOR_IS_REGULAR_NODE (node));

  if (!strcmp (format, "btor"))
    print_fun_model_btor (btor, node, base, file);
  else
    print_fun_model_smt2 (btor, node, base, file);
}

/*------------------------------------------------------------------------*/

void
btor_print_model (Btor *btor, char *format, FILE *file)
{
  assert (btor);
  assert (btor->last_sat_result == BTOR_RESULT_SAT);
  assert (format);
  assert (!strcmp (format, "btor") || !strcmp (format, "smt2"));
  assert (file);

  BtorNode *cur;
  BtorPtrHashTableIterator it;
  uint32_t base;

  base = btor_opt_get (btor, BTOR_OPT_OUTPUT_NUMBER_FORMAT);

  if (!strcmp (format, "smt2"))
    fprintf (file, "(model%s", btor->inputs->count ? "\n" : " ");

  btor_iter_hashptr_init (&it, btor->inputs);
  while (btor_iter_hashptr_has_next (&it))
  {
    cur = btor_iter_hashptr_next (&it);
    if (btor_node_is_fun (btor_simplify_exp (btor, cur)))
      print_fun_model (btor, cur, format, base, file);
    else
      print_bv_model (btor, cur, format, base, file);
  }

  if (!strcmp (format, "smt2")) fprintf (file, ")\n");
}

/*------------------------------------------------------------------------*/
/* print value                                                            */
/*------------------------------------------------------------------------*/

static void
print_bv_value_smt2 (
    Btor *btor, BtorNode *node, char *symbol_str, uint32_t base, FILE *file)
{
  assert (btor);
  assert (node);

  char *symbol;
  const BtorBitVector *ass;
  int64_t id;

  ass    = btor_model_get_bv (btor, node);
  symbol = symbol_str ? symbol_str : btor_node_get_symbol (btor, node);

  if (symbol)
    fprintf (file, "(%s ", symbol);
  else
  {
    id = btor_node_get_btor_id (BTOR_REAL_ADDR_NODE (node));
    fprintf (file,
             "(v%ld ",
             id ? id : btor_node_get_id (BTOR_REAL_ADDR_NODE (node)));
  }

  btor_dumpsmt_dump_const_value (btor, ass, base, file);
  fprintf (file, ")");
}

/*------------------------------------------------------------------------*/

static void
print_fun_value_smt2 (
    Btor *btor, BtorNode *node, char *symbol_str, uint32_t base, FILE *file)
{
  assert (btor);
  assert (node);
  assert (BTOR_IS_REGULAR_NODE (node));
  assert (file);

  uint32_t i, n;
  int64_t id;
  char *symbol;
  BtorPtrHashTable *fun_model;
  BtorPtrHashTableIterator it;
  BtorBitVectorTuple *args;
  BtorBitVector *assignment;

  fun_model = (BtorPtrHashTable *) btor_model_get_fun (btor, node);
  if (!fun_model) return;

  symbol = symbol_str ? symbol_str : btor_node_get_symbol (btor, node);

  fprintf (file, "(");

  n = 0;
  btor_iter_hashptr_init (&it, fun_model);
  while (btor_iter_hashptr_has_next (&it))
  {
    if (symbol)
      fprintf (file, "%s((%s ", n++ ? "\n  " : "", symbol);
    else
    {
      id = btor_node_get_btor_id (BTOR_REAL_ADDR_NODE (node));
      fprintf (file,
               "(%s%ld ",
               btor_node_is_array (node) ? "a" : "uf",
               id ? id : btor_node_get_id (BTOR_REAL_ADDR_NODE (node)));
    }
    assignment = it.bucket->data.as_ptr;
    args       = btor_iter_hashptr_next (&it);
    if (args->arity > 1)
    {
      for (i = 0; i < args->arity; i++)
      {
        btor_dumpsmt_dump_const_value (btor, args->bv[i], base, file);
        fprintf (file, ")%s", i + 1 == args->arity ? "" : " ");
      }
      fprintf (file, ") ");
    }
    else
    {
      btor_dumpsmt_dump_const_value (btor, args->bv[0], base, file);
      fprintf (file, ") ");
    }
    btor_dumpsmt_dump_const_value (btor, assignment, base, file);
    fprintf (file, ")");
  }

  fprintf (file, ")");
}

/*------------------------------------------------------------------------*/

void
btor_print_value_smt2 (Btor *btor, BtorNode *exp, char *symbol_str, FILE *file)
{
  assert (btor);
  assert (btor->last_sat_result == BTOR_RESULT_SAT);
  assert (exp);
  assert (file);

  uint32_t base;

  base = btor_opt_get (btor, BTOR_OPT_OUTPUT_NUMBER_FORMAT);
  if (btor_node_is_fun (btor_simplify_exp (btor, exp)))
    print_fun_value_smt2 (btor, exp, symbol_str, base, file);
  else
    print_bv_value_smt2 (btor, exp, symbol_str, base, file);
}

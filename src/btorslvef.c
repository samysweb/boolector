/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2015 Mathias Preiner.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */

#include "btorslvef.h"
#include "btorabort.h"
#include "btorbeta.h"
#include "btorbitvec.h"
#include "btorclone.h"
#include "btorcore.h"
#include "btorexp.h"
#include "btormodel.h"
#include "btorslvfun.h"
#include "btorsynthfun.h"
#include "normalizer/btornormquant.h"
#include "normalizer/btorskolemize.h"
#include "simplifier/btorder.h"
#include "simplifier/btorminiscope.h"
#include "utils/btorhashint.h"
#include "utils/btoriter.h"
#include "utils/btormisc.h"
#include "utils/btorutil.h"

// TODO (ma): debug
#include "dumper/btordumpbtor.h"
#include "dumper/btordumpsmt.h"

//#define PRINT_DBG

struct BtorEFGroundSolvers
{
  //  BtorNodeMap *forall_ufs;
  //  BtorPtrHashTable *forall_synth_funs;   /* synthesized functions cache */
  //  BtorPtrHashTable *forall_synth_inputs; /* synthesized functions input
  //  cache */
  Btor *forall; /* solver for checking the model */
  BtorNode *forall_formula;
  BtorNodeMap *forall_evars;          /* existential vars (map to skolem
                                         constants of exists solver) */
  BtorNodeMap *forall_uvars;          /* universal vars map to fresh bv vars */
  BtorNodeMap *forall_evar_deps;      /* existential vars map to argument nodes
                                         of universal vars */
  BtorPtrHashTable *forall_cur_model; /* currently synthesized model for
                                         existential vars */
  BtorNodeMap *forall_skolem;         /* skolem functions for evars */

  Btor *exists;              /* solver for computing the model */
  BtorNodeMap *exists_evars; /* skolem constants (map to existential
                                vars of forall solver) */
  BtorNodeMap *exists_ufs;   /* UFs (non-skolem constants), map to UFs
                                of forall solver */
  BtorPtrHashTable *exists_evar_models;
  //  BtorPtrHashTable *exists_refinements;

  struct
  {
    double e_solver;
    double f_solver;
    double synth;
    double qinst;
  } time;

  struct
  {
    uint32_t refinements;
  } stats;
};

typedef struct BtorEFGroundSolvers BtorEFGroundSolvers;

/*------------------------------------------------------------------------*/

static void
print_cur_model (BtorEFGroundSolvers *gslv)
{
  uint32_t i;
  BtorNode *cur;
  BtorHashTableIterator it;
  BtorSynthResult *synth_res;

  if (!gslv->forall_cur_model) return;

  btor_init_node_hash_table_iterator (&it, gslv->forall_cur_model);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    synth_res = it.bucket->data.as_ptr;
    cur       = btor_next_node_hash_table_iterator (&it);
    assert (btor_is_uf_node (cur) || btor_param_is_exists_var (cur));
    printf ("\nmodel for %s\n", btor_get_symbol_exp (gslv->forall, cur));
    switch (synth_res->type)
    {
      case BTOR_SYNTH_TYPE_SK_VAR:
      case BTOR_SYNTH_TYPE_UF:
        btor_dump_smt2_node (gslv->forall, stdout, synth_res->value, -1);
        break;
      default:
        assert (synth_res->type == BTOR_SYNTH_TYPE_SK_UF);
        if (BTOR_COUNT_STACK (synth_res->exps) == 1)
          btor_dump_smt2_node (
              gslv->forall, stdout, BTOR_TOP_STACK (synth_res->exps), -1);
        else
        {
          for (i = 0; i < BTOR_COUNT_STACK (synth_res->exps); i++)
          {
            printf ("  m[%d]: ", i);
            btor_dump_smt2_node (
                gslv->forall, stdout, BTOR_PEEK_STACK (synth_res->exps, i), -1);
          }
        }
    }
  }
}

static void
delete_model (BtorEFGroundSolvers *gslv)
{
  BtorNode *cur;
  BtorHashTableIterator it;
  BtorSynthResult *synth_res;

  if (!gslv->forall_cur_model) return;

  btor_init_node_hash_table_iterator (&it, gslv->forall_cur_model);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    synth_res = it.bucket->data.as_ptr;
    cur       = btor_next_node_hash_table_iterator (&it);
    assert (btor_is_uf_node (cur) || btor_param_is_exists_var (cur));
    btor_delete_synth_result (gslv->forall->mm, synth_res);
  }
  btor_delete_ptr_hash_table (gslv->forall_cur_model);
  gslv->forall_cur_model = 0;
}

#if 0
static void
reset_refinements (BtorEFGroundSolvers * gslv)
{
  BtorNode *cur;
  BtorHashTableIterator it;

  if (!gslv->exists_refinements)
    return;

  btor_init_node_hash_table_iterator (&it, gslv->exists_refinements);
  while (btor_has_next_node_hash_table_iterator (&it))
    {
      cur = btor_next_node_hash_table_iterator (&it);
      btor_release_exp (gslv->exists, cur);
    }
  btor_delete_ptr_hash_table (gslv->exists_refinements);
  gslv->exists_refinements = 0;
}
#endif

static void
update_node_map (BtorIntHashTable *node_map, BtorIntHashTable *update)
{
  assert (node_map->data);
  assert (update->data);

  int32_t key;
  size_t i;

  for (i = 0; i < node_map->size; i++)
  {
    if (!node_map->keys[i]) continue;
    key = node_map->data[i].as_int;
    /* if key didn't get updated we don't have to update 'node_map' */
    if (!btor_contains_int_hash_map (update, key)) continue;
    node_map->data[i].as_int = btor_get_int_hash_map (update, key)->as_int;
  }
}

/* compute dependencies between existential variables and universal variables.
 * 'deps' maps existential variables to a list of universal variables by means
 * of an argument node.
 */
BtorNodeMap *
compute_edeps (Btor *btor, BtorNode *root)
{
  uint32_t i;
  BtorNode *cur, *real_cur, *q, *args;
  BtorNodePtrStack visit, quants, uvars;
  BtorMemMgr *mm;
  BtorIntHashTable *map;
  BtorHashTableData *d;
  BtorNodeMap *deps;

  mm = btor->mm;

  BTOR_INIT_STACK (uvars);
  BTOR_INIT_STACK (quants);
  BTOR_INIT_STACK (visit);
  BTOR_PUSH_STACK (mm, visit, root);
  map  = btor_new_int_hash_map (mm);
  deps = btor_new_node_map (btor);

  while (!BTOR_EMPTY_STACK (visit))
  {
    cur      = BTOR_POP_STACK (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    d = btor_get_int_hash_map (map, real_cur->id);
    if (!d)
    {
      btor_add_int_hash_map (map, real_cur->id);

      if (btor_is_forall_node (real_cur)) BTOR_PUSH_STACK (mm, quants, cur);

      BTOR_PUSH_STACK (mm, visit, cur);
      for (i = 0; i < real_cur->arity; i++)
        BTOR_PUSH_STACK (mm, visit, real_cur->e[i]);
    }
    else if (d->as_int == 0)
    {
      d->as_int = 1;
      if (btor_is_exists_node (real_cur) && !BTOR_EMPTY_STACK (quants))
      {
        /* create dependency of 'real_cur' with all universal vars of
         * 'quants' */
        for (i = 0; i < BTOR_COUNT_STACK (quants); i++)
        {
          q = BTOR_PEEK_STACK (quants, i);
          BTOR_PUSH_STACK (mm, uvars, q->e[0]);
        }

        args = btor_args_exp (btor, uvars.start, BTOR_COUNT_STACK (uvars));
        btor_map_node (deps, real_cur->e[0], args);
        btor_release_exp (btor, args);
        BTOR_RESET_STACK (uvars);
      }
      else if (btor_is_forall_node (real_cur))
      {
        q = BTOR_POP_STACK (quants);
        assert (q == cur);
      }
    }
  }
  btor_delete_int_hash_map (map);
  BTOR_RELEASE_STACK (mm, visit);
  BTOR_RELEASE_STACK (mm, quants);
  BTOR_RELEASE_STACK (mm, uvars);
  return deps;
}

static BtorEFGroundSolvers *
setup_efg_solvers (BtorEFSolver *slv,
                   BtorNode *root,
                   BtorIntHashTable *node_map,
                   const char *prefix_forall,
                   const char *prefix_exists)
{
  bool opt_dual_solver;
  uint32_t width;
  char *sym;
  BtorEFGroundSolvers *res;
  BtorNode *cur, *var, *tmp;
  BtorHashTableIterator it;
  BtorNodeMapIterator nit;
  BtorFunSolver *fslv;
  BtorNodeMap *exp_map;
  Btor *btor;
  BtorSortUniqueTable *sorts;
  BtorSortId dsortid, cdsortid, funsortid;
  BtorIntHashTable *tmp_map = 0;
  BtorMemMgr *mm;

  btor            = slv->btor;
  mm              = btor->mm;
  opt_dual_solver = btor_get_opt (btor, BTOR_OPT_EF_DUAL_SOLVER) == 1;
  BTOR_CNEW (mm, res);

  /* new forall solver */
  res->forall = btor_new_btor ();
  btor_delete_opts (res->forall);
  btor_clone_opts (btor, res->forall);
  btor_set_msg_prefix_btor (res->forall, prefix_forall);

  exp_map = btor_new_node_map (btor);
  if (opt_dual_solver) tmp_map = btor_new_int_hash_map (mm);

  /* configure options */
  btor_set_opt (res->forall, BTOR_OPT_MODEL_GEN, 1);
  btor_set_opt (res->forall, BTOR_OPT_INCREMENTAL, 1);
  // FIXME (ma): if -bra is enabled then test_synth5.smt2 fails without
  // disabling this since f_formula will be simplified
  btor_set_opt (res->forall, BTOR_OPT_BETA_REDUCE_ALL, 0);

  tmp = btor_recursively_rebuild_exp_clone (
      btor,
      res->forall,
      root,
      exp_map,
      btor_get_opt (res->forall, BTOR_OPT_REWRITE_LEVEL));
  /* all bv vars are quantified with exists */
  assert (res->forall->bv_vars->count == 0);

  /* update 'tmp_map' for mapping nodes */
#if 0
  if (opt_dual_solver)
    {
      btor_init_node_map_iterator (&nit, exp_map);
      while (btor_has_next_node_map_iterator (&nit))
	{
	  tmp = nit.it.bucket->data.as_ptr;
	  cur = btor_next_node_map_iterator (&nit);
	  btor_add_int_hash_map (tmp_map, cur->id)->as_int =
	    BTOR_REAL_ADDR_NODE (tmp)->id;
	}
    }
#endif
  btor_delete_node_map (exp_map);

#if 0
  if (opt_dual_solver)
    {
      update_node_map (node_map, tmp_map);
      btor_delete_int_hash_map (tmp_map);
      tmp_map = btor_new_int_hash_map (mm);
    }
#endif

  root = tmp;
  assert (!btor_is_proxy_node (root));
  res->forall_formula   = root;
  res->forall_evar_deps = compute_edeps (res->forall, root);
  res->forall_evars     = btor_new_node_map (res->forall);
  res->forall_uvars     = btor_new_node_map (res->forall);
  res->forall_skolem    = btor_new_node_map (res->forall);
  sorts                 = &res->forall->sorts_unique_table;

  /* map fresh bit vector vars to universal vars */
  btor_init_node_hash_table_iterator (&it, res->forall->forall_vars);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    assert (btor_param_is_forall_var (cur));
    var = btor_var_exp (res->forall, btor_get_exp_width (res->forall, cur), 0);
    btor_map_node (res->forall_uvars, cur, var);
    btor_release_exp (res->forall, var);
    //      printf ("uvar: %s\n", node2string (cur));
  }

  /* map fresh skolem constants to existential vars */
  btor_init_node_hash_table_iterator (&it, res->forall->exists_vars);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    assert (btor_param_is_exists_var (cur));

    tmp = btor_mapped_node (res->forall_evar_deps, cur);
    if (tmp)
    {
      funsortid = btor_fun_sort (sorts, tmp->sort_id, cur->sort_id);
      var       = btor_uf_exp (res->forall, funsortid, 0);
      btor_release_sort (sorts, funsortid);
    }
    else
      var =
          btor_var_exp (res->forall, btor_get_exp_width (res->forall, cur), 0);

    btor_map_node (res->forall_skolem, cur, var);
    btor_release_exp (res->forall, var);
  }

  /* create ground solver for forall */
  assert (!res->forall->slv);
  fslv                = (BtorFunSolver *) btor_new_fun_solver (res->forall);
  fslv->assume_lemmas = true;
  res->forall->slv    = (BtorSolver *) fslv;

  /* new exists solver */
  res->exists = btor_new_btor ();
  btor_delete_opts (res->exists);
  btor_clone_opts (res->forall, res->exists);
  btor_set_msg_prefix_btor (res->exists, prefix_exists);
  btor_set_opt (res->exists, BTOR_OPT_AUTO_CLEANUP_INTERNAL, 1);

  /* create ground solver for exists */
  res->exists->slv  = btor_new_fun_solver (res->exists);
  res->exists_evars = btor_new_node_map (res->exists);
  res->exists_ufs   = btor_new_node_map (res->exists);
  //  res->exists_refinements = btor_new_ptr_hash_table (res->exists->mm, 0, 0);
  res->exists_evar_models = btor_new_ptr_hash_table (res->exists->mm, 0, 0);
  sorts                   = &res->exists->sorts_unique_table;

  /* map evars of exists solver to evars of forall solver */
  btor_init_node_hash_table_iterator (&it, res->forall->exists_vars);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    assert (btor_param_is_exists_var (cur));
    width = btor_get_exp_width (res->forall, cur);
    sym   = btor_get_symbol_exp (res->forall, cur);
    //      printf ("evar: %s\n", node2string (cur));

    if ((tmp = btor_mapped_node (res->forall_evar_deps, cur)))
    {
      /* 'tmp' is an argument node that holds all universal dependencies of
       * existential variable 'cur'*/
      assert (btor_is_args_node (tmp));

      cdsortid = btor_bitvec_sort (sorts, width);
      dsortid  = btor_recursively_rebuild_sort_clone (
          res->forall, res->exists, tmp->sort_id);
      funsortid = btor_fun_sort (sorts, dsortid, cdsortid);
      var       = btor_uf_exp (res->exists, funsortid, sym);
      btor_release_sort (sorts, cdsortid);
      btor_release_sort (sorts, dsortid);
      btor_release_sort (sorts, funsortid);
    }
    else
      var = btor_var_exp (res->exists, width, sym);
    btor_map_node (res->exists_evars, var, cur);
    btor_map_node (res->forall_evars, cur, var);
    btor_release_exp (res->exists, var);
  }

  /* map ufs of exists solver to ufs of forall solver */
  btor_init_node_hash_table_iterator (&it, res->forall->ufs);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur       = btor_next_node_hash_table_iterator (&it);
    funsortid = btor_recursively_rebuild_sort_clone (
        res->forall, res->exists, cur->sort_id);
    var = btor_uf_exp (
        res->exists, funsortid, btor_get_symbol_exp (res->forall, cur));
    btor_release_sort (sorts, funsortid);
    btor_map_node (res->exists_ufs, var, cur);
    btor_release_exp (res->exists, var);
  }

  return res;
}

static void
delete_efg_solvers (BtorEFSolver *slv, BtorEFGroundSolvers *gslv)
{
  BtorHashTableIterator it, iit;
  BtorPtrHashTable *m;

  /* delete exists solver */
  btor_delete_node_map (gslv->exists_evars);
  btor_delete_node_map (gslv->exists_ufs);

  btor_init_hash_table_iterator (&it, gslv->exists_evar_models);
  while (btor_has_next_hash_table_iterator (&it))
  {
    m = it.bucket->data.as_ptr;
    (void) btor_next_hash_table_iterator (&it);
    btor_init_hash_table_iterator (&iit, m);
    while (btor_has_next_hash_table_iterator (&iit))
    {
      btor_free_bv (gslv->exists->mm, iit.bucket->data.as_ptr);
      btor_free_bv_tuple (gslv->exists->mm,
                          btor_next_hash_table_iterator (&iit));
    }
    btor_delete_ptr_hash_table (m);
  }
  btor_delete_ptr_hash_table (gslv->exists_evar_models);

  /* delete forall solver */
  delete_model (gslv);
  //  reset_refinements (gslv);
  btor_delete_node_map (gslv->forall_evars);
  btor_delete_node_map (gslv->forall_uvars);
  btor_delete_node_map (gslv->forall_evar_deps);
  btor_delete_node_map (gslv->forall_skolem);

  btor_release_exp (gslv->forall, gslv->forall_formula);
  btor_delete_btor (gslv->forall);
  btor_delete_btor (gslv->exists);
  BTOR_DELETE (slv->btor->mm, gslv);
}

static BtorNode *
build_refinement (Btor *btor, BtorNode *root, BtorNodeMap *map)
{
  assert (btor);
  assert (root);
  assert (map);

  size_t j;
  int32_t i;
  BtorMemMgr *mm;
  BtorNode *cur, *real_cur, *result, **e;
  BtorNodePtrStack visit, args;
  BtorIntHashTable *mark;
  BtorHashTableData *d;

  mm   = btor->mm;
  mark = btor_new_int_hash_map (mm);
  BTOR_INIT_STACK (visit);
  BTOR_INIT_STACK (args);
  BTOR_PUSH_STACK (mm, visit, root);

  while (!BTOR_EMPTY_STACK (visit))
  {
    cur      = BTOR_POP_STACK (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);
    assert (!btor_is_proxy_node (real_cur));

    if ((result = btor_mapped_node (map, real_cur)))
    {
      result = btor_copy_exp (btor, result);
      goto PUSH_RESULT;
    }

    d = btor_get_int_hash_map (mark, real_cur->id);
    if (!d)
    {
      (void) btor_add_int_hash_map (mark, real_cur->id);
      BTOR_PUSH_STACK (mm, visit, cur);
      for (i = real_cur->arity - 1; i >= 0; i--)
        BTOR_PUSH_STACK (mm, visit, real_cur->e[i]);
    }
    else if (!d->as_ptr)
    {
      assert (!btor_is_param_node (real_cur)
              || !btor_param_is_exists_var (real_cur)
              || !btor_param_is_forall_var (real_cur));
      assert (!btor_is_bv_var_node (real_cur));
      assert (!btor_is_uf_node (real_cur));

      args.top -= real_cur->arity;
      e = args.top;

      if (btor_is_bv_const_node (real_cur))
      {
        result = btor_const_exp (btor, btor_const_get_bits (real_cur));
      }
      else if (btor_is_param_node (real_cur))
      {
        assert (!btor_param_is_exists_var (real_cur));
        assert (!btor_param_is_forall_var (real_cur));
        result = btor_param_exp (
            btor, btor_get_exp_width (real_cur->btor, real_cur), 0);
      }
      else if (btor_is_slice_node (real_cur))
      {
        result = btor_slice_exp (btor,
                                 e[0],
                                 btor_slice_get_upper (real_cur),
                                 btor_slice_get_lower (real_cur));
      }
      /* universal/existential vars get substituted */
      else if (btor_is_quantifier_node (real_cur))
      {
        assert (!btor_is_param_node (e[0]));
        result = btor_copy_exp (btor, e[1]);
      }
      else
        result = btor_create_exp (btor, real_cur->kind, e, real_cur->arity);

      for (i = 0; i < real_cur->arity; i++) btor_release_exp (btor, e[i]);

      d->as_ptr = btor_copy_exp (btor, result);

    PUSH_RESULT:
      BTOR_PUSH_STACK (mm, args, BTOR_COND_INVERT_NODE (cur, result));
    }
    else
    {
      assert (d->as_ptr);
      result = btor_copy_exp (btor, d->as_ptr);
      goto PUSH_RESULT;
    }
  }
  assert (BTOR_COUNT_STACK (args) == 1);
  result = BTOR_POP_STACK (args);

  BTOR_RELEASE_STACK (mm, visit);
  BTOR_RELEASE_STACK (mm, args);

  for (j = 0; j < mark->size; j++)
  {
    if (!mark->keys[j]) continue;
    assert (mark->data[j].as_ptr);
    btor_release_exp (btor, mark->data[j].as_ptr);
  }
  btor_delete_int_hash_map (mark);

  return result;
}

static BtorNode *
instantiate_args (Btor *btor, BtorNode *args, BtorNodeMap *map)
{
  assert (map);
  assert (btor_is_args_node (args));

  BtorNodePtrStack stack;
  BtorArgsIterator it;
  BtorNode *res, *arg, *mapped;
  BtorMemMgr *mm;

  mm = btor->mm;
  BTOR_INIT_STACK (stack);
  btor_init_args_iterator (&it, args);
  while (btor_has_next_args_iterator (&it))
  {
    arg = btor_next_args_iterator (&it);
    assert (btor_param_is_forall_var (arg));
    mapped = btor_mapped_node (map, arg);
    assert (mapped);
    BTOR_PUSH_STACK (mm, stack, mapped);
  }
  res = btor_args_exp (btor, stack.start, BTOR_COUNT_STACK (stack));
  BTOR_RELEASE_STACK (mm, stack);
  return res;
}

static void
refine_exists_solver (BtorEFGroundSolvers *gslv)
{
  Btor *f_solver, *e_solver;
  BtorNodeMap *map;
  BtorNodeMapIterator it;
  BtorNode *var_es, *var_fs, *c, *res, *uvar, *a;
  const BtorBitVector *bv;

  //  printf ("  refine\n");
  f_solver = gslv->forall;
  e_solver = gslv->exists;

  map = btor_new_node_map (f_solver);

  /* generate counter example for universal vars */
  assert (f_solver->last_sat_result == BTOR_RESULT_SAT);
  f_solver->slv->api.generate_model (f_solver->slv, false, false);

  /* instantiate universal vars with counter example */
  btor_init_node_map_iterator (&it, gslv->forall_uvars);
  while (btor_has_next_node_map_iterator (&it))
  {
    var_fs = it.it.bucket->data.as_ptr;
    uvar   = btor_next_node_map_iterator (&it);
    bv     = btor_get_bv_model (f_solver, btor_simplify_exp (f_solver, var_fs));
    //      printf ("%s -> ", node2string (uvar));
    //      btor_print_bv (bv);
    c = btor_const_exp (e_solver, (BtorBitVector *) bv);
    btor_map_node (map, uvar, c);
    btor_release_exp (e_solver, c);
  }

  /* map existential variables to skolem constants */
  btor_init_node_map_iterator (&it, gslv->forall_evars);
  while (btor_has_next_node_map_iterator (&it))
  {
    var_es = it.it.bucket->data.as_ptr;
    var_fs = btor_next_node_map_iterator (&it);

    a = btor_mapped_node (gslv->forall_evar_deps, var_fs);
    if (a)
    {
      assert (btor_is_uf_node (var_es));
      a      = instantiate_args (e_solver, a, map);
      var_es = btor_apply_exp (e_solver, var_es, a);
      btor_map_node (map, var_fs, var_es);
      btor_release_exp (e_solver, a);
      btor_release_exp (e_solver, var_es);
    }
    else
      btor_map_node (map, var_fs, var_es);
  }

  /* map UFs */
  btor_init_node_map_iterator (&it, gslv->exists_ufs);
  while (btor_has_next_node_map_iterator (&it))
  {
    var_fs = it.it.bucket->data.as_ptr;
    var_es = btor_next_node_map_iterator (&it);
    btor_map_node (map, var_fs, var_es);
  }

#if 0
  assert (f_solver->unsynthesized_constraints->count == 0);
  assert (f_solver->synthesized_constraints->count == 0);
  assert (f_solver->embedded_constraints->count == 0);
  assert (f_solver->varsubst_constraints->count == 0);
#endif
  res = build_refinement (e_solver, gslv->forall_formula, map);

  btor_delete_node_map (map);

  //  printf (">>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
  //  btor_dump_smt2_node (e_solver, stdout, res, -1);
  //  printf ("<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n");
  assert (res != e_solver->true_exp);
  BTOR_ABORT (
      res == e_solver->true_exp, "invalid refinement '%s'", node2string (res));
  gslv->stats.refinements++;
#if 0
#if 0
  if (gslv->exists_refinements->count % (3 * (gslv->stats.refinements + 1)) == 0)
    {
      reset_refinements (gslv);
      gslv->exists_refinements = btor_new_ptr_hash_table (gslv->exists->mm, 0, 0);
    }
#endif
  btor_add_ptr_hash_table (gslv->exists_refinements, res);
#else
  btor_assert_exp (e_solver, res);
  btor_release_exp (e_solver, res);
//  printf ("********\n");
//  btor_dump_smt2 (e_solver, stdout);
#endif
}

/* collect all function applications on skolem functions below 'exp'. */
static void
build_dependencies (BtorMemMgr *mm,
                    BtorNode *uf,
                    BtorNode *exp,
                    BtorPtrHashTable *deps)
{
  uint32_t i;
  BtorNode *cur;
  BtorNodePtrStack visit;
  BtorIntHashTable *cache;
  BtorPtrHashTable *t;
  BtorPtrHashBucket *b;

  cache = btor_new_int_hash_table (mm);

  BTOR_INIT_STACK (visit);
  BTOR_PUSH_STACK (mm, visit, exp);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_contains_int_hash_table (cache, cur->id)) continue;

    btor_add_int_hash_table (cache, cur->id);
    /* only function applications on UFs are considered as inputs */
    if (btor_is_apply_node (cur) && btor_is_uf_node (cur->e[0]))
    {
      b = btor_get_ptr_hash_table (deps, cur->e[0]);
      if (!b)
      {
        b              = btor_add_ptr_hash_table (deps, cur->e[0]);
        t              = btor_new_ptr_hash_table (mm, 0, 0);
        b->data.as_ptr = t;
      }
      else
        t = b->data.as_ptr;
      //	  printf ("dep: %s -> %s\n", node2string (cur->e[0]),
      // node2string (uf));
      btor_add_ptr_hash_table (t, uf);
      continue;
    }

    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (mm, visit, cur->e[i]);
  }
  btor_delete_int_hash_table (cache);
  BTOR_RELEASE_STACK (mm, visit);
}

/* check whether 'to_synth' is dependent on 'cur_in' */
static bool
is_dependent (BtorMemMgr *mm,
              BtorNode *to_synth,
              BtorNode *cur_in,
              BtorPtrHashTable *deps)
{
  bool result = false;
  BtorHashTableIterator it;
  BtorPtrHashBucket *b;
  BtorPtrHashTable *t;
  BtorIntHashTable *cache;
  BtorNodePtrStack to_check;
  BtorNode *cur;

  BTOR_INIT_STACK (to_check);
  cache = btor_new_int_hash_table (mm);
  cur   = to_synth;
  goto START_CYCLE_CHECK;
  while (!BTOR_EMPTY_STACK (to_check))
  {
    cur = BTOR_POP_STACK (to_check);

    if (cur == cur_in)
    {
      result = true;
      break;
    }

  START_CYCLE_CHECK:
    b = btor_get_ptr_hash_table (deps, cur);
    if (!b) continue;
    assert (b->data.as_ptr);
    t = b->data.as_ptr;
    btor_init_node_hash_table_iterator (&it, t);
    while (btor_has_next_node_hash_table_iterator (&it))
    {
      cur = btor_next_node_hash_table_iterator (&it);
      if (!btor_contains_int_hash_table (cache, cur->id))
      {
        BTOR_PUSH_STACK (mm, to_check, cur);
        btor_add_int_hash_table (cache, cur->id);
      }
    }
  }
  BTOR_RELEASE_STACK (mm, to_check);
  btor_delete_int_hash_table (cache);
  return result;
}

static void
filter_inputs (BtorMemMgr *mm, BtorNode *fs_uf, BtorPtrHashTable *inputs)
{
  uint32_t i, pos;
  BtorNode *cur;
  BtorNodeIterator it, iit;
  BtorNodePtrStack visit;
  BtorIntHashTable *cache;
  BtorHashTableIterator hit;

  cache = btor_new_int_hash_table (mm);
  BTOR_INIT_STACK (visit);
  btor_init_parent_iterator (&it, fs_uf);
  while (btor_has_next_parent_iterator (&it))
  {
    cur = btor_next_parent_iterator (&it);

    btor_init_parent_iterator (&iit, cur);
    while (btor_has_next_parent_iterator (&iit))
    {
      pos = BTOR_GET_TAG_NODE (iit.cur);
      cur = btor_next_parent_iterator (&iit);

      for (i = 0; i < cur->arity; i++)
        if (i != pos) BTOR_PUSH_STACK (mm, visit, cur->e[i]);
    }
  }

  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_contains_int_hash_table (cache, cur->id)) continue;

    btor_add_int_hash_table (cache, cur->id);
    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (mm, visit, cur->e[i]);
  }

  btor_init_node_hash_table_iterator (&hit, inputs);
  while (btor_has_next_node_hash_table_iterator (&hit))
  {
    cur = btor_next_node_hash_table_iterator (&hit);
    if (!btor_contains_int_hash_table (cache, cur->id))
      btor_remove_ptr_hash_table (inputs, cur, 0, 0);
  }
  btor_delete_int_hash_table (cache);
  BTOR_RELEASE_STACK (mm, visit);
}

static bool
check_input_prefix (Btor *btor, BtorNode *uf, BtorNode *cur_uf)
{
  assert (BTOR_IS_REGULAR_NODE (uf));
  assert (BTOR_IS_REGULAR_NODE (cur_uf));
  assert (btor_is_uf_node (cur_uf));
  assert (btor_is_uf_node (cur_uf));

  uint32_t arity0, arity1;
  BtorArgsIterator it0, it1;
  BtorNodeIterator it;
  BtorNode *app0 = 0, *app1 = 0, *arg0, *arg1, *cur;

  btor_init_parent_iterator (&it, uf);
  while (btor_has_next_parent_iterator (&it))
  {
    cur = btor_next_parent_iterator (&it);
    if (!cur->parameterized)
    {
      if (app0) return false;
      app0 = cur;
    }
  }

  btor_init_parent_iterator (&it, cur_uf);
  while (btor_has_next_parent_iterator (&it))
  {
    cur = btor_next_parent_iterator (&it);
    if (!cur->parameterized)
    {
      if (app1) return false;
      app1 = cur;
    }
  }

  if (!app0 || !app1) return false;

  arity0 = btor_get_fun_arity (btor, uf);
  arity1 = btor_get_fun_arity (btor, cur_uf);

  /* 'cur_uf' is dependent on more universals than 'uf', hence it cannot be an
   * input for 'uf'. */
  if (arity1 > arity0) return false;

  app0 = uf->first_parent;
  app1 = cur_uf->first_parent;
  assert (BTOR_IS_REGULAR_NODE (app0));
  assert (BTOR_IS_REGULAR_NODE (app1));
  assert (btor_is_apply_node (app0));
  assert (btor_is_apply_node (app1));

  if (app0->e[1] == app1->e[1])
    return true;
  else if (arity0 == arity1)
    return false;

  assert (arity0 > arity1);
  btor_init_args_iterator (&it0, app0->e[1]);
  btor_init_args_iterator (&it1, app1->e[1]);
  while (btor_has_next_args_iterator (&it1))
  {
    assert (btor_has_next_args_iterator (&it0));
    arg0 = btor_next_args_iterator (&it0);
    arg1 = btor_next_args_iterator (&it1);
    if (arg0 != arg1) return false;
  }
  return true;
}

#if 0
static BtorPtrHashTable *
prepare_inputs (BtorEFGroundSolvers * gslv, BtorNode * fs_uf,
		BtorNodeMap * model)
{
  assert (BTOR_IS_REGULAR_NODE (fs_uf));
  assert (btor_is_uf_node (fs_uf));

  BtorNode *cur, *cur_fs, *cur_synth_fun;
  BtorHashTableIterator it;
  const BtorPtrHashTable *m;
  BtorPtrHashTable *deps, *inputs;
  Btor *e_solver, *f_solver;
  BtorMemMgr *mm;

  mm = gslv->forall->mm; 
  deps = btor_new_ptr_hash_table (mm, 0, 0);
  inputs = btor_new_ptr_hash_table (mm, 0, 0);
  e_solver = gslv->exists;
  f_solver = gslv->forall;

  btor_init_node_hash_table_iterator (&it, gslv->forall_evars->table);
  while (btor_has_next_node_hash_table_iterator (&it))
    {
      cur = btor_next_node_hash_table_iterator (&it);
      cur_synth_fun = btor_mapped_node (model, cur);
      if (!cur_synth_fun) continue;
      build_dependencies (mm, cur, cur_synth_fun, deps);
    }

  btor_init_node_hash_table_iterator (&it, gslv->exists_evars->table);
  while (btor_has_next_node_hash_table_iterator (&it))
    {
      cur_fs = it.bucket->data.as_ptr;
      cur = btor_next_node_hash_table_iterator (&it);

      if (cur_fs->id == fs_uf->id)
	continue;

      if (btor_is_uf_node (cur_fs)
	  && !check_input_prefix (f_solver, fs_uf, cur_fs))
	continue;

      if (is_dependent (mm, fs_uf, cur_fs, deps))
	continue;

      if (btor_is_fun_sort (&e_solver->sorts_unique_table,
			    BTOR_REAL_ADDR_NODE (cur)->sort_id))
	{
	  m = btor_get_fun_model (e_solver, btor_simplify_exp (e_solver, cur));
	  if (m)
	    btor_add_ptr_hash_table (inputs, cur_fs)->data.as_ptr = (void *) m;
	}
      else
	btor_add_ptr_hash_table (inputs, cur_fs)->data.as_ptr =
	  (void *) btor_get_bv_model (e_solver,
		       btor_simplify_exp (e_solver, cur));
    }

  btor_init_hash_table_iterator (&it, deps);
  while (btor_has_next_hash_table_iterator (&it))
    btor_delete_ptr_hash_table (
      btor_next_data_hash_table_iterator (&it)->as_ptr);
  btor_delete_ptr_hash_table (deps);

  filter_inputs (mm, fs_uf, inputs);
  return inputs;
}

static bool
check_inputs (BtorPtrHashTable * inputs, BtorPtrHashTable * prev_inputs)
{
  BtorNode *cur;
  BtorHashTableIterator it;

  if (inputs->count != prev_inputs->count)
    return false;

  btor_init_node_hash_table_iterator (&it, inputs);
  while (btor_has_next_node_hash_table_iterator (&it))
    {
      cur = btor_next_node_hash_table_iterator (&it);
      if (!btor_get_ptr_hash_table (prev_inputs, cur))
	return false;
    }
  return true;
}
#endif

BtorNode *
mk_concrete_lambda_model (Btor *btor,
                          const BtorPtrHashTable *model,
                          BtorNode *best_match)

{
  assert (btor);
  assert (model);

  uint32_t i;
  BtorNode *uf, *res, *c, *p, *cond, *e_if, *e_else, *tmp, *eq, *ite, *args;
  BtorHashTableIterator it;
  BtorNodePtrStack params, consts;
  BtorBitVector *value;
  BtorBitVectorTuple *args_tuple;
  BtorSortId dsortid, cdsortid, funsortid;
  BtorSortUniqueTable *sorts;
  BtorSortIdStack tup_sorts;
  BtorPtrHashTable *static_rho;
  BtorMemMgr *mm;

  mm         = btor->mm;
  static_rho = btor_new_ptr_hash_table (mm, 0, 0);
  BTOR_INIT_STACK (params);
  BTOR_INIT_STACK (consts);
  BTOR_INIT_STACK (tup_sorts);

  sorts      = &btor->sorts_unique_table;
  args_tuple = model->first->key;
  value      = model->first->data.as_ptr;

  /* create params from domain sort */
  for (i = 0; i < args_tuple->arity; i++)
  {
    p = btor_param_exp (btor, args_tuple->bv[i]->width, 0);
    BTOR_PUSH_STACK (mm, params, p);
    BTOR_PUSH_STACK (mm, tup_sorts, p->sort_id);
  }

  dsortid =
      btor_tuple_sort (sorts, tup_sorts.start, BTOR_COUNT_STACK (tup_sorts));
  cdsortid  = btor_bitvec_sort (sorts, value->width);
  funsortid = btor_fun_sort (sorts, dsortid, cdsortid);
  btor_release_sort (sorts, dsortid);
  btor_release_sort (sorts, cdsortid);
  BTOR_RELEASE_STACK (mm, tup_sorts);

  if (best_match)
    uf = btor_copy_exp (btor, best_match);
  else
    uf = btor_uf_exp (btor, funsortid, 0);

  args = btor_args_exp (btor, params.start, BTOR_COUNT_STACK (params));
  assert (args->sort_id = btor_get_domain_fun_sort (sorts, uf->sort_id));
  e_else = btor_apply_exp (btor, uf, args);
  assert (BTOR_REAL_ADDR_NODE (e_else)->sort_id
          == btor_get_codomain_fun_sort (sorts, uf->sort_id));
  btor_release_exp (btor, args);
  btor_release_exp (btor, uf);

  /* generate ITEs */
  ite = 0;
  res = 0;
  btor_init_hash_table_iterator (&it, (BtorPtrHashTable *) model);
  while (btor_has_next_hash_table_iterator (&it))
  {
    value      = (BtorBitVector *) it.bucket->data.as_ptr;
    args_tuple = btor_next_hash_table_iterator (&it);

    /* create condition */
    assert (btor_get_fun_arity (btor, uf) == args_tuple->arity);
    assert (BTOR_EMPTY_STACK (consts));
    assert (BTOR_COUNT_STACK (params) == args_tuple->arity);
    for (i = 0; i < args_tuple->arity; i++)
    {
      c = btor_const_exp (btor, args_tuple->bv[i]);
      assert (BTOR_REAL_ADDR_NODE (c)->sort_id
              == BTOR_PEEK_STACK (params, i)->sort_id);
      BTOR_PUSH_STACK (btor->mm, consts, c);
    }

    assert (!BTOR_EMPTY_STACK (params));
    assert (BTOR_COUNT_STACK (params) == BTOR_COUNT_STACK (consts));
    cond = btor_eq_exp (
        btor, BTOR_PEEK_STACK (params, 0), BTOR_PEEK_STACK (consts, 0));
    for (i = 1; i < BTOR_COUNT_STACK (params); i++)
    {
      eq = btor_eq_exp (
          btor, BTOR_PEEK_STACK (params, i), BTOR_PEEK_STACK (consts, i));
      tmp = btor_and_exp (btor, cond, eq);
      btor_release_exp (btor, cond);
      btor_release_exp (btor, eq);
      cond = tmp;
    }

    /* args for static_rho */
    args = btor_args_exp (btor, consts.start, BTOR_COUNT_STACK (consts));

    while (!BTOR_EMPTY_STACK (consts))
      btor_release_exp (btor, BTOR_POP_STACK (consts));

    /* create ITE */
    e_if = btor_const_exp (btor, value);
    ite  = btor_cond_exp (btor, cond, e_if, e_else);

    /* add to static rho */
    btor_add_ptr_hash_table (static_rho, args)->data.as_ptr =
        btor_copy_exp (btor, e_if);

    btor_release_exp (btor, cond);
    btor_release_exp (btor, e_if);
    btor_release_exp (btor, e_else);
    e_else = ite;
  }

  assert (ite);
  if (ite) /* get rid of compiler warning */
  {
    res = btor_fun_exp (btor, params.start, BTOR_COUNT_STACK (params), ite);
    btor_release_exp (btor, ite);
  }
  assert (res->sort_id == funsortid);
  btor_release_sort (sorts, funsortid);

  while (!BTOR_EMPTY_STACK (params))
    btor_release_exp (btor, BTOR_POP_STACK (params));
  BTOR_RELEASE_STACK (btor->mm, params);
  BTOR_RELEASE_STACK (btor->mm, consts);

  /* res already exists */
  if (((BtorLambdaNode *) res)->static_rho)
  {
    btor_init_node_hash_table_iterator (&it, static_rho);
    while (btor_has_next_node_hash_table_iterator (&it))
    {
      btor_release_exp (btor, it.bucket->data.as_ptr);
      btor_release_exp (btor, btor_next_node_hash_table_iterator (&it));
    }
    btor_delete_ptr_hash_table (static_rho);
  }
  else
    ((BtorLambdaNode *) res)->static_rho = static_rho;
  return res;
}

/*------------------------------------------------------------------------*/

static BtorEFSolver *
clone_ef_solver (Btor *clone, Btor *btor, BtorNodeMap *exp_map)
{
  (void) clone;
  (void) btor;
  (void) exp_map;
  return 0;
}

static void
delete_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  Btor *btor;
  btor = slv->btor;
  BTOR_DELETE (btor->mm, slv);
  btor->slv = 0;
}

#if 0
static BtorNodeMap *
synthesize_model (BtorEFSolver * slv, BtorEFGroundSolvers * gslv)
{
  bool opt_synth_fun, found_model;
  uint32_t max_level;
  BtorNodeMap *model;
  Btor *e_solver, *f_solver;
  BtorNode *e_uf, *e_uf_fs, *synth_fun, *best_match;
  BtorNode *prev_synth_fun;
  BtorNodeMapIterator it;
  const BtorBitVector *bv;
  const BtorPtrHashTable *uf_model;
  BtorPtrHashTable *inputs, *synth_funs, *synth_inputs;
  BtorPtrHashBucket *b, *bb;
  BtorNodePtrStack matches;

  e_solver = gslv->exists;
  f_solver = gslv->forall;
  synth_funs = gslv->forall_synth_funs;
  synth_inputs = gslv->forall_synth_inputs;
  model = btor_new_node_map (f_solver);
  opt_synth_fun = btor_get_opt (f_solver, BTOR_OPT_EF_SYNTH) == 1;
  BTOR_INIT_STACK (matches);

  /* generate model for exists vars/ufs */
  assert (e_solver->last_sat_result == BTOR_RESULT_SAT);
  e_solver->slv->api.generate_model (e_solver->slv, false, false);

  /* map existential variables to their resp. assignment */
  btor_init_node_map_iterator (&it, gslv->exists_evars);
  btor_queue_node_map_iterator (&it, gslv->exists_ufs);
  while (btor_has_next_node_map_iterator (&it))
    {
      e_uf_fs = it.it.bucket->data.as_ptr;
      e_uf = btor_next_node_map_iterator (&it);

      if (btor_is_bitvec_sort (&e_solver->sorts_unique_table,
			       BTOR_REAL_ADDR_NODE (e_uf)->sort_id))
	{
	  bv = btor_get_bv_model (e_solver,
				  btor_simplify_exp (e_solver, e_uf));
#ifdef PRINT_DBG
	  printf ("exists %s := ", node2string (e_uf));
	  btor_print_bv (bv);
#endif
	  assert (!btor_is_proxy_node (e_uf_fs));
	  synth_fun = btor_const_exp (f_solver, (BtorBitVector *) bv);
	}
      /* map skolem functions to resp. synthesized functions */
      else
	{
	  assert (btor_is_fun_sort (&e_solver->sorts_unique_table,
				    BTOR_REAL_ADDR_NODE (e_uf)->sort_id));
	  assert (btor_is_fun_sort (&f_solver->sorts_unique_table,
				    BTOR_REAL_ADDR_NODE (e_uf_fs)->sort_id));

	  uf_model = btor_get_fun_model (e_solver, e_uf);

	  if (!uf_model) continue;
#ifdef PRINT_DBG
	  printf ("exists %s\n", node2string (e_uf));
	  BtorHashTableIterator mit;
	  BtorBitVectorTuple *tup;
	  uint32_t i;
	  btor_init_hash_table_iterator (&mit, uf_model);
	  while (btor_has_next_hash_table_iterator (&mit))
	    {
	      bv = mit.bucket->data.as_ptr;
	      tup = btor_next_hash_table_iterator (&mit);

	      for (i = 0; i < tup->arity; i++)
		{
		  printf ("a ");
		  btor_print_bv (tup->bv[i]);
		}
	      printf ("r ");
	      btor_print_bv (bv);
	    }
#endif
	  best_match = 0;
	  b = btor_get_ptr_hash_table (synth_funs, e_uf_fs);
	  if (opt_synth_fun)
	    {
	      bb = btor_get_ptr_hash_table (synth_inputs, e_uf_fs);
	      inputs = prepare_inputs (gslv, e_uf_fs, model);

	      if (b && bb && check_inputs (inputs, bb->data.as_ptr))
		prev_synth_fun = b->data.as_ptr;
	      else
		prev_synth_fun = 0;

	      /* last synthesize step failed and no candidate was found */
	      if (b && !b->data.as_ptr)
		max_level = 2;
	      else
		max_level = 0;

	      found_model = btor_synthesize_fun (f_solver, e_uf_fs, uf_model,
						 prev_synth_fun,
						 inputs,
						 100000,
						 max_level,
						 &matches);

	      if (found_model)
		{
		  assert (BTOR_COUNT_STACK (matches) == 1);
		  synth_fun =
		    btor_copy_exp (f_solver, BTOR_TOP_STACK (matches));
		}
	      else
		{
		  synth_fun = 0;
		  best_match =
		    btor_copy_exp (f_solver, BTOR_TOP_STACK (matches));
		}

	      while (!BTOR_EMPTY_STACK (matches))
		btor_release_exp (f_solver, BTOR_POP_STACK (matches));

	      assert (!synth_fun || e_uf_fs->sort_id == synth_fun->sort_id);
	      if (bb)
		btor_delete_ptr_hash_table (bb->data.as_ptr);
	      else
		bb = btor_add_ptr_hash_table (synth_inputs, e_uf_fs);
	      bb->data.as_ptr = inputs;
	    }
	  else
	    synth_fun = 0;

	  if (!b)
	    b = btor_add_ptr_hash_table (synth_funs, e_uf_fs);

	  if (!synth_fun)
	    {
	      slv->stats.synth_aborts++;
	      synth_fun = mk_concrete_lambda_model (
			      f_solver, uf_model, best_match);
	      if (best_match)
		btor_release_exp (f_solver, best_match);

	      if (b->data.as_ptr != 0)
		{
		  btor_release_exp (f_solver, b->data.as_ptr);
		  b->data.as_ptr = 0;
		}
	    }
	  else
	    {
	      if (b->data.as_ptr != synth_fun)
		slv->stats.synth_funs++;
	      else
		slv->stats.synth_funs_reused++;

	      /* save synthesized function for next iteration (if changed) */
	      if (b->data.as_ptr != synth_fun)
		{
		  if (b->data.as_ptr)
		    btor_release_exp (f_solver, b->data.as_ptr);
		  b->data.as_ptr = btor_copy_exp (f_solver, synth_fun);
		}
	    }
	}
      assert (BTOR_IS_REGULAR_NODE (e_uf_fs));
      assert (e_uf_fs->sort_id == BTOR_REAL_ADDR_NODE (synth_fun)->sort_id);
      btor_map_node (model, e_uf_fs, synth_fun);
//      printf ("CANDIDATE FOUND for %s\n", node2string (e_uf_fs));
//      btor_dump_smt2_node (f_solver, stdout, synth_fun, -1);
    }
  BTOR_RELEASE_STACK (f_solver->mm, matches);

  return model;
}
#endif

static bool
same_model (BtorPtrHashTable *m_evar, BtorPtrHashTable *m_input)
{
  bool found;
  uint32_t i, arity;
  BtorHashTableIterator it;
  BtorBitVectorTuple *t0, *t1;
  BtorMemMgr *mm;

  if (m_evar->count != m_input->count) return false;

  t0 = m_evar->first->key;
  t1 = m_input->first->key;

  /* 'evar' is more outer to 'input' in the quantifier prefix */
  if (t0->arity < t1->arity) return false;

  mm    = m_evar->mm;
  arity = t1->arity;
  btor_init_hash_table_iterator (&it, m_evar);
  while (btor_has_next_hash_table_iterator (&it))
  {
    t0 = btor_next_hash_table_iterator (&it);
    t1 = btor_new_bv_tuple (mm, arity);
    for (i = 0; i < arity; i++) btor_add_to_bv_tuple (mm, t1, t0->bv[i], i);
    found = btor_get_ptr_hash_table (m_input, t1) != 0;
    btor_free_bv_tuple (mm, t1);
    if (!found) return false;
  }
  return true;
}

static bool
check_prefix (BtorEFGroundSolvers *gslv, BtorNode *evar, BtorNode *input)
{
  assert (btor_param_is_exists_var (evar));
  assert (btor_param_is_exists_var (input));

  Btor *btor;
  BtorArgsIterator it_evar, it_input;
  BtorNode *deps_evar, *deps_input, *a_evar, *a_input;

  btor       = gslv->forall;
  deps_evar  = btor_mapped_node (gslv->forall_evar_deps, evar);
  deps_input = btor_mapped_node (gslv->forall_evar_deps, input);
  assert (deps_evar);
  if (!deps_input) return true;
  assert (btor_is_args_node (deps_evar));
  assert (btor_is_args_node (deps_input));

  if (btor_get_args_arity (btor, deps_evar)
      < btor_get_args_arity (btor, deps_input))
    return false;

  btor_init_args_iterator (&it_evar, deps_evar);
  btor_init_args_iterator (&it_input, deps_input);
  while (btor_has_next_args_iterator (&it_evar))
  {
    if (!btor_has_next_args_iterator (&it_input)) break;
    a_evar  = btor_next_args_iterator (&it_evar);
    a_input = btor_next_args_iterator (&it_input);
    if (a_evar != a_input) return false;
  }
  return true;
}

static void
collect_inputs (BtorEFGroundSolvers *gslv,
                BtorNode *root,
                BtorNode *uf,
                BtorPtrHashTable *inputs,
                BtorIntHashTable *mark)
{
  uint32_t i;
  Btor *e_solver, *f_solver;
  BtorNodePtrStack visit;
  BtorMemMgr *mm;
  BtorNode *cur, *mapped, *mapped_uf;
  BtorPtrHashBucket *b;
  BtorPtrHashTable *m, *model;

  e_solver  = gslv->exists;
  f_solver  = gslv->forall;
  mm        = f_solver->mm;
  mapped_uf = btor_mapped_node (gslv->exists_evars, uf);
  model     = btor_get_fun_model (e_solver, uf);
  assert (mapped_uf);
  BTOR_INIT_STACK (visit);
  BTOR_PUSH_STACK (mm, visit, root);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_contains_int_hash_table (mark, cur->id) || btor_is_args_node (cur))
      continue;

    btor_add_int_hash_table (mark, cur->id);

    if (cur->arity == 0 && cur != uf)
    {
      assert (!btor_get_ptr_hash_table (inputs, cur));
      mapped = btor_mapped_node (gslv->exists_evars, cur);
      if (mapped && check_prefix (gslv, mapped_uf, mapped))
      {
        if (btor_mapped_node (gslv->forall_evar_deps, mapped))
        {
          m = btor_get_fun_model (e_solver, cur);
          if (same_model (model, m))
          {
            b              = btor_add_ptr_hash_table (inputs, mapped);
            b->data.flag   = true;
            b->data.as_ptr = m;  // btor_get_fun_model (e_solver, cur);
          }
        }
        else
        {
          b              = btor_add_ptr_hash_table (inputs, mapped);
          b->data.as_ptr = btor_get_bv_model (e_solver, cur);
        }
        //	      printf ("  input: %s (%s)\n", node2string (mapped),
        // node2string (mapped_uf)); 	      printf ("  %s %s\n", node2string
        //(btor_param_get_binder (mapped_uf)), node2string
        //(btor_param_get_binder (mapped)));
      }
      continue;
    }

    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (mm, visit, cur->e[i]);
  }
  BTOR_RELEASE_STACK (mm, visit);
}

static BtorPtrHashTable *
find_inputs (BtorEFGroundSolvers *gslv,
             BtorNode *uf,
             const BtorPtrHashTable *model)
{
  uint32_t i;
  Btor *e_solver, *f_solver;
  BtorHashTableIterator it;
  BtorNode *cur;
  BtorNodePtrStack visit;
  BtorIntHashTable *mark, *mark_in;
  BtorMemMgr *mm;
  BtorBitVector *bv;
  BtorPtrHashTable *sigs, *inputs;
  //  printf ("find inputs: %s\n", node2string (uf));

  e_solver = gslv->exists;
  f_solver = gslv->forall;
  mm       = f_solver->mm;
  mark     = btor_new_int_hash_table (mm);
  mark_in  = btor_new_int_hash_table (mm);
  sigs     = btor_new_ptr_hash_table (
      mm, (BtorHashPtr) btor_hash_bv, (BtorCmpPtr) btor_compare_bv);
  inputs = btor_new_ptr_hash_table (mm, 0, 0);
  BTOR_INIT_STACK (visit);
  btor_init_node_hash_table_iterator (&it, e_solver->synthesized_constraints);
  btor_queue_node_hash_table_iterator (&it, e_solver->assumptions);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    BTOR_PUSH_STACK (mm, visit, cur);
  }

  //  printf ("  signatures:\n");
  btor_init_hash_table_iterator (&it, model);
  while (btor_has_next_hash_table_iterator (&it))
  {
    bv = it.bucket->data.as_ptr;
    (void) btor_next_hash_table_iterator (&it);
    if (!btor_get_ptr_hash_table (sigs, bv))
    {
      //	  printf ("    ");
      //	  btor_print_bv (bv);
      btor_add_ptr_hash_table (sigs, bv);
    }
  }

  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_contains_int_hash_table (mark, cur->id)) continue;

    btor_add_int_hash_table (mark, cur->id);

    bv = btor_get_assignment_bv (mm, cur, 1);
    if (btor_get_ptr_hash_table (sigs, bv))
      collect_inputs (gslv, cur, uf, inputs, mark_in);
    btor_free_bv (mm, bv);

    if (btor_is_apply_node (cur)) continue;

    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (mm, visit, cur->e[i]);
  }

  BTOR_RELEASE_STACK (mm, visit);
  btor_delete_int_hash_table (mark);
  btor_delete_int_hash_table (mark_in);
  btor_delete_ptr_hash_table (sigs);
  if (inputs->count == 0)
  {
    btor_delete_ptr_hash_table (inputs);
    inputs = 0;
  }
  return inputs;
}

#if 0
static void
check_inputs_cycle (BtorMemMgr * mm, BtorPtrHashTable * inputs)
{
  BtorNode *evar, *cur, *e;
  BtorHashTableIterator it, it2;
  BtorPtrHashTable *in;
  BtorNodePtrStack visit;
  BtorPtrHashBucket *b;
  BtorIntHashTable *cache = 0;

  BTOR_INIT_STACK (visit);
  btor_init_node_hash_table_iterator (&it, inputs);
  while (btor_has_next_node_hash_table_iterator (&it))
    {
      evar = btor_next_node_hash_table_iterator (&it);
      cur = evar;
      cache = btor_new_int_hash_table (mm);
//      printf ("****** %s\n", node2string (cur));
      goto PUSH_INPUTS;
      while (!BTOR_EMPTY_STACK (visit))
	{
	  cur = BTOR_POP_STACK (visit);
	  assert (BTOR_REAL_ADDR_NODE (cur));

	  if (btor_contains_int_hash_table (cache, cur->id))
	    continue;

	  btor_add_int_hash_table (cache, cur->id);

PUSH_INPUTS:
	  if ((b = btor_get_ptr_hash_table (inputs, cur)) && b->data.as_ptr)
	    {
	      btor_init_node_hash_table_iterator (&it2, b->data.as_ptr);
	      while (btor_has_next_node_hash_table_iterator (&it2))
		{
		  e = btor_next_node_hash_table_iterator (&it2);
		  if (e == evar)
		    {
		      printf ("CYCLE: remove %s (%s)\n", node2string (e), node2string (cur));
		      btor_remove_ptr_hash_table (b->data.as_ptr, e, 0, 0);
		      continue;
		    }
		  BTOR_PUSH_STACK (mm, visit, e);
		}
	    }

	}
      btor_delete_int_hash_table (cache);
    }
  BTOR_RELEASE_STACK (mm, visit);
}
#endif

static void
check_input_cycle (BtorMemMgr *mm, BtorNode *evar, BtorPtrHashTable *inputs)
{
  BtorNode *cur, *e, *cur_in;
  BtorHashTableIterator it, it2;
  BtorPtrHashTable *in, *evar_in;
  BtorNodePtrStack visit;
  BtorPtrHashBucket *b;
  BtorIntHashTable *cache = 0;

  b = btor_get_ptr_hash_table (inputs, evar);
  if (!b) return;
  evar_in = b->data.as_ptr;

  if (!evar_in) return;

  BTOR_INIT_STACK (visit);
  cur = evar;
  //      printf ("****** %s\n", node2string (cur));
  btor_init_node_hash_table_iterator (&it, evar_in);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur_in = btor_next_node_hash_table_iterator (&it);

    cache = btor_new_int_hash_table (mm);
    BTOR_PUSH_STACK (mm, visit, cur_in);
    while (!BTOR_EMPTY_STACK (visit))
    {
      cur = BTOR_POP_STACK (visit);
      assert (BTOR_REAL_ADDR_NODE (cur));

      if (btor_contains_int_hash_table (cache, cur->id)) continue;

      btor_add_int_hash_table (cache, cur->id);
      if ((b = btor_get_ptr_hash_table (inputs, cur)) && b->data.as_ptr)
      {
        btor_init_node_hash_table_iterator (&it2, b->data.as_ptr);
        while (btor_has_next_node_hash_table_iterator (&it2))
        {
          e = btor_next_node_hash_table_iterator (&it2);
          if (e == evar)
          {
            //		      printf ("CYCLE: remove %s\n", node2string
            //(cur_in));
            btor_remove_ptr_hash_table (evar_in, cur_in, 0, 0);
            goto NEXT_IN;
          }
          BTOR_PUSH_STACK (mm, visit, e);
        }
      }
    }
  NEXT_IN:
    btor_delete_int_hash_table (cache);
  }
  BTOR_RELEASE_STACK (mm, visit);

#if 0
  btor_init_node_hash_table_iterator (&it, evar_in);
  while (btor_has_next_node_hash_table_iterator (&it))
    printf ("  input: %s\n", node2string (btor_next_node_hash_table_iterator (&it)));
#endif
}

static void
check_inputs_used (BtorMemMgr *mm,
                   BtorNode *synth_fun,
                   BtorPtrHashTable *inputs)
{
  uint32_t i;
  BtorPtrHashBucket *b;
  BtorNodePtrStack visit;
  BtorNode *cur;
  BtorIntHashTable *cache;
  BtorHashTableIterator it;

  if (!inputs) return;

  cache = btor_new_int_hash_table (mm);
  BTOR_INIT_STACK (visit);
  BTOR_PUSH_STACK (mm, visit, synth_fun);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur = BTOR_REAL_ADDR_NODE (BTOR_POP_STACK (visit));

    if (btor_contains_int_hash_table (cache, cur->id) || !cur->parameterized)
      continue;

    btor_add_int_hash_table (cache, cur->id);
    for (i = 0; i < cur->arity; i++) BTOR_PUSH_STACK (mm, visit, cur->e[i]);
  }
  BTOR_RELEASE_STACK (mm, visit);

  btor_init_node_hash_table_iterator (&it, inputs);
  while (btor_has_next_node_hash_table_iterator (&it))
  {
    cur = btor_next_node_hash_table_iterator (&it);
    assert (BTOR_IS_REGULAR_NODE (cur));

    if (!btor_contains_int_hash_table (cache, cur->id))
    {
      //	  printf ("remove unused: %s\n", node2string (cur));
      btor_remove_ptr_hash_table (inputs, cur, 0, 0);
    }
  }
  btor_delete_int_hash_table (cache);
}

static int
cmp_evars (const void *p0, const void *p1)
{
  Btor *btor;
  BtorNode *e0, *e1;
  const char *s0, *s1;

  e0   = *(BtorNode **) p0;
  e1   = *(BtorNode **) p1;
  btor = e0->btor;
  s0   = btor_get_symbol_exp (btor, e0);
  s1   = btor_get_symbol_exp (btor, e1);
  assert (s0);
  assert (s1);

  return strcmp (s0, s1);
}

static const BtorPtrHashTable *
update_evar_model (BtorEFGroundSolvers *gslv,
                   BtorNode *evar,
                   const BtorPtrHashTable *model)
{
  BtorPtrHashBucket *b;
  BtorPtrHashTable *evar_model, *evar_models;
  BtorMemMgr *mm;
  BtorBitVector *bv;
  BtorBitVectorTuple *bv_tup;
  BtorHashTableIterator it;

  mm          = gslv->exists->mm;
  evar_models = gslv->exists_evar_models;

  if (!(b = btor_get_ptr_hash_table (evar_models, evar)))
  {
    b = btor_add_ptr_hash_table (evar_models, evar);
    b->data.as_ptr =
        btor_new_ptr_hash_table (mm,
                                 (BtorHashPtr) btor_hash_bv_tuple,
                                 (BtorCmpPtr) btor_compare_bv_tuple);
  }
  evar_model = b->data.as_ptr;

  btor_init_hash_table_iterator (&it, model);
  while (btor_has_next_hash_table_iterator (&it))
  {
    bv     = it.bucket->data.as_ptr;
    bv_tup = btor_next_hash_table_iterator (&it);
    if (!btor_get_ptr_hash_table (evar_model, bv_tup))
    {
      b = btor_add_ptr_hash_table (evar_model, btor_copy_bv_tuple (mm, bv_tup));
      b->data.as_ptr = btor_copy_bv (mm, bv);
    }
  }
  return evar_model;
}

static BtorPtrHashTable *
synthesize_model (BtorEFSolver *slv, BtorEFGroundSolvers *gslv)
{
  uint32_t i, limit, level;
  bool opt_synth_fun, found_model;
  BtorPtrHashTable *model, *prev_model, *inputs, *in;
  Btor *e_solver, *f_solver;
  BtorNode *e_uf, *e_uf_fs, *m, *prev_synth_fun, *pfun;
  BtorNodeMapIterator it;
  BtorHashTableIterator hit;
  const BtorBitVector *bv;
  BtorBitVectorTuple *bv_tup;
  const BtorPtrHashTable *uf_model;
  BtorPtrHashTable *evar_models, *evar_model;
  BtorNodePtrStack *matches, evars;
  BtorSynthResult *synth_res, *prev_synth_res;
  BtorPtrHashBucket *b;
  BtorMemMgr *mm;

  e_solver      = gslv->exists;
  f_solver      = gslv->forall;
  mm            = f_solver->mm;
  prev_model    = gslv->forall_cur_model;
  evar_models   = gslv->exists_evar_models;
  model         = btor_new_ptr_hash_table (mm, 0, 0);
  opt_synth_fun = btor_get_opt (f_solver, BTOR_OPT_EF_SYNTH) == 1;
  inputs        = btor_new_ptr_hash_table (mm, 0, 0);

  /* generate model for exists vars/ufs */
  assert (e_solver->last_sat_result == BTOR_RESULT_SAT);
  e_solver->slv->api.generate_model (e_solver->slv, false, false);

  /* map existential variables to their resp. assignment */
  // TODO: evar ordering?
#if 0
  BTOR_INIT_STACK (evars);
  // TODO: UFs not considered here yet
  btor_init_node_map_iterator (&it, gslv->forall_evars);
  while (btor_has_next_node_map_iterator (&it))
    BTOR_PUSH_STACK (mm, evars, btor_next_node_map_iterator (&it));

//  qsort (evars.start, BTOR_COUNT_STACK (evars), sizeof (BtorNode *), cmp_evars);
  
  for (i = 0; i < BTOR_COUNT_STACK (evars); i++)
    {
      e_uf_fs = BTOR_PEEK_STACK (evars, i);
      e_uf = btor_mapped_node (gslv->forall_evars, e_uf_fs);
#else
  btor_init_node_map_iterator (&it, gslv->exists_evars);
  btor_queue_node_map_iterator (&it, gslv->exists_ufs);
  while (btor_has_next_node_map_iterator (&it))
  {
    e_uf_fs = it.it.bucket->data.as_ptr;
    e_uf    = btor_next_node_map_iterator (&it);
#endif
  assert (btor_is_uf_node (e_uf_fs) || btor_param_is_exists_var (e_uf_fs));

  /* map skolem functions to resp. synthesized functions */
  if (btor_mapped_node (gslv->forall_evar_deps, e_uf_fs)
      || btor_is_uf_node (e_uf_fs))
  {
    /* 'e_uf_fs' is an existential variable */
    assert (btor_is_fun_sort (&e_solver->sorts_unique_table,
                              BTOR_REAL_ADDR_NODE (e_uf)->sort_id));
    assert (btor_is_uf_node (e_uf));

    uf_model = btor_get_fun_model (e_solver, e_uf);

    if (!uf_model) continue;

//	  evar_model = update_evar_model (gslv, e_uf, uf_model);
#if 0
	  printf ("exists %s\n", node2string (e_uf));
	  BtorHashTableIterator mit;
	  BtorBitVectorTuple *tup;
	  uint32_t i;
	  btor_init_hash_table_iterator (&mit, uf_model);
	  while (btor_has_next_hash_table_iterator (&mit))
	    {
	      bv = mit.bucket->data.as_ptr;
	      tup = btor_next_hash_table_iterator (&mit);

	      char *s;
	      for (i = 0; i < tup->arity; i++)
		{
		  s = btor_bv_to_char_bv (mm, tup->bv[i]);
		  printf ("a %s %zu\n", s, btor_bv_to_uint64_bv (tup->bv[i]));
		  btor_freestr (mm, s);
		}
	      s = btor_bv_to_char_bv (mm, bv);
	      printf ("r %s %zu\n", s, btor_bv_to_uint64_bv (bv));
	      btor_freestr (mm, s);
	    }
#endif
    synth_res      = btor_new_synth_result (mm);
    prev_synth_res = 0;
    prev_synth_fun = 0;
    found_model    = false;
    if (opt_synth_fun)
    {
      level = 0;
      limit = 10000;

      /* check previously synthesized function */
      if (prev_model && (b = btor_get_ptr_hash_table (prev_model, e_uf_fs)))
      {
        prev_synth_res = b->data.as_ptr;
        assert (prev_synth_res);
        if (prev_synth_res->full
            && BTOR_COUNT_STACK (prev_synth_res->exps) == 1)
        {
          //		      printf ("try to reuse\n");
          if (prev_synth_res->type == BTOR_SYNTH_TYPE_SK_UF)
            prev_synth_fun = BTOR_TOP_STACK (prev_synth_res->exps);
          else
          {
            assert (prev_synth_res->type == BTOR_SYNTH_TYPE_UF);
            prev_synth_fun = prev_synth_res->value;
          }
        }
        /* we did not find expressions that cover all input/output
         * pairs previously, increase previous limit */
        else
        {
          limit = prev_synth_res->limit * 2;
          //		      printf ("INCREASE LIMIT TO %u\n", limit);
          //		      level = 2;
        }
      }
      b = btor_add_ptr_hash_table (inputs, e_uf_fs);
      if (!btor_is_uf_node (e_uf_fs))
      {
        assert (btor_param_is_exists_var (e_uf_fs));
        in = find_inputs (gslv, e_uf, uf_model);
      }
      else
        in = 0;
      b->data.as_ptr = in;
      check_input_cycle (mm, e_uf_fs, inputs);
      found_model = btor_synthesize_fun (f_solver,
                                         uf_model,
                                         prev_synth_fun,
                                         in,
                                         limit,
                                         level,
                                         &synth_res->exps);
      assert (!found_model || !BTOR_EMPTY_STACK (synth_res->exps));
      synth_res->limit = limit;
      //	      if (found_model)
      //		btor_dump_smt2_node (f_solver, stdout, BTOR_TOP_STACK
      //(synth_res->exps), -1); 	      if (found_model)
      //		printf ("FOUND MODEL\n");
    }

    if (btor_is_uf_node (e_uf_fs))
    {
      synth_res->type = BTOR_SYNTH_TYPE_UF;
      if (!found_model)  // || BTOR_COUNT_STACK (synth_res->exps) > 1)
      {
        synth_res->value = mk_concrete_lambda_model (
            f_solver, uf_model, 0);  // BTOR_TOP_STACK (synth_res->exps));
        synth_res->full = false;
      }
      else
      {
        assert (BTOR_COUNT_STACK (synth_res->exps) == 1);
        synth_res->full  = true;
        synth_res->value = BTOR_TOP_STACK (synth_res->exps);
        BTOR_RESET_STACK (synth_res->exps);
      }
    }
    else
    {
      assert (btor_param_is_exists_var (e_uf_fs));
      synth_res->type = BTOR_SYNTH_TYPE_SK_UF;
      synth_res->full = found_model;

      // TODO: if partial and multiple matches, then use best match for concrete
      // model und release other exps on stack and reset stack s.t. only
      // concrete model is on the stack
      if (!found_model)
      {
        //		  printf ("PARTIAL %u\n", BTOR_COUNT_STACK
        //(synth_res->exps));
        /* select base case for concrete model */
        switch (BTOR_COUNT_STACK (synth_res->exps))
        {
          case 0: pfun = 0; break;
          case 1: pfun = BTOR_POP_STACK (synth_res->exps); break;
          default: pfun = BTOR_TOP_STACK (synth_res->exps);
        }
        m = mk_concrete_lambda_model (f_solver, uf_model, pfun);
        if (BTOR_EMPTY_STACK (synth_res->exps) && pfun)
          btor_release_exp (f_solver, pfun);
        BTOR_PUSH_STACK (mm, synth_res->exps, m);
      }
      else if (BTOR_COUNT_STACK (synth_res->exps))
      {
        assert (BTOR_COUNT_STACK (synth_res->exps) == 1);
        //		  printf ("check inputs used: %s\n", node2string
        //(e_uf_fs));
        check_inputs_used (mm, BTOR_TOP_STACK (synth_res->exps), in);
      }

#if 0
	      else if (BTOR_COUNT_STACK (synth_res->exps) == 1)
		{
		btor_dump_smt2_node (f_solver, stdout, BTOR_TOP_STACK (synth_res->exps), -1);
		}
#endif
    }
    btor_add_ptr_hash_table (model, e_uf_fs)->data.as_ptr = synth_res;
  }
  else
  {
    assert (btor_is_bitvec_sort (&e_solver->sorts_unique_table,
                                 BTOR_REAL_ADDR_NODE (e_uf)->sort_id));
    bv = btor_get_bv_model (e_solver, btor_simplify_exp (e_solver, e_uf));
#ifdef PRINT_DBG
    printf ("exists %s := ", node2string (e_uf));
    btor_print_bv (bv);
#endif
    synth_res        = btor_new_synth_result (mm);
    synth_res->type  = BTOR_SYNTH_TYPE_SK_VAR;
    synth_res->value = btor_const_exp (f_solver, (BtorBitVector *) bv);
    btor_add_ptr_hash_table (model, e_uf_fs)->data.as_ptr = synth_res;
  }
}

btor_init_node_hash_table_iterator (&hit, inputs);
while (btor_has_next_node_hash_table_iterator (&hit))
{
  if (hit.bucket->data.as_ptr)
    btor_delete_ptr_hash_table (hit.bucket->data.as_ptr);
  (void) btor_next_node_hash_table_iterator (&hit);
}
btor_delete_ptr_hash_table (inputs);
#if 0
  BTOR_RELEASE_STACK (mm, evars);
#endif
return model;
}

static void
update_formula (BtorEFGroundSolvers *gslv)
{
  Btor *forall;
  BtorNode *f, *g;

  forall = gslv->forall;
  f      = gslv->forall_formula;
  /* update formula if changed via simplifications */
  if (btor_is_proxy_node (f))
  {
    g = btor_copy_exp (forall, btor_simplify_exp (forall, f));
    btor_release_exp (forall, f);
    gslv->forall_formula = g;
  }
}

static BtorNode *
substitute_evar (Btor *btor, BtorNode *root, BtorNode *evar, BtorNode *subst)
{
  int32_t i;
  size_t j;
  BtorMemMgr *mm;
  BtorNodePtrStack visit, args;
  BtorNode *cur, *real_cur, *result, **e;
  BtorIntHashTable *mark;
  BtorHashTableData *d;

  mm   = btor->mm;
  mark = btor_new_int_hash_map (mm);

  BTOR_INIT_STACK (visit);
  BTOR_INIT_STACK (args);

  BTOR_PUSH_STACK (mm, visit, root);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur      = BTOR_POP_STACK (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    d = btor_get_int_hash_map (mark, real_cur->id);
    if (!d)
    {
      btor_add_int_hash_map (mark, real_cur->id);
      BTOR_PUSH_STACK (mm, visit, cur);
      for (i = real_cur->arity - 1; i >= 0; i--)
        BTOR_PUSH_STACK (mm, visit, real_cur->e[i]);
    }
    else if (d->as_ptr == 0)
    {
      assert (!btor_is_quantifier_node (real_cur));
      assert (real_cur->arity <= BTOR_COUNT_STACK (args));
      args.top -= real_cur->arity;
      e = args.top;

      if (real_cur->arity == 0)
      {
        /* substitute evar */
        if (real_cur == evar)
        {
          assert (btor_param_is_exists_var (real_cur));
          result = btor_copy_exp (btor, subst);
        }
        else
          result = btor_copy_exp (btor, real_cur);
      }
      else if (btor_is_slice_node (real_cur))
      {
        result = btor_slice_exp (btor,
                                 e[0],
                                 btor_slice_get_upper (real_cur),
                                 btor_slice_get_lower (real_cur));
      }
      else
        result = btor_create_exp (btor, real_cur->kind, e, real_cur->arity);

      for (i = 0; i < real_cur->arity; i++) btor_release_exp (btor, e[i]);

      d->as_ptr = btor_copy_exp (btor, result);

    PUSH_RESULT:
      BTOR_PUSH_STACK (mm, args, BTOR_COND_INVERT_NODE (cur, result));
    }
    else
    {
      assert (d->as_ptr);
      result = btor_copy_exp (btor, d->as_ptr);
      goto PUSH_RESULT;
    }
  }
  assert (BTOR_COUNT_STACK (args) == 1);
  result = BTOR_POP_STACK (args);

  BTOR_RELEASE_STACK (mm, visit);
  BTOR_RELEASE_STACK (mm, args);

  for (j = 0; j < mark->size; j++)
  {
    if (!mark->keys[j]) continue;
    assert (mark->data[j].as_ptr);
    btor_release_exp (btor, mark->data[j].as_ptr);
  }
  btor_delete_int_hash_map (mark);

  assert (!BTOR_REAL_ADDR_NODE (result)->quantifier_below);
  return result;
}

static BtorNode *
expand_evars (Btor *btor, BtorNode *exp, BtorPtrHashTable *model)
{
  assert (BTOR_IS_REGULAR_NODE (exp));
  assert (btor_is_lambda_node (exp));
  assert (exp->parameterized);
  assert (model);

  int32_t i;
  size_t j;
  BtorMemMgr *mm;
  BtorNodePtrStack visit, args, cleanup;
  BtorNode *cur, *real_cur, *result, **e, *tmp, *subst, *evar, *a, *app;
  BtorNode *fun, *l0, *l1;
  BtorIntHashTable *mark;
  BtorHashTableData *d;
  BtorPtrHashBucket *b;
  BtorSortId funsortid;
  BtorNodeMap *funs;
  BtorSynthResult *synth_res;
  BtorHashTableIterator it;
  BtorNodeIterator nit0, nit1;

  mm   = btor->mm;
  mark = btor_new_int_hash_map (mm);
  funs = btor_new_node_map (btor);

  BTOR_INIT_STACK (visit);
  BTOR_INIT_STACK (args);
  BTOR_INIT_STACK (cleanup);
  BTOR_PUSH_STACK (mm, visit, exp);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur      = BTOR_POP_STACK (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    d = btor_get_int_hash_map (mark, real_cur->id);
    if (!d)
    {
      if ((b = btor_get_ptr_hash_table (model, real_cur)))
      {
        synth_res = b->data.as_ptr;
        if (synth_res->type == BTOR_SYNTH_TYPE_SK_UF)
        {
          assert (BTOR_COUNT_STACK (synth_res->exps) == 1);
          tmp = BTOR_TOP_STACK (synth_res->exps);

          btor_init_lambda_iterator (&nit0, tmp);
          btor_init_lambda_iterator (&nit1, exp);
          while (btor_has_next_lambda_iterator (&nit0))
          {
            assert (btor_has_next_lambda_iterator (&nit1));
            l0 = btor_next_lambda_iterator (&nit0);
            l1 = btor_next_lambda_iterator (&nit1);
            btor_assign_param (btor, l0, l1->e[0]);
          }
          subst = btor_beta_reduce_full (btor, btor_binder_get_body (tmp));
          btor_unassign_params (btor, tmp);
          BTOR_PUSH_STACK (mm, cleanup, subst);
        }
        else
          subst = synth_res->value;
        BTOR_PUSH_STACK (mm, visit, BTOR_COND_INVERT_NODE (cur, subst));
      }
      else
      {
        btor_add_int_hash_map (mark, real_cur->id);
        BTOR_PUSH_STACK (mm, visit, cur);
        for (i = real_cur->arity - 1; i >= 0; i--)
          BTOR_PUSH_STACK (mm, visit, real_cur->e[i]);
      }
    }
    else if (d->as_ptr == 0)
    {
      assert (!btor_get_ptr_hash_table (model, real_cur));
      assert (real_cur->arity <= BTOR_COUNT_STACK (args));
      args.top -= real_cur->arity;
      e = args.top;

      if (real_cur->arity == 0)
      {
        if (btor_is_param_node (real_cur))
          result =
              btor_param_exp (btor, btor_get_exp_width (btor, real_cur), 0);
        else
          result = btor_copy_exp (btor, real_cur);
      }
      else if (btor_is_slice_node (real_cur))
      {
        result = btor_slice_exp (btor,
                                 e[0],
                                 btor_slice_get_upper (real_cur),
                                 btor_slice_get_lower (real_cur));
      }
      else
        result = btor_create_exp (btor, real_cur->kind, e, real_cur->arity);

      for (i = 0; i < real_cur->arity; i++) btor_release_exp (btor, e[i]);

      d->as_ptr = btor_copy_exp (btor, result);

    PUSH_RESULT:
      BTOR_PUSH_STACK (mm, args, BTOR_COND_INVERT_NODE (cur, result));
    }
    else
    {
      assert (d->as_ptr);
      result = btor_copy_exp (btor, d->as_ptr);
      goto PUSH_RESULT;
    }
  }
  assert (BTOR_COUNT_STACK (args) == 1);
  result = BTOR_POP_STACK (args);

  BTOR_RELEASE_STACK (mm, visit);
  BTOR_RELEASE_STACK (mm, args);

  while (!BTOR_EMPTY_STACK (cleanup))
    btor_release_exp (btor, BTOR_POP_STACK (cleanup));
  BTOR_RELEASE_STACK (mm, cleanup);

  for (j = 0; j < mark->size; j++)
  {
    if (!mark->data[j].as_ptr) continue;
    btor_release_exp (btor, mark->data[j].as_ptr);
  }
  btor_delete_int_hash_map (mark);
  btor_delete_node_map (funs);

  assert (!BTOR_REAL_ADDR_NODE (result)->quantifier_below);
  assert (!BTOR_REAL_ADDR_NODE (result)->parameterized);
  return result;
}

/* instantiate each universal variable with the resp. fresh bit vector variable
 * and replace existential variables with the synthesized model (may expand the
 * quantifier tree if more synthesized functions available).
 * 'model' maps existential variables to synthesized function models. */
static BtorNode *
instantiate_formula (BtorEFGroundSolvers *gslv, BtorPtrHashTable *model)
{
  assert (gslv);
  assert (!btor_is_proxy_node (gslv->forall_formula));

  int32_t i;
  size_t j;
  Btor *btor;
  BtorMemMgr *mm;
  BtorNodePtrStack visit, args;
  BtorNode *cur, *real_cur, *result, **e, *tmp, *subst, *evar, *a, *app;
  BtorNode *fun;
  BtorIntHashTable *mark;
  BtorHashTableData *d;
  BtorNodeMap *uvar_map, *skolem;
  BtorPtrHashBucket *b;
  BtorSortId funsortid;
  BtorNodeMap *deps, *funs;
  BtorSynthResult *synth_res;
  BtorHashTableIterator it;

  btor     = gslv->forall;
  mm       = btor->mm;
  mark     = btor_new_int_hash_map (mm);
  uvar_map = gslv->forall_uvars;
  deps     = gslv->forall_evar_deps;
  skolem   = gslv->forall_skolem;
  funs     = btor_new_node_map (btor);

  /* instantiate synthesized functions that still contain existential variables
   */
  if (model)
  {
    btor_init_node_hash_table_iterator (&it, model);
    while (btor_has_next_node_hash_table_iterator (&it))
    {
      synth_res = it.bucket->data.as_ptr;
      cur       = btor_next_node_hash_table_iterator (&it);
      if (synth_res->type == BTOR_SYNTH_TYPE_SK_UF)
      {
        assert (BTOR_COUNT_STACK (synth_res->exps) == 1);
        cur = BTOR_TOP_STACK (synth_res->exps);
        if (cur->parameterized)
        {
          //		  printf ("parameterized: %s\n", node2string (cur));
          subst = expand_evars (btor, cur, model);
          assert (!BTOR_REAL_ADDR_NODE (subst)->parameterized);
        }
        else
          subst = btor_copy_exp (btor, cur);
        if (!btor_mapped_node (funs, cur)) btor_map_node (funs, cur, subst);
        btor_release_exp (btor, subst);
      }
    }
  }

  BTOR_INIT_STACK (visit);
  BTOR_INIT_STACK (args);

  //  uint32_t num_exps = 0;
  //  for (i = 1; i < BTOR_NUM_OPS_NODE - 1; i++)
  //    num_exps += gslv->forall->ops[i].cur;
  BTOR_PUSH_STACK (mm, visit, gslv->forall_formula);
  while (!BTOR_EMPTY_STACK (visit))
  {
    cur      = BTOR_POP_STACK (visit);
    real_cur = BTOR_REAL_ADDR_NODE (cur);

    d = btor_get_int_hash_map (mark, real_cur->id);
    if (!d)
    {
      btor_add_int_hash_map (mark, real_cur->id);
      BTOR_PUSH_STACK (mm, visit, cur);
      for (i = real_cur->arity - 1; i >= 0; i--)
        BTOR_PUSH_STACK (mm, visit, real_cur->e[i]);
    }
    else if (d->as_ptr == 0)
    {
      assert (real_cur->arity <= BTOR_COUNT_STACK (args));
      args.top -= real_cur->arity;
      e = args.top;

      if (btor_is_uf_node (real_cur))
      {
        if (model && (b = btor_get_ptr_hash_table (model, real_cur)))
        {
          synth_res = b->data.as_ptr;
          assert (synth_res->type == BTOR_SYNTH_TYPE_UF);
          result = btor_copy_exp (btor, synth_res->value);
        }
        else
          result = btor_copy_exp (btor, real_cur);
      }
      else if (real_cur->arity == 0)
      {
        /* instantiate universal vars with fresh bv vars in 'uvar_map' */
        if (btor_is_param_node (real_cur)
            && btor_param_is_forall_var (real_cur))
        {
          result = btor_mapped_node (uvar_map, real_cur);
          assert (result);
          result = btor_copy_exp (btor, result);
        }
        else
          result = btor_copy_exp (btor, real_cur);
      }
      else if (btor_is_slice_node (real_cur))
      {
        result = btor_slice_exp (btor,
                                 e[0],
                                 btor_slice_get_upper (real_cur),
                                 btor_slice_get_lower (real_cur));
      }
      /* universal variable got substituted by var in 'uvar_map' */
      else if (btor_is_forall_node (real_cur))
      {
        result = btor_copy_exp (btor, e[1]);
      }
      /* substitute existential variable with either the synthesized model
       * or a fresh skolem constant */
      else if (btor_is_exists_node (real_cur))
      {
        evar = real_cur->e[0];

        if (model && (b = btor_get_ptr_hash_table (model, evar)))
        {
          synth_res = b->data.as_ptr;
          if (synth_res->type == BTOR_SYNTH_TYPE_SK_UF)
          {
            assert (!BTOR_EMPTY_STACK (synth_res->exps));
            a = btor_mapped_node (deps, evar);
            assert (a);
            a      = instantiate_args (btor, a, uvar_map);
            result = 0;
            for (i = 0; i < BTOR_COUNT_STACK (synth_res->exps); i++)
            {
              fun = BTOR_PEEK_STACK (synth_res->exps, i);
              fun = btor_mapped_node (funs, fun);
              assert (fun);
              assert (btor_is_lambda_node (fun));
              app = btor_apply_exp (btor, fun, a);
              // TODO: try to beta reduce fun and measure performance
              subst = substitute_evar (btor, e[1], evar, app);
              btor_release_exp (btor, app);
              if (result)
              {
                tmp = btor_or_exp (btor, result, subst);
                btor_release_exp (btor, result);
                btor_release_exp (btor, subst);
                result = tmp;
              }
              else
                result = subst;
            }
            btor_release_exp (btor, a);
            assert (result);
          }
          else
          {
            assert (synth_res->type == BTOR_SYNTH_TYPE_SK_VAR);
            assert (btor_is_bv_const_node (synth_res->value));
            result = substitute_evar (btor, e[1], evar, synth_res->value);
          }
        }
        /* no model -> substitute with skolem constant */
        else
        {
          fun = btor_mapped_node (skolem, evar);
          assert (fun);
          if ((a = btor_mapped_node (deps, evar)))
          {
            a     = instantiate_args (btor, a, uvar_map);
            subst = btor_apply_exp (btor, fun, a);
            btor_release_exp (btor, a);
          }
          else
            subst = btor_copy_exp (btor, fun);
          result = substitute_evar (btor, e[1], evar, subst);
          btor_release_exp (btor, subst);
        }
      }
      else
        result = btor_create_exp (btor, real_cur->kind, e, real_cur->arity);

      for (i = 0; i < real_cur->arity; i++) btor_release_exp (btor, e[i]);

      d->as_ptr = btor_copy_exp (btor, result);

    PUSH_RESULT:
      BTOR_PUSH_STACK (mm, args, BTOR_COND_INVERT_NODE (cur, result));
    }
    else
    {
      assert (d->as_ptr);
      result = btor_copy_exp (btor, d->as_ptr);
      goto PUSH_RESULT;
    }
  }
  assert (BTOR_COUNT_STACK (args) == 1);
  result = BTOR_POP_STACK (args);

  BTOR_RELEASE_STACK (mm, visit);
  BTOR_RELEASE_STACK (mm, args);
  btor_delete_node_map (funs);

  for (j = 0; j < mark->size; j++)
  {
    if (!mark->keys[j]) continue;
    assert (mark->data[j].as_ptr);
    btor_release_exp (btor, mark->data[j].as_ptr);
  }
  btor_delete_int_hash_map (mark);

  assert (!BTOR_REAL_ADDR_NODE (result)->quantifier_below);
  assert (!BTOR_REAL_ADDR_NODE (result)->parameterized);

  //  uint32_t num_exps2 = 0;
  //  for (i = 1; i < BTOR_NUM_OPS_NODE - 1; i++)
  //    num_exps2 += gslv->forall->ops[i].cur;
  //
  //  printf ("num_ops: %u -> %u\n", num_exps, num_exps2);

  return result;
}

#if 0
static void
underapprox (BtorEFGroundSolvers * gslv)
{
  BtorNode *cur, *var, *ult, *c;
  BtorNodeMapIterator it;

  btor_init_node_map_iterator (&it, gslv->forall_uvars);
  while (btor_has_next_node_map_iterator (&it))
    {
      var = it.it.bucket->data.as_ptr;
      cur = btor_next_node_map_iterator (&it);

      c = btor_int_exp (gslv->forall, 4,
			btor_get_exp_width (gslv->forall, var));
      ult = btor_ult_exp (gslv->forall, var, c);
      btor_release_exp (gslv->forall, c);
      btor_assume_exp (gslv->forall, ult);
      btor_release_exp (gslv->forall, ult);
    }
}
#endif

static BtorSolverResult
find_model (BtorEFSolver *slv, BtorEFGroundSolvers *gslv, bool skip_exists)
{
  double start;
  BtorSolverResult res;
  BtorNode *g;
  BtorPtrHashTable *model = 0;
  BtorHashTableIterator it;

  /* exists solver does not have any constraints, so it does not make much
   * sense to initialize every variable by zero and ask if the model
   * is correct. */
  if (!skip_exists)
  {
#if 0
      btor_init_node_hash_table_iterator (&it, gslv->exists_refinements);
      while (btor_has_next_node_hash_table_iterator (&it))
	{
	  g = btor_next_node_hash_table_iterator (&it);
	  btor_assume_exp (gslv->exists, g);
	}
#endif
    /* query exists solver */
    start = btor_time_stamp ();
    //      printf ("QUERY EXISTS\n");
    //      btor_dump_smt2 (gslv->exists, stdout);
    res = btor_sat_btor (gslv->exists, -1, -1);
    gslv->time.e_solver += btor_time_stamp () - start;

    if (res == BTOR_RESULT_UNSAT) /* formula is UNSAT */
      return res;

    start = btor_time_stamp ();
    model = synthesize_model (slv, gslv);
    gslv->time.synth += btor_time_stamp () - start;

    delete_model (gslv);
    gslv->forall_cur_model = model;
    //      printf ("CUR MODEL:\n");
    //      print_cur_model (gslv);
    //      printf ("\n");
  }

  g = instantiate_formula (gslv, model);
  btor_assume_exp (gslv->forall, BTOR_INVERT_NODE (g));
  btor_release_exp (gslv->forall, g);

  /* query forall solver */
  start = btor_time_stamp ();
  res   = btor_sat_btor (gslv->forall, -1, -1);
  update_formula (gslv);
  assert (!btor_is_proxy_node (gslv->forall_formula));
  gslv->time.f_solver += btor_time_stamp () - start;

  if (res == BTOR_RESULT_UNSAT) /* formula is SAT */
  {
    res = BTOR_RESULT_SAT;
    return res;
  }

  start = btor_time_stamp ();
  // TODO: try to not refine if dual is enabled
  // (refinement over add_inst better?)
  refine_exists_solver (gslv);
  gslv->time.qinst += btor_time_stamp () - start;
  slv->stats.refinements++;

  return BTOR_RESULT_UNKNOWN;
}

#if 0
static BtorSolverResult
find_model (BtorEFSolver * slv, BtorEFGroundSolvers * gslv)
{
  double start;
  BtorSolverResult res;
  BtorNode *g;
  BtorNodeMap *map;

  /* exists solver does not have any constraints, so it does not make much
   * sense to initialize every variable by zero and ask if the model
   * is correct. */
  if (gslv->exists->inconsistent
      || (gslv->exists->unsynthesized_constraints->count
          + gslv->exists->synthesized_constraints->count
          + gslv->exists->varsubst_constraints->count
          + gslv->exists->embedded_constraints->count
          + gslv->exists->assumptions->count > 0))
    {
      /* query exists solver */
      start = btor_time_stamp ();
      res = btor_sat_btor (gslv->exists, -1, -1);
      gslv->time.e_solver += btor_time_stamp () - start;

      if (res == BTOR_RESULT_UNSAT)  /* formula is UNSAT */
	return res;

      start = btor_time_stamp ();
      map = synthesize_model (slv, gslv);
      gslv->time.synth += btor_time_stamp () - start;
      assert (!btor_is_proxy_node (gslv->forall_formula));
      g = btor_substitute_terms (gslv->forall, gslv->forall_formula, map);
      btor_assume_exp (gslv->forall, BTOR_INVERT_NODE (g));
      btor_release_exp (gslv->forall, g);
      if (gslv->exists_cur_model)
	delete_exists_model (gslv->exists_cur_model);
      gslv->exists_cur_model = map;
    }
  else
    btor_assume_exp (gslv->forall, BTOR_INVERT_NODE (gslv->forall_formula));

  /* query forall solver */
  start = btor_time_stamp ();
  res = btor_sat_btor (gslv->forall, -1, -1);
  update_formula (gslv);
  gslv->time.f_solver += btor_time_stamp () - start;

  if (res == BTOR_RESULT_UNSAT) /* formula is SAT */
    {
      res = BTOR_RESULT_SAT;
      return res;
    }

  start = btor_time_stamp ();
  // TODO: try to not refine if dual is enabled
  // (refinement over add_inst better?)
  refine_exists_solver (gslv);
  gslv->time.qinst += btor_time_stamp () - start;
  slv->stats.refinements++;

  return BTOR_RESULT_UNKNOWN;
}
#endif

static void
map_vars (BtorEFGroundSolvers *gslv,
          BtorEFGroundSolvers *dual_gslv,
          BtorIntHashTable *node_map,
          BtorIntHashTable *node_map_dual,
          BtorNodeMap *vars_map,
          BtorNodeMap *dual_vars_map)
{
  int32_t key, mapped, mapped_dual;
  size_t i;
  BtorNode *cur, *cur_dual;

  for (i = 0; i < node_map->size; i++)
  {
    key = node_map->keys[i];
    if (!key) continue;
    assert (btor_contains_int_hash_map (node_map_dual, key));
    mapped      = node_map->data[i].as_int;
    mapped_dual = btor_get_int_hash_map (node_map_dual, key)->as_int;

    cur      = btor_get_node_by_id (gslv->forall, mapped);
    cur_dual = btor_get_node_by_id (dual_gslv->forall, mapped_dual);
    if (!cur) continue;      /* variable was eliminated in original formula */
    if (!cur_dual) continue; /* variable was eliminated in dual formula */
    assert (cur);
    assert (cur_dual);

    if (btor_is_bv_var_node (cur)
        && (btor_mapped_node (gslv->forall_evars, cur)
            || btor_mapped_node (gslv->forall_uvars, cur)))
    {
      btor_map_node (vars_map,
                     cur,
                     btor_is_apply_node (cur_dual) ? cur_dual->e[0] : cur_dual);
    }
    else if (btor_is_apply_node (cur)
             && btor_mapped_node (gslv->forall_evars, cur->e[0]))
    {
      assert (btor_is_bv_var_node (cur_dual));
      btor_map_node (vars_map, cur->e[0], cur_dual);
    }

    if (btor_is_bv_var_node (cur_dual)
        && (btor_mapped_node (dual_gslv->forall_evars, cur_dual)
            || btor_mapped_node (dual_gslv->forall_uvars, cur_dual)))
    {
      btor_map_node (
          dual_vars_map, cur_dual, btor_is_apply_node (cur) ? cur->e[0] : cur);
    }
    else if (btor_is_apply_node (cur_dual)
             && btor_mapped_node (dual_gslv->forall_evars, cur_dual->e[0]))
    {
      assert (btor_is_bv_var_node (cur));
      btor_map_node (dual_vars_map, cur_dual->e[0], cur);
    }
  }
}

/* instantiate universal variables in gslv->forall_formula with model of
 * dual_gslv->forall_cur_model and add it to gslv->exists.
 * 'vars_map' maps universal variables of 'dual_gslv' to their dual counterpart
 * (existential variable) of 'gslv'.
 */
// FIXME
#if 0
static void
add_instantiation (BtorEFGroundSolvers * gslv, BtorEFGroundSolvers * dual_gslv,
		   BtorNodeMap * vars_map)
{
  BtorNodeMap *map, *exp_map;
  BtorNodeMapIterator it;
  BtorNodePtrStack substs, args;
  BtorNode *var_fs, *var_es, *res;
  BtorNode *cur, *m, *subst, *arg, *fun, *app, *earg;
  BtorMemMgr *mm;
  BtorArgsIterator ait;

  if (!dual_gslv->forall_cur_model)
    return;

  mm = gslv->forall->mm;
  map = btor_new_node_map (gslv->forall);
  BTOR_INIT_STACK (args);

  /* map f_forall_vars to resp. assignment */
  BTOR_INIT_STACK (substs);
  btor_init_node_hash_table_iterator (&it, dual_gslv->forall_cur_model);
  while (btor_has_next_node_hash_table_iterator (&it))
    {
      m = it.it.bucket->data.as_ptr;
      cur = btor_next_node_hash_table_iterator (&it);
      var_fs = btor_mapped_node (vars_map, cur);
      /* some variables may have been eliminated in the dual formula */
      if (!var_fs) continue;
      assert (var_fs);
      exp_map = btor_new_node_map (gslv->forall);
      if (btor_is_bv_var_node (cur))
	{
	  assert (btor_is_bv_const_node (m));
	  subst = build_refinement (gslv->forall, m, vars_map);
	}
      else // instantiate UFs with resp. arguments
	{
	  assert (btor_is_uf_node (cur));
	  assert (btor_is_lambda_node (m));
	  app = btor_mapped_node (dual_gslv->forall_evars, cur);
	  assert (app);
	  assert (btor_is_apply_node (app));

	  /* map universal vars from gslv->forall to existential vars of
	   * gslv->forall and then to glsv->exists */
	  btor_init_args_iterator (&ait, app->e[1]);
	  while (btor_has_next_args_iterator (&ait))
	    {
	      /* 'arg' is a universal variable of 'dual_gslv->forall' */
	      arg = btor_next_args_iterator (&ait);
//	      printf ("  arg: %s\n", node2string (arg));
	      assert (btor_is_bv_var_node (arg));
	      assert (btor_mapped_node (dual_gslv->forall_uvars, arg));
	      /* 'earg' is a skolem constant of 'gslv->forall' */
	      earg = btor_mapped_node (vars_map, arg);
	      assert (earg);
	      assert (earg->btor == gslv->forall);
	      assert (btor_is_bv_var_node (earg) || btor_is_uf_node (earg));

//	      printf ("  arg_mapped: %s\n", node2string (earg));
	      if (btor_is_bv_var_node (earg))
		BTOR_PUSH_STACK (mm, args, earg);
	      else
		{
		  earg = btor_mapped_node (gslv->forall_evars, earg);
		  assert (earg);
		  assert (btor_is_apply_node (earg));
		  BTOR_PUSH_STACK (mm, args, earg);
		}
	    }
	  fun = build_refinement (gslv->forall, m, vars_map);
	  subst = btor_apply_exps (gslv->forall, args.start,
				   BTOR_COUNT_STACK (args), fun);  
	  btor_release_exp (gslv->forall, fun);
	  BTOR_RESET_STACK (args);
	}
      assert (BTOR_REAL_ADDR_NODE (subst)->btor == gslv->forall);
      btor_delete_node_map (exp_map);
      btor_map_node (map, var_fs, subst);
      BTOR_PUSH_STACK (mm, substs, subst);
    }

  subst = btor_substitute_terms (gslv->forall, gslv->forall_formula, map);

  btor_delete_node_map (map);
  map = btor_new_node_map (gslv->forall);

  /* map f_exists_vars to e_exists_vars */
  btor_init_node_map_iterator (&it, gslv->exists_evars);
  while (btor_has_next_node_map_iterator (&it))
    {
      var_fs = it.it.bucket->data.as_ptr;
      var_es = btor_next_node_map_iterator (&it);
      btor_map_node (map, var_fs, var_es);
    }

  res = build_refinement (gslv->exists, subst, map); 
  btor_release_exp (gslv->forall, subst);

  while (!BTOR_EMPTY_STACK (substs))
    btor_release_exp (gslv->forall, BTOR_POP_STACK (substs));
  BTOR_RELEASE_STACK (mm, substs);

  BTOR_RELEASE_STACK (mm, args);
  btor_delete_node_map (map);

  // FIXME: for some reason true_exp is generated once for the dual case?
//  assert (res != gslv->exists->true_exp);
//  BTOR_ABORT (res == gslv->exists->true_exp,
//	      "invalid refinement '%s'", node2string (res));
  btor_assert_exp (gslv->exists, res);
  btor_release_exp (gslv->exists, res);
}
#endif

static BtorSolverResult
sat_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  //  double start;
  bool opt_dual_solver, skip_exists = true;
  int32_t key;
  size_t i;
  BtorSolverResult res;
  BtorNode *g, *dual_g, *cur;
  BtorEFGroundSolvers *gslv, *dual_gslv;
  BtorNodeMap *vars_map = 0, *dual_vars_map = 0;
  BtorIntHashTable *node_map = 0, *node_map_dual = 0, *tmp_map = 0;
  /* 'vars_map' maps existential/universal (gslv) to universal/existential
   * vars (dual_gslv) */

  // TODO (ma): incremental support

  /* simplify formula and normalize quantifiers */
  res = btor_simplify (slv->btor);
  if (res != BTOR_RESULT_UNKNOWN) return res;

  opt_dual_solver = btor_get_opt (slv->btor, BTOR_OPT_EF_DUAL_SOLVER) == 1;

  g = btor_normalize_quantifiers (slv->btor);
  //  printf ("NORMALIZED\n");
  //  btor_dump_smt2_node (slv->btor, stdout, g, -1);

#if 0
  if (opt_dual_solver)
    {
      vars_map = btor_new_node_map (slv->btor);
      dual_vars_map = btor_new_node_map (slv->btor);
      tmp_map = btor_new_int_hash_map (slv->btor->mm); 
      node_map = btor_new_int_hash_map (slv->btor->mm);
      node_map_dual = btor_new_int_hash_map (slv->btor->mm);
//      dual_g = btor_normalize_quantifiers_node (slv->btor, BTOR_INVERT_NODE (g),
//					    tmp_map);
      dual_g = btor_invert_quantifiers (slv->btor, g, tmp_map);
//      printf ("INVERTED\n");
//      btor_dump_smt2_node (slv->btor, stdout, dual_g, -1);
      for (i = 0; i < tmp_map->size; i++)
	{
	  key = tmp_map->keys[i];
	  if (!key) continue;
	  cur = btor_get_node_by_id (slv->btor, key);
	  if (!btor_is_param_node (cur)
	      || (!btor_param_is_forall_var (cur)
		  && !btor_param_is_exists_var (cur)))
	      continue;
	  btor_add_int_hash_map (node_map, key)->as_int = key;
	  btor_add_int_hash_map (node_map_dual, key)->as_int =
	    tmp_map->data[i].as_int;
	}
      btor_delete_int_hash_map (tmp_map);
    }
#endif

  gslv = setup_efg_solvers (slv, g, node_map, "forall", "exists");
  btor_release_exp (slv->btor, g);

#if 0
  if (opt_dual_solver)
    {
      dual_gslv = setup_efg_solvers (slv, dual_g, node_map_dual,
				     "dual_forall", "dual_exists");
      btor_release_exp (slv->btor, dual_g);

      assert (!opt_dual_solver || node_map->count == node_map_dual->count);
      map_vars (gslv, dual_gslv, node_map, node_map_dual,
		vars_map, dual_vars_map);
      btor_delete_int_hash_map (node_map);
      btor_delete_int_hash_map (node_map_dual);
    }
#endif

#ifndef NDEBUG
  bool found_dual_model = false;
#endif
  while (true)
  {
    res = find_model (slv, gslv, skip_exists);
    if (res != BTOR_RESULT_UNKNOWN) break;
    assert (!found_dual_model);

#if 0
      if (opt_dual_solver)
	{
	  add_instantiation (dual_gslv, gslv, vars_map);

	  res = find_model (slv, dual_gslv);
	  if (res == BTOR_RESULT_SAT || res == BTOR_RESULT_UNKNOWN)
	    {
	      add_instantiation (gslv, dual_gslv, dual_vars_map);
	      /* the formula is only UNSAT if there are no UFs in the original
	       * one */
	      if (res == BTOR_RESULT_SAT
		  && gslv->exists_ufs->table->count == 0)
		    {
#ifndef NDEBUG
		      found_dual_model = true;
#endif
		      printf ("FOUND DUAL MODEL\n");
		    }
	    }
	  else if (res == BTOR_RESULT_UNSAT)
	    {
	      printf ("VALID\n");
	    }
	  slv->time.dual_e_solver = dual_gslv->time.e_solver;
	  slv->time.dual_f_solver = dual_gslv->time.f_solver;
	  slv->time.dual_synth = dual_gslv->time.synth;
	  slv->time.dual_qinst = dual_gslv->time.qinst;
	}
#endif
    slv->time.e_solver = gslv->time.e_solver;
    slv->time.f_solver = gslv->time.f_solver;
    slv->time.synth    = gslv->time.synth;
    slv->time.qinst    = gslv->time.qinst;
    skip_exists        = false;
  }

  if (res == BTOR_RESULT_SAT) print_cur_model (gslv);

  slv->time.e_solver = gslv->time.e_solver;
  slv->time.f_solver = gslv->time.f_solver;
  slv->time.synth    = gslv->time.synth;
  slv->time.qinst    = gslv->time.qinst;

#if 0
  if (opt_dual_solver)
    {
      slv->time.dual_e_solver = dual_gslv->time.e_solver;
      slv->time.dual_f_solver = dual_gslv->time.f_solver;
      slv->time.dual_synth = dual_gslv->time.synth;
      slv->time.dual_qinst = dual_gslv->time.qinst;
      btor_delete_node_map (vars_map);
      btor_delete_node_map (dual_vars_map);
      delete_efg_solvers (slv, dual_gslv);
    }
#endif
  delete_efg_solvers (slv, gslv);
  slv->btor->last_sat_result = res;
  return res;
}

static void
generate_model_ef_solver (BtorEFSolver *slv,
                          bool model_for_all_nodes,
                          bool reset)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  // TODO (ma): for now not supported
  (void) model_for_all_nodes;
  (void) reset;

  //  BtorNode *cur, *param, *var_fs;
  //  BtorNodeMapIterator it;
  //  const BtorBitVector *bv;

  btor_init_bv_model (slv->btor, &slv->btor->bv_model);
  btor_init_fun_model (slv->btor, &slv->btor->fun_model);
#if 0
  btor_init_node_map_iterator (&it, slv->e_exists_vars);
  while (btor_has_next_node_map_iterator (&it))
    {
      cur = btor_next_node_map_iterator (&it);
      var_fs = btor_mapped_node (slv->e_exists_vars, cur);
      param = btor_mapped_node (slv->f_exists_vars, var_fs);

      bv = btor_get_bv_model (slv->e_solver,
	       btor_simplify_exp (slv->e_solver, cur));
      assert (btor_get_node_by_id (slv->btor, param->id));
      btor_add_to_bv_model (
	  slv->btor, slv->btor->bv_model,
	  btor_get_node_by_id (slv->btor, param->id),
	  (BtorBitVector *) bv);
    }

  return;
  // TODO (ma): UF models
  btor_init_node_map_iterator (&it, slv->f_synth_fun_models);
  while (btor_has_next_node_map_iterator (&it))
    {
      printf ("model for %s\n", node2string (it.it.cur));
      cur = btor_next_data_node_map_iterator (&it)->as_ptr;
      btor_dump_btor_node (slv->f_solver, stdout, cur);
      btor_dump_smt2_node (slv->f_solver, stdout, cur, -1);
    }
#endif
}

static void
print_stats_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  BTOR_MSG (slv->btor->msg, 1, "");
  BTOR_MSG (slv->btor->msg,
            1,
            "exists solver refinements: %u",
            slv->stats.refinements);
  BTOR_MSG (slv->btor->msg,
            1,
            "synthesize function aborts: %u",
            slv->stats.synth_aborts);
  BTOR_MSG (
      slv->btor->msg, 1, "synthesized functions: %u", slv->stats.synth_funs);
  BTOR_MSG (slv->btor->msg,
            1,
            "synthesized functions reused: %u",
            slv->stats.synth_funs_reused);
  //  printf ("****************\n");
  //  btor_print_stats_btor (slv->e_solver);
  //  btor_print_stats_btor (slv->f_solver);
}

static void
print_time_stats_ef_solver (BtorEFSolver *slv)
{
  assert (slv);
  assert (slv->kind == BTOR_EF_SOLVER_KIND);
  assert (slv->btor);
  assert (slv->btor->slv == (BtorSolver *) slv);

  BTOR_MSG (
      slv->btor->msg, 1, "%.2f seconds exists solver", slv->time.e_solver);
  BTOR_MSG (
      slv->btor->msg, 1, "%.2f seconds forall solver", slv->time.f_solver);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds synthesizing functions",
            slv->time.synth);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds quantifier instantiation",
            slv->time.qinst);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds dual exists solver",
            slv->time.dual_e_solver);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds dual forall solver",
            slv->time.dual_f_solver);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds dual synthesizing functions",
            slv->time.dual_synth);
  BTOR_MSG (slv->btor->msg,
            1,
            "%.2f seconds dual quantifier instantiation",
            slv->time.dual_qinst);
}

BtorSolver *
btor_new_ef_solver (Btor *btor)
{
  assert (btor);
  // TODO (ma): incremental calls not supported yet
  assert (!btor_get_opt (btor, BTOR_OPT_INCREMENTAL));

  BtorEFSolver *slv;

  BTOR_CNEW (btor->mm, slv);

  slv->kind               = BTOR_EF_SOLVER_KIND;
  slv->btor               = btor;
  slv->api.clone          = (BtorSolverClone) clone_ef_solver;
  slv->api.delet          = (BtorSolverDelete) delete_ef_solver;
  slv->api.sat            = (BtorSolverSat) sat_ef_solver;
  slv->api.generate_model = (BtorSolverGenerateModel) generate_model_ef_solver;
  slv->api.print_stats    = (BtorSolverPrintStats) print_stats_ef_solver;
  slv->api.print_time_stats =
      (BtorSolverPrintTimeStats) print_time_stats_ef_solver;

  BTOR_MSG (btor->msg, 1, "enabled ef engine");

  return (BtorSolver *) slv;
}

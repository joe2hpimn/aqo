#include "aqo.h"
#include "access/parallel.h"
#include "utils/queryenvironment.h"

/*****************************************************************************
 *
 *	QUERY EXECUTION STATISTICS COLLECTING UTILITIES
 *
 * The module which updates data in the feature space linked with executed query
 * type using obtained query execution statistics.
 * Works only if aqo_learn is on.
 *
 *****************************************************************************/

typedef struct
{
	List *clauselist;
	List *selectivities;
	List *relidslist;
} aqo_obj_stat;

static double cardinality_sum_errors;
static int	cardinality_num_objects;

/* It is needed to recognize stored Query-related aqo data in the query
 * environment field.
 */
static char *AQOPrivateData = "AQOPrivateData";


/* Query execution statistics collecting utilities */
static void atomic_fss_learn_step(int fss_hash, int ncols,
					  double **matrix, double *targets,
					  double *features, double target);
static void learn_sample(List *clauselist,
			 List *selectivities,
			 List *relidslist,
			 double true_cardinality,
			 double predicted_cardinality);
static List *restore_selectivities(List *clauselist,
					  List *relidslist,
					  JoinType join_type,
					  bool was_parametrized);
static void update_query_stat_row(double *et, int *et_size,
					  double *pt, int *pt_size,
					  double *ce, int *ce_size,
					  double planning_time,
					  double execution_time,
					  double cardinality_error,
					  int64 *n_exec);
static void StoreToQueryContext(QueryDesc *queryDesc);
static bool ExtractFromQueryContext(QueryDesc *queryDesc);
static void RemoveFromQueryContext(QueryDesc *queryDesc);

/*
 * This is the critical section: only one runner is allowed to be inside this
 * function for one feature subspace.
 * matrix and targets are just preallocated memory for computations.
 */
static void
atomic_fss_learn_step(int fss_hash, int ncols,
					  double **matrix, double *targets,
					  double *features, double target)
{
	int	nrows;

	if (!load_fss(fss_hash, ncols, matrix, targets, &nrows))
		nrows = 0;

	nrows = OkNNr_learn(nrows, ncols, matrix, targets, features, target);
	update_fss(fss_hash, nrows, ncols, matrix, targets);
}

/*
 * For given object (i. e. clauselist, selectivities, relidslist, predicted and
 * true cardinalities) performs learning procedure.
 */
static void
learn_sample(List *clauselist, List *selectivities, List *relidslist,
			 double true_cardinality, double predicted_cardinality)
{
	int			fss_hash;
	int			nfeatures;
	double	  *matrix[aqo_K];
	double	   targets[aqo_K];
	double	   *features;
	double		target;
	int			i;

	cardinality_sum_errors += fabs(log(predicted_cardinality) -
								   log(true_cardinality));
	cardinality_num_objects += 1;
/*
 * Suppress the optimization for debug purposes.
	if (fabs(log(predicted_cardinality) - log(true_cardinality)) <
		object_selection_prediction_threshold)
	{
		return;
	}
*/
	target = log(true_cardinality);

	fss_hash = get_fss_for_object(clauselist, selectivities, relidslist,
					   &nfeatures, &features);

	/* In the case of zero matrix we not need to learn */

	if (nfeatures > 0)
		for (i = 0; i < aqo_K; ++i)
			matrix[i] = palloc(sizeof(double) * nfeatures);

	/* Here should be critical section */
	atomic_fss_learn_step(fss_hash, nfeatures, matrix, targets, features, target);
	/* Here should be the end of critical section */

	if (nfeatures > 0)
		for (i = 0; i < aqo_K; ++i)
			pfree(matrix[i]);

	pfree(features);
}

/*
 * For given node specified by clauselist, relidslist and join_type restores
 * the same selectivities of clauses as were used at query optimization stage.
 */
List *
restore_selectivities(List *clauselist,
					  List *relidslist,
					  JoinType join_type,
					  bool was_parametrized)
{
	List	   *lst = NIL;
	ListCell   *l;
	int			i = 0;
	bool		parametrized_sel;
	int			nargs;
	int		   *args_hash;
	int		   *eclass_hash;
	double	   *cur_sel;
	int			cur_hash;
	int			cur_relid;

	parametrized_sel = was_parametrized && (list_length(relidslist) == 1);
	if (parametrized_sel)
	{
		cur_relid = linitial_int(relidslist);
		get_eclasses(clauselist, &nargs, &args_hash, &eclass_hash);
	}

	foreach(l, clauselist)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		cur_sel = NULL;
		if (parametrized_sel)
		{
			cur_hash = get_clause_hash(rinfo->clause, nargs,
									   args_hash, eclass_hash);
			cur_sel = selectivity_cache_find_global_relid(cur_hash, cur_relid);
			if (cur_sel == NULL)
			{
				if (join_type == JOIN_INNER)
					cur_sel = &rinfo->norm_selec;
				else
					cur_sel = &rinfo->outer_selec;
			}
		}
		else if (join_type == JOIN_INNER)
			cur_sel = &rinfo->norm_selec;
		else
			cur_sel = &rinfo->outer_selec;

		lst = lappend(lst, cur_sel);
		i++;
	}

	if (parametrized_sel)
	{
		pfree(args_hash);
		pfree(eclass_hash);
	}

	return lst;
}

/*
 * Walks over obtained PlanState tree, collects relation objects with their
 * clauses, selectivities and relids and passes each object to learn_sample.
 *
 * Returns clauselist, selectivities and relids.
 * Store observed subPlans into other_plans list.
 *
 * We use list_copy() of p->plan->path_clauses and p->plan->path_relids
 * because the plan may be stored in the cache after this. Operation
 * list_concat() changes input lists and may destruct cached plan.
 */
static bool
learnOnPlanState(PlanState *p, void *context)
{
	aqo_obj_stat *ctx = (aqo_obj_stat *) context;

	planstate_tree_walker(p, learnOnPlanState, context);

	/*
	 * Some nodes inserts after planning step (See T_Hash node type).
	 * In this case we have'nt AQO prediction and fss record.
	 */
	if (p->plan->had_path)
	{
		List *cur_selectivities;

		cur_selectivities = restore_selectivities(p->plan->path_clauses,
												  p->plan->path_relids,
												  p->plan->path_jointype,
												  p->plan->was_parametrized);
		ctx->selectivities = list_concat(ctx->selectivities, cur_selectivities);
		ctx->clauselist = list_concat(ctx->clauselist,
											list_copy(p->plan->path_clauses));
		if (p->plan->path_relids != NIL)
			/*
			 * This plan can be stored as cached plan. In the case we will have
			 * bogus path_relids field (changed by list_concat routine) at the
			 * next usage (and aqo-learn) of this plan.
			 */
			ctx->relidslist = list_copy(p->plan->path_relids);

		if (p->instrument && (p->righttree != NULL || p->lefttree == NULL ||
							  p->plan->path_clauses != NIL))
		{
			double learn_rows;

			InstrEndLoop(p->instrument);
			if (p->instrument->nloops > 0)
			{
				learn_rows = p->instrument->ntuples / p->instrument->nloops;

				if (p->plan->parallel_aware || (p->plan->path_parallel_workers > 0 &&
					(nodeTag(p->plan) == T_HashJoin ||
					nodeTag(p->plan) == T_MergeJoin ||
					nodeTag(p->plan) == T_NestLoop)))
					learn_rows *= (p->plan->path_parallel_workers + 1);

				/* It is needed for correct exp(result) calculation. */
				if (learn_rows < 1.)
					learn_rows = 1.;
			}
			else
			{
				/*
				 * LAV: I found only one case for this code: if query returns
				 * with error. May be we will process this case and not learn
				 * AQO on the query?
				 */
				learn_rows = 1.;
			}

			/*
			 * Some execution logic optimizations can lead to the situation
			 * than a subtree will never be visited.
			 */
			if (!(p->instrument->ntuples <= 0. && p->instrument->nloops <= 0.))
				learn_sample(ctx->clauselist, ctx->selectivities, ctx->relidslist,
							 learn_rows, p->plan->plan_rows);
		}
	}

	return false;
}

/*
 * Updates given row of query statistics.
 */
void
update_query_stat_row(double *et, int *et_size,
					  double *pt, int *pt_size,
					  double *ce, int *ce_size,
					  double planning_time,
					  double execution_time,
					  double cardinality_error,
					  int64 *n_exec)
{
	int			i;

	if (cardinality_error >= 0)
	{
		if (*ce_size >= aqo_stat_size)
			for (i = 1; i < aqo_stat_size; ++i)
				ce[i - 1] = ce[i];
		*ce_size = (*ce_size >= aqo_stat_size) ? aqo_stat_size : (*ce_size + 1);
		ce[*ce_size - 1] = cardinality_error;
	}

	if (*et_size >= aqo_stat_size)
		for (i = 1; i < aqo_stat_size; ++i)
			et[i - 1] = et[i];

	*et_size = (*et_size >= aqo_stat_size) ? aqo_stat_size : (*et_size + 1);
	et[*et_size - 1] = execution_time;

	if (*pt_size >= aqo_stat_size)
		for (i = 1; i < aqo_stat_size; ++i)
			pt[i - 1] = pt[i];

	*pt_size = (*pt_size >= aqo_stat_size) ? aqo_stat_size : (*pt_size + 1);
	pt[*pt_size - 1] = planning_time;
	(*n_exec)++;
}

/*****************************************************************************
 *
 *	QUERY EXECUTION STATISTICS COLLECTING HOOKS
 *
 *****************************************************************************/

/*
 * Set up flags to store cardinality statistics.
 */
void
aqo_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	instr_time	current_time;

	if (query_context.use_aqo || query_context.learn_aqo)
	{
		INSTR_TIME_SET_CURRENT(current_time);
		INSTR_TIME_SUBTRACT(current_time, query_context.query_starttime);
		query_context.query_planning_time = INSTR_TIME_GET_DOUBLE(current_time);

		query_context.explain_only = ((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0);

		if (query_context.learn_aqo && !query_context.explain_only)
			queryDesc->instrument_options |= INSTRUMENT_ROWS;

		/* Save all query-related parameters into the query context. */
		StoreToQueryContext(queryDesc);
	}

	if (prev_ExecutorStart_hook)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * General hook which runs before ExecutorEnd and collects query execution
 * cardinality statistics.
 * Also it updates query execution statistics in aqo_query_stat.
 */
void
aqo_ExecutorEnd(QueryDesc *queryDesc)
{
	double		totaltime;
	double		cardinality_error;
	QueryStat  *stat = NULL;
	instr_time	endtime;

	if (!ExtractFromQueryContext(queryDesc))
		/* AQO keep all query-related preferences at the query context.
		 * It is needed to prevent from possible recursive changes, at
		 * preprocessing stage of subqueries.
		 * If context not exist we assume AQO was disabled at preprocessing
		 * stage for this query.
		 */
		goto end;

	Assert(!IsParallelWorker());

	if (query_context.explain_only)
	{
		query_context.learn_aqo = false;
		query_context.collect_stat = false;
	}

	if (query_context.learn_aqo)
	{
		aqo_obj_stat ctx = {NIL, NIL, NIL};

		cardinality_sum_errors = 0.;
		cardinality_num_objects = 0;

		learnOnPlanState(queryDesc->planstate, (void *) &ctx);
	}

	if (query_context.collect_stat)
	{
		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_SUBTRACT(endtime, query_context.query_starttime);
		totaltime = INSTR_TIME_GET_DOUBLE(endtime);
		if (query_context.learn_aqo && cardinality_num_objects > 0)
			cardinality_error = cardinality_sum_errors /
				cardinality_num_objects;
		else
			cardinality_error = -1;

		stat = get_aqo_stat(query_context.query_hash);
		if (stat != NULL)
		{
			if (query_context.use_aqo)
				update_query_stat_row(stat->execution_time_with_aqo,
									  &stat->execution_time_with_aqo_size,
									  stat->planning_time_with_aqo,
									  &stat->planning_time_with_aqo_size,
									  stat->cardinality_error_with_aqo,
									  &stat->cardinality_error_with_aqo_size,
									  query_context.query_planning_time,
									  totaltime - query_context.query_planning_time,
									  cardinality_error,
									  &stat->executions_with_aqo);
			else
				update_query_stat_row(stat->execution_time_without_aqo,
									  &stat->execution_time_without_aqo_size,
									  stat->planning_time_without_aqo,
									  &stat->planning_time_without_aqo_size,
									  stat->cardinality_error_without_aqo,
									  &stat->cardinality_error_without_aqo_size,
									  query_context.query_planning_time,
									  totaltime - query_context.query_planning_time,
									  cardinality_error,
									  &stat->executions_without_aqo);
		}
	}
	selectivity_cache_clear();

	/*
	 * Store all learn data into the AQO service relations.
	 */
	if ((query_context.collect_stat) && (stat != NULL))
	{
		if (!query_context.adding_query && query_context.auto_tuning)
			automatical_query_tuning(query_context.query_hash, stat);

		update_aqo_stat(query_context.fspace_hash, stat);
		pfree_query_stat(stat);
	}
	RemoveFromQueryContext(queryDesc);

end:
	if (prev_ExecutorEnd_hook)
		prev_ExecutorEnd_hook(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);

	/*
	 * standard_ExecutorEnd clears the queryDesc->planstate. After this point no
	 * one operation with the plan can be made.
	 */
}

/*
 * Converts path info into plan node for collecting it after query execution.
 */
void
aqo_copy_generic_path_info(PlannerInfo *root, Plan *dest, Path *src)
{
	bool		is_join_path;

	if (prev_copy_generic_path_info_hook)
		prev_copy_generic_path_info_hook(root, dest, src);

	is_join_path = (src->type == T_NestPath || src->type == T_MergePath ||
					src->type == T_HashPath);

	if (dest->had_path)
	{
		/*
		 * The convention is that any extension that sets had_path is also
		 * responsible for setting path_clauses, path_jointype, path_relids,
		 * path_parallel_workers, and was_parameterized.
		 */
		Assert(dest->path_clauses && dest->path_jointype &&
			   dest->path_relids && dest->path_parallel_workers);
		return;
	}

	if (is_join_path)
	{
		dest->path_clauses = ((JoinPath *) src)->joinrestrictinfo;
		dest->path_jointype = ((JoinPath *) src)->jointype;
	}
	else
	{
		dest->path_clauses = list_concat(
									list_copy(src->parent->baserestrictinfo),
						 src->param_info ? src->param_info->ppi_clauses : NIL);
		dest->path_jointype = JOIN_INNER;
	}

	dest->path_relids = get_list_of_relids(root, src->parent->relids);
	dest->path_parallel_workers = src->parallel_workers;
	dest->was_parametrized = (src->param_info != NULL);

	if (src->param_info)
	{
		dest->predicted_cardinality = src->param_info->predicted_ppi_rows;
		dest->fss_hash = src->param_info->fss_ppi_hash;
	}
	else
	{
		dest->predicted_cardinality = src->parent->predicted_cardinality;
		dest->fss_hash = src->parent->fss_hash;
	}

	dest->had_path = true;
}

/*
 * Store into query environment field AQO data related to the query.
 * We introduce this machinery to avoid problems with subqueries, induced by
 * top-level query.
 */
static void
StoreToQueryContext(QueryDesc *queryDesc)
{
	EphemeralNamedRelation	enr;
	int	qcsize = sizeof(QueryContextData);
	MemoryContext	oldCxt;

	oldCxt = MemoryContextSwitchTo(AQOMemoryContext);
	enr = palloc0(sizeof(EphemeralNamedRelationData));
	if (queryDesc->queryEnv == NULL)
		queryDesc->queryEnv = create_queryEnv();

	enr->md.name = AQOPrivateData;
	enr->md.enrtuples = 0;
	enr->md.enrtype = 0;
	enr->md.reliddesc = InvalidOid;
	enr->md.tupdesc = NULL;

	enr->reldata = palloc0(qcsize);
	memcpy(enr->reldata, &query_context, qcsize);

	register_ENR(queryDesc->queryEnv, enr);
	MemoryContextSwitchTo(oldCxt);
}

/*
 * Restore AQO data, related to the query.
 */
static bool
ExtractFromQueryContext(QueryDesc *queryDesc)
{
	EphemeralNamedRelation	enr;

	/* This is a very rare case when we don't load aqo as shared library during
	 * startup perform 'CREATE EXTENSION aqo' command in the backend and first
	 * query in any another backend is 'UPDATE aqo_queries...'. In this case
	 * ExecutorEnd hook will be executed without ExecutorStart hook.
	 */
	if (queryDesc->queryEnv == NULL)
		return false;

	enr = get_ENR(queryDesc->queryEnv, AQOPrivateData);

	if (enr == NULL)
		return false;

	memcpy(&query_context, enr->reldata, sizeof(QueryContextData));

	return true;
}

static void
RemoveFromQueryContext(QueryDesc *queryDesc)
{
	EphemeralNamedRelation enr = get_ENR(queryDesc->queryEnv, AQOPrivateData);
	unregister_ENR(queryDesc->queryEnv, AQOPrivateData);
	pfree(enr->reldata);
	pfree(enr);
}

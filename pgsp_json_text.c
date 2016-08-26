/*-------------------------------------------------------------------------
 *
 * pgsp_json_text.h: Text plan generator for pg_store_plan.
 *
 * Copyright (c) 2012-2015, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plan/pgsp_json_text.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/bitmapset.h"
#include "utils/json.h"
#include "utils/jsonapi.h"
#include "utils/builtins.h"

#include "pgsp_json_text.h"
#include "pgsp_json_int.h"

static void clear_nodeval(node_vals *vals);
static void print_current_node(pgspParserContext *ctx);
static void print_current_trig_node(pgspParserContext *ctx);
static void print_prop(StringInfo s, char *prepstr,
					   const char *prop, int leve, int exind);
static void print_prop_if_exists(StringInfo s, char *prepstr,
								 const char *prop, int leve, int exind);
static void print_prop_if_nz(StringInfo s, char *prepstr,
							 const char *prop, int leve, int exind);
static void json_text_objstart(void *state);
static void json_text_objend(void *state);
static void json_text_ofstart(void *state, char *fname, bool isnull);
static void json_text_ofend(void *state, char *fname, bool isnull);
static void json_text_scalar(void *state, char *token, JsonTokenType tokentype);

/* Parser callbacks for plan textization */
SETTERDECL(node_type)
{
	word_table *p;

	vals->node_type = val;
	vals->nodetag = T_Invalid;

	p = search_word_table(nodetypes, val, PGSP_JSON_TEXTIZE);
	if (p)
	{
		vals->node_type = (p->textname ? p->textname : p->longname);
		vals->nodetag = p->tag;
	}
}

SETTERDECL(strategy)
{
	word_table *p;
	
	p = search_word_table(strategies, val, PGSP_JSON_TEXTIZE);

	switch (vals->nodetag)
	{
		case T_Agg:
			switch (p->tag)
			{
				case S_Hashed:
					vals->node_type = "HashAggregate"; break;
				case S_Sorted:
					vals->node_type = "GroupAggregate"; break;
				default:
					break;
			}
			break;

		case T_SetOp:
			if (p->tag == S_Hashed)
				vals->node_type = "HashSetOp";
			break;

		default:
			break;
	}
}
CONVERSION_SETTER(scan_dir, conv_scandir);
SQLQUOTE_SETTER(obj_name);
SQLQUOTE_SETTER(alias);
SQLQUOTE_SETTER(schema_name);
LIST_SETTER(output);
DEFAULT_SETTER(merge_cond);
CONVERSION_SETTER(join_type, conv_jointype);
CONVERSION_SETTER(setopcommand, conv_setsetopcommand);
CONVERSION_SETTER(sort_method, conv_sortmethod);
LIST_SETTER(sort_key);
SQLQUOTE_SETTER(index_name);
DEFAULT_SETTER(startup_cost);
DEFAULT_SETTER(total_cost);
DEFAULT_SETTER(plan_rows);
DEFAULT_SETTER(plan_width);
DEFAULT_SETTER(sort_space_used);
CONVERSION_SETTER(sort_space_type, conv_sortspacetype);
DEFAULT_SETTER(filter);
DEFAULT_SETTER(join_filter);
DEFAULT_SETTER(func_call);
DEFAULT_SETTER(index_cond);
DEFAULT_SETTER(recheck_cond);
CONVERSION_SETTER(operation, conv_operation);
DEFAULT_SETTER(subplan_name);
DEFAULT_SETTER(hash_cond);
DEFAULT_SETTER(tid_cond);
DEFAULT_SETTER(filter_removed);
DEFAULT_SETTER(idxrchk_removed);
DEFAULT_SETTER(peak_memory_usage);
DEFAULT_SETTER(org_hash_batches);
DEFAULT_SETTER(org_hash_buckets);
DEFAULT_SETTER(hash_batches);
DEFAULT_SETTER(hash_buckets);
DEFAULT_SETTER(actual_startup_time);
DEFAULT_SETTER(actual_total_time);
DEFAULT_SETTER(actual_rows);
DEFAULT_SETTER(actual_loops);
DEFAULT_SETTER(heap_fetches);
DEFAULT_SETTER(shared_hit_blks);
DEFAULT_SETTER(shared_read_blks);
DEFAULT_SETTER(shared_dirtied_blks);
DEFAULT_SETTER(shared_written_blks);
DEFAULT_SETTER(local_hit_blks);
DEFAULT_SETTER(local_read_blks);
DEFAULT_SETTER(local_dirtied_blks);
DEFAULT_SETTER(local_written_blks);
DEFAULT_SETTER(temp_read_blks);
DEFAULT_SETTER(temp_written_blks);
DEFAULT_SETTER(io_read_time);
DEFAULT_SETTER(io_write_time);
SQLQUOTE_SETTER(trig_name);
SQLQUOTE_SETTER(trig_relation);
DEFAULT_SETTER(trig_time);
DEFAULT_SETTER(trig_calls);
DEFAULT_SETTER(plan_time);
DEFAULT_SETTER(exec_time);
DEFAULT_SETTER(exact_heap_blks);
DEFAULT_SETTER(lossy_heap_blks);
DEFAULT_SETTER(joinfilt_removed);

#define ISZERO(s) (!s || strcmp(s, "0") == 0 || strcmp(s, "0.000") == 0 )
#define HASSTRING(s) (s && strlen(s) > 0)
#define TEXT_LEVEL_STEP 6
#define TEXT_INDENT_OFFSET 2
#define TEXT_INDENT_BASE(l, e)											\
	(((l < 2) ? 0 : (TEXT_LEVEL_STEP * (l - 2) + TEXT_INDENT_OFFSET)) + e)
#define TEXT_INDENT_DETAILS(l, e)					\
	(TEXT_INDENT_BASE(l, e) + ((l < 2) ? 2 : 6))

static void
print_obj_name0(StringInfo s,
				const char *obj_name, const char *schema_name, const char *alias)
{
	bool on_written = false;

	if (HASSTRING(obj_name))
	{
		on_written = true;
		appendStringInfoString(s, " on ");
		if (HASSTRING(schema_name))
		{
			appendStringInfoString(s, schema_name);
			appendStringInfoChar(s, '.');
		}
		appendStringInfoString(s, obj_name);
	}
	if (HASSTRING(alias) &&
		(!HASSTRING(obj_name) || strcmp(obj_name, alias) != 0))
	{
		if (!on_written)
			appendStringInfoString(s, " on ");
		else
			appendStringInfoChar(s, ' ');
		appendStringInfoString(s, alias);
	}
}

static void
print_obj_name(pgspParserContext *ctx)
{
	node_vals *v = ctx->nodevals;
	StringInfo s = ctx->dest;

	print_obj_name0(s, v->obj_name, v->schema_name, v->alias);
}

static void
print_prop(StringInfo s, char *prepstr,
		   const char *prop, int level, int exind)
{
	if (level > 0)
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
	}
	appendStringInfoString(s, prepstr);
	appendStringInfoString(s, prop);
}

static void
print_prop_if_exists(StringInfo s, char *prepstr,
					 const char *prop, int level, int exind)
{
	if (HASSTRING(prop))
		print_prop(s, prepstr, prop, level, exind);
}

static void
print_propstr_if_exists(StringInfo s, char *prepstr,
						StringInfo prop, int level, int exind)
{
	if (prop && prop->data[0])
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, prepstr);
		appendStringInfoString(s, prop->data);
	}
}

static void
print_prop_if_nz(StringInfo s, char *prepstr,
				 const char *prop, int level, int exind)
{
	if (!ISZERO(prop))
		print_prop(s, prepstr, prop, level, exind);
}

static void 
print_current_node(pgspParserContext *ctx)
{
	node_vals *v = ctx->nodevals;
	StringInfo s = ctx->dest;
	ListCell *lc;
	int level = ctx->level - 1;
	bool comma = false;
	int exind = 0;

	if (v->node_type == T_Invalid)
		return;

	if (s->len > 0)
		appendStringInfoString(s, "\n");
	appendStringInfoSpaces(s, TEXT_INDENT_BASE(level, exind));

	if (HASSTRING(v->subplan_name))
	{
		appendStringInfoString(s, v->subplan_name);
		appendStringInfoString(s, "\n");
		exind = 2;
		appendStringInfoSpaces(s, TEXT_INDENT_BASE(level, exind));
	}

	/* list items doesn't need this header */
	if (level > 1 && ctx->current_list == P_Invalid)
		appendStringInfoString(s, "->  ");

	switch (v->nodetag)
	{
		case T_ModifyTable:
		case T_SeqScan:
		case T_BitmapHeapScan:
		case T_TidScan:
		case T_SubqueryScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_WorkTableScan:
		case T_ForeignScan:
			if (v->nodetag == T_ModifyTable)
				appendStringInfoString(s, v->operation);
			else
				appendStringInfoString(s, v->node_type);

			print_obj_name(ctx);
			break;

		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapIndexScan:
			appendStringInfoString(s, v->node_type);
			print_prop_if_exists(s, " ", v->scan_dir, 0, 0);
			print_prop_if_exists(s, " using ", v->index_name, 0, 0);
			print_obj_name(ctx);
			break;

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			appendStringInfoString(s, v->node_type);
			if (v->join_type && strcmp(v->join_type, "Inner") != 0)
			{
				appendStringInfoChar(s, ' ');
				appendStringInfoString(s, v->join_type);
			}
			if (v->nodetag != T_NestLoop)
				appendStringInfoString(s, " Join");
			break;

		case T_SetOp:
			appendStringInfoString(s, v->node_type);
			print_prop_if_exists(s, " ", v->setopcommand, 0, 0);
			break;

		default:
			appendStringInfoString(s, v->node_type);
			break;
	}

	/* Don't show costs for child talbes */
	if (ctx->current_list == P_TargetTables)
		return;

	if (!ISZERO(v->startup_cost) &&
		!ISZERO(v->total_cost) &&
		HASSTRING(v->plan_rows) &&
		HASSTRING(v->plan_width))
	{
		appendStringInfoString(s, "  (cost=");
		appendStringInfoString(s, v->startup_cost);
		appendStringInfoString(s, "..");
		appendStringInfoString(s, v->total_cost);
		appendStringInfoString(s, " rows=");
		appendStringInfoString(s, v->plan_rows);
		appendStringInfoString(s, " width=");
		appendStringInfoString(s, v->plan_width);
		appendStringInfoString(s, ")");
	}

	if (HASSTRING(v->actual_loops) && ISZERO(v->actual_loops))
		appendStringInfoString(s, " (never executed)");
	else if (HASSTRING(v->actual_rows) &&
			 HASSTRING(v->actual_loops) &&
			 HASSTRING(v->actual_startup_time) &&
			 HASSTRING(v->actual_total_time))
	{
		appendStringInfoString(s, " (actual ");
		appendStringInfoString(s, "time=");
		appendStringInfoString(s, v->actual_startup_time);
		appendStringInfoString(s, "..");
		appendStringInfoString(s, v->actual_total_time);
		appendStringInfoString(s, " ");

		appendStringInfoString(s, "rows=");
		appendStringInfoString(s, v->actual_rows);

		appendStringInfoString(s, " loops=");
		appendStringInfoString(s, v->actual_loops);

		appendStringInfoString(s, ")");
	}

	foreach(lc, v->target_tables)
	{
		char *str = (char *)lfirst (lc);

		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, str);
	}
		
	print_propstr_if_exists(s, "Output: ", v->output, level, exind);
	print_prop_if_exists(s, "Merge Cond: ", v->merge_cond, level, exind);
	print_prop_if_exists(s, "Hash Cond: " , v->hash_cond, level, exind);
	print_prop_if_exists(s, "Tid Cond: " , v->tid_cond, level, exind);
	print_prop_if_exists(s, "Join Filter: " , v->join_filter, level, exind);
	print_prop_if_exists(s, "Index Cond: " , v->index_cond, level, exind);
	print_prop_if_exists(s, "Recheck Cond: ", v->recheck_cond, level, exind);
	print_propstr_if_exists(s, "Sort Key: ", v->sort_key, level, exind);

	if (HASSTRING(v->sort_method))
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Sort Method: ");
		appendStringInfoString(s, v->sort_method);

		if (HASSTRING(v->sort_space_type) &&
			HASSTRING(v->sort_space_used))
		{
			appendStringInfoString(s, "  ");
			appendStringInfoString(s, v->sort_space_type);
			appendStringInfoString(s, ": ");
			appendStringInfoString(s, v->sort_space_used);
			appendStringInfoString(s, "kB");
		}
	}

	print_prop_if_exists(s, "Function Call: ", v->func_call, level, exind);
	print_prop_if_exists(s, "Filter: ", v->filter, level, exind);
	print_prop_if_nz(s, "Rows Removed by Filter: ",
						 v->filter_removed, level, exind);
	print_prop_if_nz(s, "Rows Removed by Index Recheck: ",
						 v->idxrchk_removed, level, exind);
	print_prop_if_nz(s, "Rows Removed by Join Filter: ",
						 v->joinfilt_removed, level, exind);

	if (HASSTRING(v->exact_heap_blks) ||
		HASSTRING(v->lossy_heap_blks))
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Heap Blocks:");
		print_prop_if_nz(s, " exact=", v->exact_heap_blks, 0, exind);
		print_prop_if_nz(s, " lossy=", v->lossy_heap_blks, 0, exind);
	}

	if (!ISZERO(v->hash_buckets))
	{
		bool show_original = false;

		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Buckets: ");
		appendStringInfoString(s, v->hash_buckets);

		/* See show_hash_info() in explain.c for details */
		if ((v->org_hash_buckets &&
			 strcmp(v->hash_buckets, v->org_hash_buckets) != 0) ||
			(v->org_hash_batches &&
			 strcmp(v->hash_batches, v->org_hash_batches) != 0))
			show_original = true;

		if (show_original && v->org_hash_buckets)
		{
			appendStringInfoString(s, " (originally ");
			appendStringInfoString(s, v->org_hash_buckets);
			appendStringInfoChar(s, ')');
		}

		if (!ISZERO(v->hash_batches))
		{
			appendStringInfoString(s, "  Batches: ");
			appendStringInfoString(s, v->hash_batches);
			if (show_original && v->org_hash_batches)
			{
				appendStringInfoString(s, " (originally ");
				appendStringInfoString(s, v->org_hash_batches);
				appendStringInfoChar(s, ')');
			}
		}
		if (!ISZERO(v->peak_memory_usage))
		{
			appendStringInfoString(s, "  Memory Usage: ");
			appendStringInfoString(s, v->peak_memory_usage);
			appendStringInfoString(s, "kB");
		}
	}

	print_prop_if_exists(s, "Heap Fetches: ", v->heap_fetches, level, exind);

	if (!ISZERO(v->shared_hit_blks) ||
		!ISZERO(v->shared_read_blks) ||
		!ISZERO(v->shared_dirtied_blks) ||
		!ISZERO(v->shared_written_blks))
	{
		appendStringInfoString(s, "\n");
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "Buffers: shared");

		if (!ISZERO(v->shared_hit_blks))
		{
			appendStringInfoString(s, " hit=");
			appendStringInfoString(s, v->shared_hit_blks);
			comma =true;
		}
		if (!ISZERO(v->shared_read_blks))
		{
			appendStringInfoString(s, " read=");
			appendStringInfoString(s, v->shared_read_blks);
			comma =true;
		}
		if (!ISZERO(v->shared_dirtied_blks))
		{
			appendStringInfoString(s, " dirtied=");
			appendStringInfoString(s, v->shared_dirtied_blks);
			comma =true;
		}
		if (!ISZERO(v->shared_written_blks))
		{
			appendStringInfoString(s, " written=");
			appendStringInfoString(s, v->shared_written_blks);
			comma =true;
		}
	}
	if (!ISZERO(v->local_hit_blks) ||
		!ISZERO(v->local_read_blks) ||
		!ISZERO(v->local_dirtied_blks) ||
		!ISZERO(v->local_written_blks))
	{
		if (comma)
			appendStringInfoString(s, ", ");
		else
		{
			appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
			appendStringInfoString(s, "Buffers: ");
		}

		appendStringInfoString(s, "local");
		if (!ISZERO(v->local_hit_blks))
		{
			appendStringInfoString(s, " hit=");
			appendStringInfoString(s, v->local_hit_blks);
			comma =true;
		}
		if (!ISZERO(v->local_read_blks))
		{
			appendStringInfoString(s, " read=");
			appendStringInfoString(s, v->local_read_blks);
			comma =true;
		}
		if (!ISZERO(v->local_dirtied_blks))
		{
			appendStringInfoString(s, " dirtied=");
			appendStringInfoString(s, v->local_dirtied_blks);
			comma =true;
		}
		if (!ISZERO(v->local_written_blks))
		{
			appendStringInfoString(s, " written=");
			appendStringInfoString(s, v->local_written_blks);
			comma =true;
		}
	}
	if (!ISZERO(v->temp_read_blks) ||
		!ISZERO(v->temp_written_blks))
	{
		if (comma)
			appendStringInfoString(s, ", ");
		else
		{
			appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
			appendStringInfoString(s, "Buffers: ");
		}

		appendStringInfoString(s, "temp");
		if (!ISZERO(v->temp_read_blks))
		{
			appendStringInfoString(s, " read=");
			appendStringInfoString(s, v->temp_read_blks);
			comma =true;
		}
		if (!ISZERO(v->temp_written_blks))
		{
			appendStringInfoString(s, " written=");
			appendStringInfoString(s, v->temp_written_blks);
			comma =true;
		}
	}
	if (!ISZERO(v->io_read_time) ||
		!ISZERO(v->io_write_time))
	{
		/* Feed a line if any of Buffers: items has been shown */
		if (comma)
			appendStringInfoString(s, "\n");
			
		appendStringInfoSpaces(s, TEXT_INDENT_DETAILS(level, exind));
		appendStringInfoString(s, "I/O Timings: ");

		if (!ISZERO(v->io_read_time))
		{
			appendStringInfoString(s, " read=");
			appendStringInfoString(s, v->io_read_time);
		}
		if (!ISZERO(v->io_write_time))
		{
			appendStringInfoString(s, " write=");
			appendStringInfoString(s, v->io_write_time);
		}
	}
}

static void
print_current_trig_node(pgspParserContext *ctx)
{
	node_vals *v = ctx->nodevals;
	StringInfo s = ctx->dest;

	if (HASSTRING(v->trig_name) && !ISZERO(v->trig_time))
	{
		if (s->len > 0)
			appendStringInfoString(s, "\n");
		appendStringInfoString(s, "Trigger ");
		appendStringInfoString(s, v->trig_name);
		appendStringInfoString(s, ": time=");
		appendStringInfoString(s, v->trig_time);
		appendStringInfoString(s, " calls=");
		appendStringInfoString(s, v->trig_calls);
	}
}


static void
clear_nodeval(node_vals *vals)
{
	memset(vals, 0, sizeof(node_vals));
}

static void
json_text_objstart(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	ctx->level++;
}

static void
json_text_objend(void *state)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	/* Print current node if the object is a P_Plan or a child of P_Plans */
	if (bms_is_member(ctx->level - 1, ctx->plan_levels))
	{
		print_current_node(ctx);
		clear_nodeval(ctx->nodevals);
	}
	else if (ctx->section == P_Triggers)
	{
		print_current_trig_node(ctx);
		clear_nodeval(ctx->nodevals);
	}
	else if (ctx->current_list == P_TargetTables)
	{
		/* Move the current working taget tables into nodevals */
		node_vals *v = ctx->nodevals;

		if (!ctx->work_str)
			ctx->work_str = makeStringInfo();

		resetStringInfo(ctx->work_str);
		appendStringInfoString(ctx->work_str, v->operation);
		print_obj_name0(ctx->work_str, v->obj_name, v->schema_name, v->alias);
		v->target_tables = lappend(v->target_tables,
								   pstrdup(ctx->work_str->data));
		resetStringInfo(ctx->work_str);
	}

	ctx->last_elem_is_object = true;
	ctx->level--;
}

static void
json_text_ofstart(void *state, char *fname, bool isnull)
{
	word_table *p;
	pgspParserContext *ctx = (pgspParserContext *)state;

	ctx->setter = NULL;
	p = search_word_table(propfields, fname, PGSP_JSON_TEXTIZE);

	if (!p)
	{
		ereport(DEBUG1,
				(errmsg("Short JSON parser encoutered unknown field name: \"%s\", skipped.", fname),
				 errdetail_log("INPUT: \"%s\"", ctx->org_string)));
	}
	else
	{
		/*
		 * Print node immediately if the next level of Plan/Plans comes. The
		 * plan construct is tail-recursive so this doesn't harm.
		 */
		if (p->tag == P_Plan || p->tag == P_Plans)
		{
			print_current_node(ctx);
			clear_nodeval(ctx->nodevals);
		}
		else if (p->tag == P_TargetTables)
		{
			node_vals *v = ctx->nodevals;

			ctx->current_list = p->tag;
			ctx->list_fname = fname;

			/* stash some data */
			v->tmp_obj_name = v->obj_name;
			v->tmp_schema_name = v->schema_name;
			v->tmp_alias = v->alias;
		}

		/*
		 * This paser prints partial result at the end of every P_Plan object,
		 * which includes elements in P_Plans list.
		 */
		if (p->tag == P_Plan || p->tag == P_Plans)
			ctx->plan_levels = bms_add_member(ctx->plan_levels, ctx->level);
		else
			ctx->plan_levels = bms_del_member(ctx->plan_levels, ctx->level);

		if (p->tag == P_Plan || p->tag == P_Triggers)
			ctx->section = p->tag;
		ctx->setter = p->setter;
	}
}

static void
json_text_ofend(void *state, char *fname, bool isnull)
{
	pgspParserContext *ctx = (pgspParserContext *)state;
	node_vals *v = ctx->nodevals;

	/* We assume that lists with same fname will not be nested */
	if (ctx->list_fname && strcmp(fname, ctx->list_fname) == 0)
	{
		/* Restore stashed data, see json_text_ofstart */
		if (ctx->current_list == P_TargetTables)
		{
			v->obj_name = v->tmp_obj_name;
			v->schema_name = v->tmp_schema_name;
			v->alias = v->tmp_alias;
		}
		ctx->list_fname = NULL;
		ctx->current_list = P_Invalid;
	}

	/* Planning/Execution time to be appeared at the end of plan */
	if (HASSTRING(v->plan_time) ||
		HASSTRING(v->exec_time))
	{
		if (HASSTRING(v->plan_time))
		{
			appendStringInfoString(ctx->dest, "\nPlanning Time: ");
			appendStringInfoString(ctx->dest, v->plan_time);
			appendStringInfoString(ctx->dest, " ms");
		}
		else
		{
			appendStringInfoString(ctx->dest, "\nExecution Time: ");
			appendStringInfoString(ctx->dest, v->exec_time);
			appendStringInfoString(ctx->dest, " ms");
		}
		clear_nodeval(v);
	}
}

static void
json_text_scalar(void *state, char *token, JsonTokenType tokentype)
{
	pgspParserContext *ctx = (pgspParserContext *)state;

	if (ctx->setter)
		ctx->setter(ctx->nodevals, token);
}

char *
pgsp_json_textize(char *json)
{
	JsonLexContext lex;
	JsonSemAction sem;
	pgspParserContext	ctx;

	init_json_lex_context(&lex, json);
	init_parser_context(&ctx, PGSP_JSON_TEXTIZE, json, NULL, 0);

	ctx.nodevals = (node_vals*)palloc0(sizeof(node_vals));

	sem.semstate = (void*)&ctx;
	sem.object_start       = json_text_objstart;
	sem.object_end         = json_text_objend;
	sem.array_start        = NULL;
	sem.array_end          = NULL;
	sem.object_field_start = json_text_ofstart;
	sem.object_field_end   = json_text_ofend;
	sem.array_element_start= NULL;
	sem.array_element_end  = NULL;
	sem.scalar             = json_text_scalar;


	if (!run_pg_parse_json(&lex, &sem))
	{
		if (ctx.nodevals->node_type)
			print_current_node(&ctx);

		if (ctx.dest->len > 0 &&
			ctx.dest->data[ctx.dest->len - 1] != '\n')
			appendStringInfoChar(ctx.dest, '\n');
		
		if (ctx.dest->len == 0)
			appendStringInfoString(ctx.dest, "<Input was not JSON>");
		else
			appendStringInfoString(ctx.dest, "<truncated>");
	}

	pfree(ctx.nodevals);

	return ctx.dest->data;
}

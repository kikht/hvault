#include <postgres.h>
#include <catalog/pg_namespace.h>
#include <catalog/pg_operator.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <commands/defrem.h>
#include <foreign/foreign.h>
#include <nodes/nodeFuncs.h>
#include <nodes/pg_list.h>
#include <nodes/primnodes.h>
#include <nodes/relation.h>
#include <optimizer/cost.h>
#include <optimizer/restrictinfo.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/planmain.h>
#include <tcop/tcopprot.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "hvault.h"

#define POINT_SIZE 32
#define FOOTPRINT_SIZE 120
#define STARTUP_COST 10
#define PIXEL_COST 0.001

#define GEOMETRY_OP_QUERY \
    "SELECT o.oid from pg_opclass c" \
        " JOIN pg_amop ao ON c.opcfamily = ao.amopfamily" \
        " JOIN pg_operator o ON ao.amopopr = o.oid " \
        " WHERE c.opcname = 'gist_geometry_ops_2d' AND o.oprname = $1"

/* 
 * This file includes routines involved in query planning
 */

typedef struct 
{
    Index relid;      
    AttrNumber natts;
    HvaultColumnType *coltypes;
    char *catalog;

    Oid geomopers[HvaultGeomNumRealOpers];
} HvaultTableInfo;

typedef struct 
{
    HvaultTableInfo const *table;
    List *own_quals;
    List *fdw_expr;
    char const * filter;
    char const * sort;
    List *predicates;
} HvaultPathData;

typedef struct
{
    HvaultTableInfo const *table;
    List *fdw_expr;
    StringInfoData query;
    bool first_qual;
} HvaultDeparseContext;

typedef struct {
    HvaultGeomOperator op;
    bool isneg;
} GeomPredicateDesc;

typedef struct {
    RestrictInfo *rinfo;
    Var *var;
    Expr *arg;
    HvaultColumnType coltype;
    GeomPredicateDesc pred;
    GeomPredicateDesc catalog_pred;
} GeomOpQual;

static int  get_row_width(HvaultColumnType *coltypes, AttrNumber numattrs);
static void getSortPathKeys(PlannerInfo const *root,
                            RelOptInfo const *baserel,
                            List **pathkeys_list,
                            List **sort_part);
static bool bms_equal_any(Relids relids, List *relids_list);
static void getGeometryOpers(HvaultTableInfo *table);
static void extractCatalogQuals(PlannerInfo *root,
                                RelOptInfo *baserel,
                                HvaultTableInfo const *table,
                                List **catalog_quals,
                                List **catalog_joins,
                                List **catalog_ec,
                                List **catalog_ec_vars,
                                List **footprint_quals,
                                List **footprint_joins);
static bool isFootprintQual(Expr *expr, 
                            const HvaultTableInfo *table, 
                            GeomOpQual *qual);
static void addForeignPaths(PlannerInfo *root, 
                            RelOptInfo *baserel, 
                            HvaultTableInfo const *table,
                            List *pathkeys_list,
                            List *sort_qual_list,
                            List *catalog_quals,
                            List *footprint_quals,
                            Relids req_outer);
static void generateJoinPath(PlannerInfo *root, 
                             RelOptInfo *baserel, 
                             HvaultTableInfo const *table,
                             List *pathkeys_list,
                             List *sort_qual_list,
                             List *catalog_quals,
                             List *catalog_joins,
                             List *footprint_quals,
                             List *footprint_joins,
                             Relids relids,
                             List **considered_relids);
static int insertFdwExpr(List **fdw_expr, Expr *node);
static void deparseExpr(Expr *node, HvaultDeparseContext *ctx);
static void deparseFootprintExpr(GeomOpQual *qual, 
                                 HvaultDeparseContext *ctx);
static void addDeparseItem(HvaultDeparseContext *ctx);

/* 
 * This function is intened to provide first estimate of a size of relation
 * involved in query. Generally we have all query information available at 
 * planner stage and can build our estimate basing on:
 *  baserel->reltargetlist - List of Var and PlaceHolderVar nodes for 
 *      the values we need to output from this relation. 
 *  baserel->baserestrictinfo - 
 *      List of RestrictInfo nodes, containing info about
 *      each non-join qualification clause in which this relation
 *      participates 
 *  baserel->joininfo - 
 *      List of RestrictInfo nodes, containing info about each
 *      join clause in which this relation participates 
 *  root->eq_classes - 
 *      List of EquivalenceClass nodes that were extracted from query.
 *      These lists are transitive closures of of the expressions like a = b
 *
 * Currently our estimate is quite simple:
 * rows = (total number of files in catalog) * (max tuples per file)
 * 
 * To get row width we detect unused columns here and sum the size of others
 * (we actually know the size of every column we can emit)
 *
 * TODO: we should extract catalog quals to get better estimate of number
 *       of files involved in query.
 */
void 
hvaultGetRelSize(PlannerInfo *root, 
                 RelOptInfo *baserel, 
                 Oid foreigntableid)
{
    HvaultTableInfo *fdw_private;
    double num_files, tuples_per_file, scale_factor;
    Relation rel;
    TupleDesc tupleDesc;

    fdw_private = (HvaultTableInfo *) palloc(sizeof(HvaultTableInfo));

    rel = heap_open(foreigntableid, AccessShareLock);
    tupleDesc = RelationGetDescr(rel);
    fdw_private->natts = tupleDesc->natts;
    heap_close(rel, AccessShareLock);    
    
    fdw_private->relid = baserel->relid;
    fdw_private->coltypes = hvaultGetUsedColumns(root, baserel, foreigntableid, 
                                                 fdw_private->natts);
    fdw_private->catalog = hvaultGetTableOptionString(foreigntableid, 
                                                      "catalog");

    /* TODO: Use constant catalog quals for better estimate */
    num_files = hvaultGetNumFiles(fdw_private->catalog);
    tuples_per_file = HVAULT_TUPLES_PER_FILE;
    scale_factor = 1; /* 4 for 500m, 16 for 250m */

    baserel->width = get_row_width(fdw_private->coltypes, fdw_private->natts);
    baserel->rows = num_files * tuples_per_file * scale_factor ; 
    baserel->fdw_private = fdw_private;

    elog(DEBUG2, "GetRelSize: baserestrictinfo: %s\njoininfo: %s",
         nodeToString(baserel->baserestrictinfo),
         nodeToString(baserel->joininfo));
}

void 
hvaultGetPaths(PlannerInfo *root, 
               RelOptInfo *baserel,
               Oid foreigntableid)
{
    HvaultTableInfo *fdw_private = (HvaultTableInfo *) baserel->fdw_private;
    ListCell *l, *m, *k, *s;
    List *pathkeys_list = NIL;  
    List *sort_qual_list = NIL;
    List *catalog_quals = NIL;
    List *catalog_joins = NIL;
    List *catalog_ec = NIL;
    List *catalog_ec_vars = NIL;
    List *footprint_quals = NIL;
    List *footprint_joins = NIL;
    List *considered_relids = NIL;
    List *all_joins = NIL;
    int considered_clauses;

    /* Process pathkeys */
    if (has_useful_pathkeys(root, baserel))
    {
        getSortPathKeys(root, baserel, &pathkeys_list, &sort_qual_list);
    }
    if (list_length(pathkeys_list) == 0)
    {
        pathkeys_list = list_make1(NIL);
        sort_qual_list = list_make1(NULL);
    }

    getGeometryOpers(fdw_private);
    extractCatalogQuals(root, baserel, fdw_private, &catalog_quals, 
                        &catalog_joins, &catalog_ec, &catalog_ec_vars,
                        &footprint_quals, &footprint_joins);
    
    /* Create simple unparametrized path */
    addForeignPaths(root, baserel, fdw_private, pathkeys_list, sort_qual_list, 
                    catalog_quals, footprint_quals, NULL);
    
    /* Create parametrized join paths */
    all_joins = list_copy(catalog_joins);
    foreach(l, footprint_joins)
    {
        GeomOpQual *qual = lfirst(l);
        all_joins = lappend(all_joins, qual->rinfo);
    }
    considered_clauses = list_length(all_joins) + list_length(catalog_ec);
    foreach(l, all_joins)
    {
        RestrictInfo *rinfo = lfirst(l);
        Relids clause_relids = rinfo->clause_relids;
        if (bms_equal_any(clause_relids, considered_relids))
            continue;

        foreach(m, considered_relids)
        {
            Relids oldrelids = (Relids) lfirst(m);

            /*
             * If either is a subset of the other, no new set is possible.
             * This isn't a complete test for redundancy, but it's easy and
             * cheap.  get_join_index_paths will check more carefully if we
             * already generated the same relids set.
             */
            if (bms_subset_compare(clause_relids, oldrelids) != BMS_DIFFERENT)
                continue;

            /*
             * If the number of relid sets considered exceeds our heuristic
             * limit, stop considering combinations of clauses.  We'll still
             * consider the current clause alone, though (below this loop).
             */
            if (list_length(considered_relids) >= 10 * considered_clauses)
                break;       

            generateJoinPath(root, baserel, fdw_private, 
                             pathkeys_list, sort_qual_list,
                             catalog_quals, catalog_joins, 
                             footprint_quals, footprint_joins,
                             bms_union(oldrelids, clause_relids),
                             &considered_relids);
        }

        generateJoinPath(root, baserel, fdw_private,
                         pathkeys_list, sort_qual_list,
                         catalog_quals, catalog_joins, 
                         footprint_quals, footprint_joins,
                         clause_relids,
                         &considered_relids);
    }

    /* Derive join paths from EC */
    forboth(l, catalog_ec, s, catalog_ec_vars)
    {
        EquivalenceClass *ec = (EquivalenceClass *) lfirst(l);
        Var *catalog_var = (Var *) lfirst(s);
        if (catalog_var == NULL)
            continue;

        foreach(m, ec->ec_members)
        {
            EquivalenceMember *em = (EquivalenceMember *) lfirst(m);
            Var *var;
            Relids relids;

            if (!IsA(em->em_expr, Var))
                continue;

            var = (Var *) em->em_expr;
            if (var->varno == baserel->relid)
                continue;

            relids = bms_make_singleton(catalog_var->varno);
            relids = bms_add_member(relids, var->varno);
            if (bms_equal_any(relids, considered_relids))
                continue;

            foreach(k, considered_relids)
            {
                Relids oldrelids = (Relids) lfirst(k);
                Relids union_relids;
                
                if (bms_is_member(var->varno, oldrelids))                
                    continue;

                if (list_length(considered_relids) >= 10 * considered_clauses)
                    break;

                union_relids = bms_copy(oldrelids);
                union_relids = bms_add_member(union_relids, var->varno);
                generateJoinPath(root, baserel, fdw_private,
                                 pathkeys_list, sort_qual_list,
                                 catalog_quals, catalog_joins, 
                                 footprint_quals, footprint_joins,
                                 union_relids, &considered_relids);
            }

            generateJoinPath(root, baserel, fdw_private, 
                             pathkeys_list, sort_qual_list,
                             catalog_quals, catalog_joins, 
                             footprint_quals, footprint_joins,
                             relids, &considered_relids);        
        }
    }
}

ForeignScan *
hvaultGetPlan(PlannerInfo *root, 
              RelOptInfo *baserel,
              Oid foreigntableid, 
              ForeignPath *best_path,
              List *tlist, 
              List *scan_clauses)
{
    HvaultPathData *fdw_private = (HvaultPathData *) best_path->fdw_private; 

    List *rest_clauses = NIL; /* clauses that must be checked externally */
    List *fdw_plan_private = NIL;
    ListCell *l;
    List *coltypes;
    AttrNumber attnum;
    StringInfoData query;

    elog(DEBUG1, "Selected path quals: %s", 
         nodeToString(fdw_private->own_quals));

    /* Prepare catalog query */
    initStringInfo(&query);
    appendStringInfoString(&query, fdw_private->filter);
    if (fdw_private->sort)
    {
        appendStringInfoString(&query, " ORDER BY ");
        appendStringInfoString(&query, fdw_private->sort);
    }

    elog(DEBUG1, "Catalog query: %s", query.data);


    /* Extract clauses that need to be checked externally */
    foreach(l, scan_clauses)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
        if (!list_member_ptr(fdw_private->own_quals, rinfo))
        {
            rest_clauses = lappend(rest_clauses, rinfo);
        }
        else
        {
            elog(DEBUG1, "Skipping external clause %s", 
                 nodeToString(rinfo->clause));
        }
    }
    rest_clauses = extract_actual_clauses(rest_clauses, false);

    elog(DEBUG2, "GetPlan: scan_cl: %s\nrest_cl: %s",
         nodeToString(scan_clauses),
         nodeToString(rest_clauses));
    elog(DEBUG3, "GetPlan: tlist: %s", nodeToString(tlist));

    /* store fdw_private in List */
    coltypes = NIL;
    for (attnum = 0; attnum < fdw_private->table->natts; attnum++)
    {
        coltypes = lappend_int(coltypes, fdw_private->table->coltypes[attnum]);
    }
    fdw_plan_private = lappend(fdw_plan_private, makeString(query.data));
    fdw_plan_private = lappend(fdw_plan_private, coltypes);
    fdw_plan_private = lappend(fdw_plan_private, fdw_private->predicates);

    return make_foreignscan(tlist, rest_clauses, baserel->relid, 
                            fdw_private->fdw_expr, fdw_plan_private);
}

static int
get_row_width(HvaultColumnType *coltypes, AttrNumber numattrs)
{
    int width = 0;
    AttrNumber i;

    for (i = 0; i < numattrs; ++i)
    {
        switch (coltypes[i]) {
            case HvaultColumnFloatVal:
                width += sizeof(double);
                break;
            case HvaultColumnInt8Val:
                width += sizeof(int8_t);
                break;
            case HvaultColumnInt16Val:
                width += sizeof(int16_t);
                break;
            case HvaultColumnInt32Val:
                width += sizeof(int32_t);
                break;
            case HvaultColumnInt64Val:
                width += sizeof(int64_t);
                break;
            case HvaultColumnFileIdx:
            case HvaultColumnLineIdx:
            case HvaultColumnSampleIdx:
                width += 4;
                break;
            case HvaultColumnPoint:
                width += POINT_SIZE;
                break;
            case HvaultColumnFootprint:
                width += FOOTPRINT_SIZE;
                break;
            case HvaultColumnTime:
                width += 8;
            case HvaultColumnNull:
                /* nop */
                break;
            default:
                ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Undefined column type")));
        }
    }
    return width;
}

static List *
makePathKeys(EquivalenceClass *base_ec, 
             Oid base_opfamily, 
             int base_strategy,
             EquivalenceClass *line_ec,
             EquivalenceClass *sample_ec,
             Oid intopfamily)
{
    List *result = NIL;
    PathKey *pk = makeNode(PathKey);
    pk->pk_eclass = base_ec;
    pk->pk_nulls_first = false;
    pk->pk_strategy = base_strategy;
    pk->pk_opfamily = base_opfamily;
    result = lappend(result, pk);

    if (line_ec)
    {
        PathKey *pk = makeNode(PathKey);
        pk->pk_eclass = line_ec;
        pk->pk_nulls_first = false;
        pk->pk_strategy = BTLessStrategyNumber;
        pk->pk_opfamily = intopfamily;
        result = lappend(result, pk);

        if (sample_ec)
        {
            PathKey *pk = makeNode(PathKey);
            pk->pk_eclass = sample_ec;
            pk->pk_nulls_first = false;
            pk->pk_strategy = BTLessStrategyNumber;
            pk->pk_opfamily = intopfamily;
            result = lappend(result, pk);            
        }
    }

    return result;
}

static void   
getSortPathKeys(PlannerInfo const *root,
                RelOptInfo const *baserel,
                List **pathkeys_list,
                List **sort_part)
{
    List *pathkeys;
    ListCell *l, *m;
    Oid intopfamily;
    HvaultTableInfo *fdw_private = (HvaultTableInfo *) baserel->fdw_private;
    EquivalenceClass *file_ec = NULL, *line_ec = NULL, 
                     *sample_ec = NULL, *time_ec = NULL;

    foreach(l, root->eq_classes)
    {
        EquivalenceClass *ec = (EquivalenceClass *) lfirst(l);
        elog(DEBUG2, "processing EquivalenceClass %s", nodeToString(ec));
        if (!bms_is_member(baserel->relid, ec->ec_relids))
        {
            elog(DEBUG1, "not my relid");
            continue;
        }

        foreach(m, ec->ec_members)
        {
            Var *var;
            EquivalenceMember *em = (EquivalenceMember *) lfirst(m);

            if (!IsA(em->em_expr, Var))
                continue;
        
            var = (Var *) em->em_expr;
            if (baserel->relid != var->varno)
                continue;
            
            EquivalenceClass **dest_ec = NULL;
            Assert(var->varattno < fdw_private->natts);
            switch(fdw_private->coltypes[var->varattno-1])
            {
                case HvaultColumnFileIdx:
                    dest_ec = &file_ec;
                    break;
                case HvaultColumnLineIdx:
                    dest_ec = &line_ec;
                    break;
                case HvaultColumnSampleIdx:
                    dest_ec = &sample_ec;
                    break;
                case HvaultColumnTime:
                    dest_ec = &time_ec;
                    break;
                default:
                    /* nop */
                    break;
            }

            if (dest_ec)
            {
                if (*dest_ec)
                {
                    elog(ERROR, "Duplicate special column");
                    return; /* Will never reach here */
                }

                *dest_ec = ec;
            }
        }
    }

    intopfamily  = get_opfamily_oid(BTREE_AM_OID, 
                                    list_make1(makeString("integer_ops")), 
                                    false);
    if (file_ec)
    {
        pathkeys = makePathKeys(file_ec, intopfamily, BTLessStrategyNumber, 
                                line_ec, sample_ec, intopfamily);
        *pathkeys_list = lappend(*pathkeys_list, pathkeys);
        *sort_part = lappend(*sort_part, "file_id ASC");

        pathkeys = makePathKeys(file_ec, intopfamily, BTGreaterStrategyNumber, 
                                line_ec, sample_ec, intopfamily);
        *pathkeys_list = lappend(*pathkeys_list, pathkeys);
        *sort_part = lappend(*sort_part, "file_id DESC");
    }

    if (time_ec)
    {
        Oid timeopfamily = get_opfamily_oid(
            BTREE_AM_OID, list_make1(makeString("datetime_ops")), false);    

        pathkeys = makePathKeys(time_ec, timeopfamily, BTLessStrategyNumber, 
                                line_ec, sample_ec, intopfamily);
        *pathkeys_list = lappend(*pathkeys_list, pathkeys);
        *sort_part = lappend(*sort_part, "starttime ASC");

        pathkeys = makePathKeys(time_ec, intopfamily, BTGreaterStrategyNumber, 
                                line_ec, sample_ec, intopfamily);
        *pathkeys_list = lappend(*pathkeys_list, pathkeys);
        *sort_part = lappend(*sort_part, "starttime DESC");
    }
}

static inline bool 
isCatalogVar(HvaultColumnType type)
{
    return type == HvaultColumnFileIdx || type == HvaultColumnTime;
}

typedef struct
{
    HvaultTableInfo const table;
} CatalogQualsContext;

static bool 
isCatalogQualWalker(Node *node, CatalogQualsContext *ctx)
{
    if (node == NULL)
        return false;

    switch (nodeTag(node))
    {
        case T_Var:
            {
                /*
                 * If Var is in our table, then check its type and correct 
                 * condition type if necessary.
                 * Ignore other tables, because this function can also be used 
                 * with join conditions.
                 */
                Var *var = (Var *) node;
                if (var->varno == ctx->table.relid)
                {
                    if (!isCatalogVar(ctx->table.coltypes[var->varattno-1]))
                        return true;
                }
            }
            break;
        case T_Param:
        case T_ArrayRef:
        case T_FuncExpr:
        case T_Const:
        case T_OpExpr:
        case T_DistinctExpr:
        case T_ScalarArrayOpExpr:
        case T_RelabelType:
        case T_BoolExpr:
        case T_NullTest:
        case T_ArrayExpr:
        case T_List:
            /* OK */
            break;
        default:
            /*
             * If it's anything else, assume it's unsafe.  This list can be
             * expanded later, but don't forget to add deparse support below.
             */
            return true;
    }

    /* Recurse to examine sub-nodes */
    return expression_tree_walker(node, isCatalogQualWalker, (void *) ctx);
}

/* Little trick to ensure that table info is const */
static inline bool 
isCatalogQual(Expr *expr, HvaultTableInfo const *table)
{
    return !isCatalogQualWalker((Node *) expr, (CatalogQualsContext *) table);
}

/* Catalog only EC will be put into baserestrictinfo by planner, so here
 * we need to extract only ECs that contain both catalog & outer table vars.
 * We skip patalogic case when one EC contains two different catalog vars
 */
static Var *
isCatalogJoinEC(EquivalenceClass *ec, HvaultTableInfo const *table)
{
    ListCell *l;
    Var *catalog_var = NULL;
    int num_outer_vars = 0;

    foreach(l, ec->ec_members)
    {
        EquivalenceMember *em = (EquivalenceMember *) lfirst(l);
        if (IsA(em->em_expr, Var))
        {
            Var *var = (Var *) em->em_expr;
            if (var->varno == table->relid)
            {
                if (isCatalogVar(table->coltypes[var->varattno-1]))
                {
                    if (catalog_var == NULL)
                    {
                        catalog_var = var;
                    }
                    else
                    {
                        return NULL;
                    }
                }
                else
                {
                    return NULL;
                }
            }
            else
            {
                num_outer_vars++;
            }
        }
        else
        {
            /* Should be Const here, but check it in more general way */
            if (!isCatalogQual(em->em_expr, table))
                return 0;
        }
    }
    return num_outer_vars > 0 && catalog_var ? catalog_var : NULL;
}

static void
extractCatalogQuals(PlannerInfo *root,
                    RelOptInfo *baserel,
                    HvaultTableInfo const *table,
                    List **catalog_quals,
                    List **catalog_joins,
                    List **catalog_ec,
                    List **catalog_ec_vars,
                    List **footprint_quals,
                    List **footprint_joins)
{
    ListCell *l;
    GeomOpQual *fqual = palloc(sizeof(GeomOpQual));

    foreach(l, baserel->baserestrictinfo)
    {
        RestrictInfo *rinfo = lfirst(l);
        if (isCatalogQual(rinfo->clause, table))
        {
            elog(DEBUG2, "Detected catalog qual %s", 
                 nodeToString(rinfo->clause));
            *catalog_quals = lappend(*catalog_quals, rinfo);
        } 
        else if (isFootprintQual(rinfo->clause, table, fqual))
        {
            elog(DEBUG2, "Detected footprint qual %s", 
                 nodeToString(rinfo->clause));
            fqual->rinfo = rinfo;
            *footprint_quals = lappend(*footprint_quals, fqual);
            fqual = palloc(sizeof(GeomOpQual));
        }
    }

    foreach(l, baserel->joininfo)
    {
        RestrictInfo *rinfo = lfirst(l);
        if (isCatalogQual(rinfo->clause, table))
        {
            *catalog_joins = lappend(*catalog_joins, rinfo);
        }
        else if (isFootprintQual(rinfo->clause, table, fqual))
        {
            elog(DEBUG2, "Detected footprint join %s", 
                 nodeToString(rinfo->clause));
            fqual->rinfo = rinfo;
            *footprint_joins = lappend(*footprint_joins, fqual);
            fqual = palloc(sizeof(GeomOpQual));
        }
    }
    pfree(fqual);

    foreach(l, root->eq_classes)
    {
        EquivalenceClass *ec = (EquivalenceClass *) lfirst(l);
        Var *catalog_var = isCatalogJoinEC(ec, table);
        if (catalog_var != NULL)
        {
            *catalog_ec = lappend(*catalog_ec, ec);
            *catalog_ec_vars = lappend(*catalog_ec_vars, catalog_var);
        }
    }
}

static bool 
bms_equal_any(Relids relids, List *relids_list)
{
    ListCell   *lc;

    foreach(lc, relids_list)
    {
        if (bms_equal(relids, (Relids) lfirst(lc)))
            return true;
    }
    return false;
}

static void
getQueryCosts(char const *query,
              int nargs, 
              Oid *argtypes, 
              Cost *startup_cost,
              Cost *total_cost,
              double *plan_rows,
              int *plan_width)
{
    MemoryContext oldmemctx, memctx;
    List *parsetree, *stmt_list;
    PlannedStmt *plan;

    memctx = AllocSetContextCreate(CurrentMemoryContext, 
                                   "hvaultQueryCosts context", 
                                   ALLOCSET_DEFAULT_MINSIZE,
                                   ALLOCSET_DEFAULT_INITSIZE,
                                   ALLOCSET_DEFAULT_MAXSIZE);
    oldmemctx = MemoryContextSwitchTo(memctx);

    parsetree = pg_parse_query(query);
    Assert(list_length(parsetree) == 1);
    stmt_list = pg_analyze_and_rewrite(linitial(parsetree), 
                                       query, 
                                       argtypes, 
                                       nargs);
    Assert(list_length(stmt_list) == 1);
    plan = pg_plan_query((Query *) linitial(stmt_list), 
                         CURSOR_OPT_GENERIC_PLAN, 
                         NULL);

    *startup_cost = plan->planTree->startup_cost;
    *total_cost = plan->planTree->total_cost;
    *plan_rows = plan->planTree->plan_rows;
    *plan_width = plan->planTree->plan_width;

    MemoryContextSwitchTo(oldmemctx);
    MemoryContextDelete(memctx);
}

static List *
predicateToList(HvaultGeomPredicate *p)
{
    return list_make4_int(p->coltype, p->op, p->argno, p->isneg);
}

static void addForeignPaths(PlannerInfo *root, 
                           RelOptInfo *baserel, 
                           HvaultTableInfo const *table,
                           List *pathkeys_list,
                           List *sort_qual_list,
                           List *catalog_quals,
                           List *footprint_quals,
                           Relids req_outer)
{
    double rows;
    Cost startup_cost, total_cost;
    ListCell *l, *m;
    HvaultDeparseContext deparse_ctx;
    Cost catmin, catmax;
    double catrows;
    int catwidth;
    int nargs, argno;
    Oid *argtypes;
    List *predicates, *own_quals, *pred_quals;
    Selectivity selectivity;

    own_quals = list_copy(catalog_quals);
    /* Prepare catalog query */
    deparse_ctx.table = table;
    deparse_ctx.fdw_expr = NIL;
    deparse_ctx.first_qual = true;
    initStringInfo(&deparse_ctx.query);
    
    appendStringInfoString(&deparse_ctx.query, HVAULT_CATALOG_QUERY_PREFIX);
    appendStringInfoString(&deparse_ctx.query, table->catalog);
    appendStringInfoChar(&deparse_ctx.query, ' ');

    foreach(l, catalog_quals)
    {   
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
        addDeparseItem(&deparse_ctx);
        deparseExpr(rinfo->clause, &deparse_ctx);
    }

    predicates = NIL;
    pred_quals = NIL;
    foreach(l, footprint_quals)
    {
        GeomOpQual *qual = (GeomOpQual *) lfirst(l);
        HvaultGeomPredicate pred;
        if (qual->catalog_pred.op != HvaultGeomInvalidOp)
        {
            addDeparseItem(&deparse_ctx);
            deparseFootprintExpr(qual, &deparse_ctx);
        }
        pred.coltype = qual->coltype;
        pred.op = qual->pred.op;
        pred.isneg = qual->pred.isneg;
        pred.argno = insertFdwExpr(&deparse_ctx.fdw_expr, qual->arg);
        predicates = lappend(predicates, predicateToList(&pred));
        own_quals = lappend(own_quals, qual->rinfo);
        pred_quals = lappend(pred_quals, qual->rinfo);
    }

    nargs = list_length(deparse_ctx.fdw_expr);
    argtypes = palloc(sizeof(Oid) * nargs);
    argno = 0;
    foreach(l, deparse_ctx.fdw_expr)
    {
        Node *expr = (Node *) lfirst(l);
        argtypes[argno] = exprType(expr);
        argno++;
    }
    getQueryCosts(deparse_ctx.query.data, 
                  nargs, 
                  argtypes, 
                  &catmin, 
                  &catmax, 
                  &catrows, 
                  &catwidth);
    pfree(argtypes);
    argtypes = NULL;

    selectivity = clauselist_selectivity(root, pred_quals, baserel->relid, 
                                         JOIN_INNER, NULL);
    rows = catrows * selectivity * HVAULT_TUPLES_PER_FILE;
    startup_cost = catmin + STARTUP_COST;
    total_cost = catmax + STARTUP_COST + rows * PIXEL_COST;
    forboth(l, pathkeys_list, m, sort_qual_list)
    {
        List *pathkeys = (List *) lfirst(l);
        char *sort_qual = (char *) lfirst(m);

        if (add_path_precheck(baserel, startup_cost, total_cost, 
                              pathkeys, req_outer))
        {
            ForeignPath *path;
            HvaultPathData *path_data;

            path_data = palloc(sizeof(HvaultPathData));    
            path_data->table = table;
            path_data->own_quals = own_quals;
            path_data->filter = deparse_ctx.query.data;
            path_data->sort = sort_qual;
            path_data->fdw_expr = deparse_ctx.fdw_expr;
            path_data->predicates = predicates;

            path = create_foreignscan_path(root, baserel, rows, 
                                           startup_cost, total_cost, pathkeys, 
                                           req_outer, (List *) path_data);
            add_path(baserel, (Path *) path);
        }
    }
}

static void   
generateJoinPath(PlannerInfo *root, 
                 RelOptInfo *baserel, 
                 HvaultTableInfo const *table,
                 List *pathkeys_list,
                 List *sort_qual_list,
                 List *catalog_quals,
                 List *catalog_joins,
                 List *footprint_quals,
                 List *footprint_joins,
                 Relids relids,
                 List **considered_relids)
{
    ListCell *l;
    List *quals = list_copy(catalog_quals);
    List *fquals = list_copy(footprint_quals);
    List *ec_quals;
    Relids req_outer;

    req_outer = bms_copy(relids);
    req_outer = bms_del_member(req_outer, baserel->relid);
    if (bms_is_empty(req_outer))
    {
        elog(WARNING, "Considering strange relids");
        return;
    }

    foreach(l, catalog_joins)
    {
        RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
        if (bms_is_subset(rinfo->clause_relids, relids))
            quals = lappend(quals, rinfo);
    }

    foreach(l, footprint_joins)
    {
        GeomOpQual *qual = (GeomOpQual *) lfirst(l);
        if (bms_is_subset(qual->rinfo->clause_relids, relids))
            fquals = lappend(fquals, qual);   
    }

    ec_quals = generate_join_implied_equalities(root, relids, req_outer, 
                                                baserel);
    quals = list_concat(quals, ec_quals);
    addForeignPaths(root, baserel, table, pathkeys_list, sort_qual_list, 
                    quals, fquals, req_outer);

    *considered_relids = lcons(relids, *considered_relids);
}


/* -----------------------------------
 *
 * Deparse expression functions
 *
 * These functions examine query clauses and translate them to corresponding 
 * catalog query clauses. It is intended to reduce number of files we need to 
 * scan for the query. Lots of deparse functions are ported from 
 * contrib/postgres_fdw/deparse.c and ruleutils.c. It seems that deparsing 
 * API should be extended somehow to handle such cases.
 *
 * -----------------------------------
 */

static int 
insertFdwExpr(List **fdw_expr, Expr *node)
{
    ListCell *l;
    int idx = 0;

    foreach(l, *fdw_expr)
    {
        if (lfirst(l) == node)
            break;
        idx++;
    }
    if (idx == list_length(*fdw_expr))
        *fdw_expr = lappend(*fdw_expr, node);
    return idx;
}


/* 
 * Deparse expression as runtime computable parameter. 
 * Parameter index directly maps to fdw_expr.
 */
static void 
deparseParameter(Expr *node, HvaultDeparseContext *ctx)
{
    int idx = insertFdwExpr(&ctx->fdw_expr, node);
    appendStringInfo(&ctx->query, "$%d", idx + 1);
}

/*
 * Deparse variable expression. 
 * Local file_id and time variables are mapped to corresponding catalog column.
 * Other local variables are not supported. Footprint variables are processed 
 * separately. Other's table variables are handled as runtime computable 
 * parameters, this is the case of parametrized join path.
 */
static void
deparseVar(Var *node, HvaultDeparseContext *ctx)
{
    if (node->varno == ctx->table->relid)
    {
        /* Catalog column */
        HvaultColumnType type;
        char *colname = NULL;
        
        Assert(node->varattno > 0);
        Assert(node->varattno <= ctx->table->natts);

        type = ctx->table->coltypes[node->varattno-1];
        switch(type)
        {
            case HvaultColumnTime:
                colname = "starttime";
                
            break;
            case HvaultColumnFileIdx:
                colname = "file_id";
            break;
            default:
                elog(ERROR, "unsupported local column type: %d", type);
        }
        appendStringInfoString(&ctx->query, quote_identifier(colname));
    }
    else
    {
        /* Other table's column, use as parameter */
        deparseParameter((Expr *) node, ctx);
    }
}

static void
deparseFuncExpr(FuncExpr *node, HvaultDeparseContext *ctx)
{
    HeapTuple proctup;
    Form_pg_proc procform;
    const char *proname;
    bool first;
    ListCell *arg;

    /*
     * If the function call came from an implicit coercion, then just show the
     * first argument.
     */
    if (node->funcformat == COERCE_IMPLICIT_CAST)
    {
        deparseExpr((Expr *) linitial(node->args), ctx);
        return;
    }

    /*
     * If the function call came from a cast, then show the first argument
     * plus an explicit cast operation.
     */
    if (node->funcformat == COERCE_EXPLICIT_CAST)
    {
        Oid rettype = node->funcresulttype;
        int32_t coercedTypmod;

        /* Get the typmod if this is a length-coercion function */
        (void) exprIsLengthCoercion((Node *) node, &coercedTypmod);

        deparseExpr((Expr *) linitial(node->args), ctx);
        appendStringInfo(&ctx->query, "::%s",
                         format_type_with_typemod(rettype, coercedTypmod));
        return;
    }

    /*
     * Normal function: display as proname(args).
     */
    proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(node->funcid));
    if (!HeapTupleIsValid(proctup)) 
    {
        elog(ERROR, "cache lookup failed for function %u", node->funcid);
        return; /* Will never reach here */    
    }
    procform = (Form_pg_proc) GETSTRUCT(proctup);

    
    if (OidIsValid(procform->provariadic))
    {
        elog(ERROR, "Variadic functions are not supported");
    }

    /* Print schema name only if it's not pg_catalog */
    if (procform->pronamespace != PG_CATALOG_NAMESPACE)
    {
        const char *schemaname;
        schemaname = get_namespace_name(procform->pronamespace);
        schemaname = quote_identifier(schemaname);
        appendStringInfo(&ctx->query, "%s.", schemaname);
    }

    /* Deparse the function name ... */
    proname = NameStr(procform->proname);
    appendStringInfo(&ctx->query, "%s(", quote_identifier(proname));
    /* ... and all the arguments */
    first = true;
    foreach(arg, node->args)
    {
        if (!first)
            appendStringInfoString(&ctx->query, ", ");
        deparseExpr((Expr *) lfirst(arg), ctx);
        first = false;
    }
    appendStringInfoChar(&ctx->query, ')');

    ReleaseSysCache(proctup);
}

static void
deparseOperatorName(StringInfo buf, Form_pg_operator opform)
{
    /* opname is not a SQL identifier, so we should not quote it. */
    char *opname = NameStr(opform->oprname);

    /* Print schema name only if it's not pg_catalog */
    if (opform->oprnamespace != PG_CATALOG_NAMESPACE)
    {
        const char *opnspname = get_namespace_name(opform->oprnamespace);
        /* Print fully qualified operator name. */
        appendStringInfo(buf, "OPERATOR(%s.%s)",
                         quote_identifier(opnspname), opname);
    }
    else
    {
        /* Just print operator name. */
        appendStringInfo(buf, "%s", opname);
    }
}

static void
deparseOpExpr(OpExpr *node, HvaultDeparseContext *ctx)
{
    HeapTuple tuple;
    Form_pg_operator form;
    char oprkind;
    ListCell *arg;

    /* Retrieve information about the operator from system catalog. */
    tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
    if (!HeapTupleIsValid(tuple))
    {
        elog(ERROR, "cache lookup failed for operator %u", node->opno);
        return; /* Will never reach here */
    }
    form = (Form_pg_operator) GETSTRUCT(tuple);
    oprkind = form->oprkind;

    /* Sanity check. */
    Assert((oprkind == 'r' && list_length(node->args) == 1) ||
           (oprkind == 'l' && list_length(node->args) == 1) ||
           (oprkind == 'b' && list_length(node->args) == 2));

    /* Always parenthesize the expression. */
    appendStringInfoChar(&ctx->query, '(');

    /* Deparse left operand. */
    if (oprkind == 'r' || oprkind == 'b')
    {
        arg = list_head(node->args);
        deparseExpr(lfirst(arg), ctx);
        appendStringInfoChar(&ctx->query, ' ');
    }

    /* Deparse operator name. */
    deparseOperatorName(&ctx->query, form);

    /* Deparse right operand. */
    if (oprkind == 'l' || oprkind == 'b')
    {
        arg = list_tail(node->args);
        appendStringInfoChar(&ctx->query, ' ');
        deparseExpr(lfirst(arg), ctx);
    }

    appendStringInfoChar(&ctx->query, ')');

    ReleaseSysCache(tuple);
}

static void
deparseDistinctExpr(DistinctExpr *node, HvaultDeparseContext *ctx)
{
    Assert(list_length(node->args) == 2);

    appendStringInfoChar(&ctx->query, '(');
    deparseExpr(linitial(node->args), ctx);
    appendStringInfo(&ctx->query, " IS DISTINCT FROM ");
    deparseExpr(lsecond(node->args), ctx);
    appendStringInfoChar(&ctx->query, ')');
}

static void
deparseArrayRef(ArrayRef *node, HvaultDeparseContext *ctx)
{
    ListCell *lowlist_item;
    ListCell *uplist_item;

    /* Always parenthesize the expression. */
    appendStringInfoChar(&ctx->query, '(');

    /*
     * Deparse and parenthesize referenced array expression first. 
     */
    appendStringInfoChar(&ctx->query, '(');
    deparseExpr(node->refexpr, ctx);
    appendStringInfoChar(&ctx->query, ')');
     

    /* Deparse subscript expressions. */
    lowlist_item = list_head(node->reflowerindexpr);    /* could be NULL */
    foreach(uplist_item, node->refupperindexpr)
    {
        appendStringInfoChar(&ctx->query, '[');
        if (lowlist_item)
        {
            deparseExpr(lfirst(lowlist_item), ctx);
            appendStringInfoChar(&ctx->query, ':');
            lowlist_item = lnext(lowlist_item);
        }
        deparseExpr(lfirst(uplist_item), ctx);
        appendStringInfoChar(&ctx->query, ']');
    }

    appendStringInfoChar(&ctx->query, ')');
}

static void
deparseScalarArrayOpExpr(ScalarArrayOpExpr *node, HvaultDeparseContext *ctx)
{
    HeapTuple tuple;
    Form_pg_operator form;

    /* Retrieve information about the operator from system catalog. */
    tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
    if (!HeapTupleIsValid(tuple))
    {
        elog(ERROR, "cache lookup failed for operator %u", node->opno);
        return; /* Will never reach here */
    }
    form = (Form_pg_operator) GETSTRUCT(tuple);

    /* Sanity check. */
    Assert(list_length(node->args) == 2);

    /* Always parenthesize the expression. */
    appendStringInfoChar(&ctx->query, '(');

    /* Deparse left operand. */
    deparseExpr(linitial(node->args), ctx);
    appendStringInfoChar(&ctx->query, ' ');
    /* Deparse operator name plus decoration. */
    deparseOperatorName(&ctx->query, form);
    appendStringInfo(&ctx->query, " %s (", node->useOr ? "ANY" : "ALL");
    /* Deparse right operand. */
    deparseExpr(lsecond(node->args), ctx);
    appendStringInfoChar(&ctx->query, ')');

    /* Always parenthesize the expression. */
    appendStringInfoChar(&ctx->query, ')');

    ReleaseSysCache(tuple);
}

static void
deparseArrayExpr(ArrayExpr *node, HvaultDeparseContext *ctx)
{
    bool first = true;
    ListCell *l;

    appendStringInfo(&ctx->query, "ARRAY[");
    foreach(l, node->elements)
    {
        if (!first)
            appendStringInfo(&ctx->query, ", ");
        deparseExpr(lfirst(l), ctx);
        first = false;
    }
    appendStringInfoChar(&ctx->query, ']');

    /* If the array is empty, we need an explicit cast to the array type. */
    if (node->elements == NIL)
        appendStringInfo(&ctx->query, "::%s",
                         format_type_with_typemod(node->array_typeid, -1));
}

static void
deparseRelabelType(RelabelType *node, HvaultDeparseContext *ctx)
{
    deparseExpr(node->arg, ctx);
    if (node->relabelformat != COERCE_IMPLICIT_CAST)
        appendStringInfo(&ctx->query, "::%s",
                         format_type_with_typemod(node->resulttype,
                                                  node->resulttypmod));
}

static void
deparseBoolExpr(BoolExpr *node, HvaultDeparseContext *ctx)
{
    const char *op = NULL;
    bool first;
    ListCell *l;

    switch (node->boolop)
    {
        case AND_EXPR:
            op = "AND";
            break;
        case OR_EXPR:
            op = "OR";
            break;
        case NOT_EXPR:
            appendStringInfo(&ctx->query, "(NOT ");
            deparseExpr(linitial(node->args), ctx);
            appendStringInfoChar(&ctx->query, ')');
            return;
        default:
            elog(ERROR, "Unknown boolean expression type %d", node->boolop);
    }

    appendStringInfoChar(&ctx->query, '(');
    first = true;
    foreach(l, node->args)
    {
        if (!first)
            appendStringInfo(&ctx->query, " %s ", op);
        deparseExpr((Expr *) lfirst(l), ctx);
        first = false;
    }
    appendStringInfoChar(&ctx->query, ')');
}

static void
deparseNullTest(NullTest *node, HvaultDeparseContext *ctx)
{
    appendStringInfoChar(&ctx->query, '(');
    deparseExpr(node->arg, ctx);
    if (node->nulltesttype == IS_NULL)
        appendStringInfo(&ctx->query, " IS NULL)");
    else
        appendStringInfo(&ctx->query, " IS NOT NULL)");
}

static void 
deparseConstant(Const *node, HvaultDeparseContext *ctx)
{
    Oid typoutput;
    bool typIsVarlena;
    char *extval, *quoted = NULL;

    if (node->constisnull)
    {
        appendStringInfo(&ctx->query, "NULL");
        appendStringInfo(&ctx->query, "::%s",
                         format_type_with_typemod(node->consttype,
                                                  node->consttypmod));
        return;
    }

    getTypeOutputInfo(node->consttype,
                      &typoutput, &typIsVarlena);
    extval = OidOutputFunctionCall(typoutput, node->constvalue);

    switch (node->consttype)
    {
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case OIDOID:
        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
            {
                /*
                 * No need to quote unless it's a special value such as 'NaN'.
                 * See comments in get_const_expr().
                 */
                if (strspn(extval, "0123456789+-eE.") == strlen(extval))
                {
                    if (extval[0] == '+' || extval[0] == '-')
                        appendStringInfo(&ctx->query, "(%s)", extval);
                    else
                        appendStringInfoString(&ctx->query, extval);
                }
                else
                    appendStringInfo(&ctx->query, "'%s'", extval);
            }
            break;
        case BITOID:
        case VARBITOID:
            appendStringInfo(&ctx->query, "B'%s'", extval);
            break;
        case BOOLOID:
            if (strcmp(extval, "t") == 0)
                appendStringInfoString(&ctx->query, "true");
            else
                appendStringInfoString(&ctx->query, "false");
            break;
        default:
            quoted = quote_literal_cstr(extval);
            appendStringInfoString(&ctx->query, quoted);
            pfree(quoted);
            quoted = NULL;
            break;
    }

    appendStringInfo(&ctx->query, "::%s",
                     format_type_with_typemod(node->consttype,
                                              node->consttypmod));
}

static void
deparseExpr(Expr *node, HvaultDeparseContext *ctx)
{
    if (node == NULL)
        return;

    switch (nodeTag(node))
    {
        case T_Var:
            deparseVar((Var *) node, ctx);
            break;
        case T_Const:
            deparseConstant((Const *) node, ctx);
            break;
        case T_Param:
            deparseParameter(node, ctx);
            break;
        case T_FuncExpr:
            deparseFuncExpr((FuncExpr *) node, ctx);
            break;
        case T_OpExpr:
            deparseOpExpr((OpExpr *) node, ctx);
            break;
        case T_DistinctExpr:
            deparseDistinctExpr((DistinctExpr *) node, ctx);
            break;
        case T_ArrayRef:
            deparseArrayRef((ArrayRef *) node, ctx);
        case T_ScalarArrayOpExpr:
            deparseScalarArrayOpExpr((ScalarArrayOpExpr *) node, ctx);
            break;
        case T_ArrayExpr:
            deparseArrayExpr((ArrayExpr *) node, ctx);
            break;
        case T_RelabelType:
            deparseRelabelType((RelabelType *) node, ctx);
            break;
        case T_BoolExpr:
            deparseBoolExpr((BoolExpr *) node, ctx);
            break;
        case T_NullTest:
            deparseNullTest((NullTest *) node, ctx);
            break;
        default:
            elog(ERROR, "unsupported expression type for deparse: %d",
                 (int) nodeTag(node));
            break;
    }
}

static void
addDeparseItem(HvaultDeparseContext *ctx)
{
    if (ctx->first_qual)
    {
        appendStringInfoString(&ctx->query, "WHERE ");
        ctx->first_qual = false;
    }
    else 
    {
        appendStringInfoString(&ctx->query, " AND ");
    }
}

/* -----------------------------------
 *
 * Footprint expression functions
 *
 * -----------------------------------
 */

char * geomopstr[HvaultGeomNumAllOpers] = {

    /* HvaultGeomOverlaps  -> */ "&&",
    /* HvaultGeomContains  -> */ "~",
    /* HvaultGeomWithin    -> */ "@",
    /* HvaultGeomSame      -> */ "~=",
    /* HvaultGeomOverleft  -> */ "&<",
    /* HvaultGeomOverright -> */ "&>",
    /* HvaultGeomOverabove -> */ "|&>",
    /* HvaultGeomOverbelow -> */ "&<|",
    /* HvaultGeomLeft      -> */ "<<",
    /* HvaultGeomRight     -> */ ">>",
    /* HvaultGeomAbove     -> */ "|>>",
    /* HvaultGeomBelow     -> */ "<<|",
    /* HvaultGeomCommLeft  -> */ "&<",
    /* HvaultGeomCommRight -> */ "&>",
    /* HvaultGeomCommAbove -> */ "|&>",
    /* HvaultGeomCommBelow -> */ "&<|",
};

static HvaultGeomOperator geomopcomm[HvaultGeomNumAllOpers] = 
{
    /* HvaultGeomOverlaps  -> */ HvaultGeomOverlaps,
    /* HvaultGeomContains  -> */ HvaultGeomWithin,
    /* HvaultGeomWithin    -> */ HvaultGeomContains,
    /* HvaultGeomSame      -> */ HvaultGeomSame,
    /* HvaultGeomOverleft  -> */ HvaultGeomCommLeft,
    /* HvaultGeomOverright -> */ HvaultGeomCommRight,
    /* HvaultGeomOverabove -> */ HvaultGeomCommAbove,
    /* HvaultGeomOverbelow -> */ HvaultGeomCommBelow,
    /* HvaultGeomLeft      -> */ HvaultGeomRight,
    /* HvaultGeomRight     -> */ HvaultGeomLeft,
    /* HvaultGeomAbove     -> */ HvaultGeomBelow,
    /* HvaultGeomBelow     -> */ HvaultGeomAbove,
    /* HvaultGeomCommLeft  -> */ HvaultGeomOverleft,
    /* HvaultGeomCommRight -> */ HvaultGeomOverright,
    /* HvaultGeomCommAbove -> */ HvaultGeomOverabove,
    /* HvaultGeomCommBelow -> */ HvaultGeomOverbelow,
};

static GeomPredicateDesc geomopmap[2*HvaultGeomNumAllOpers] = 
{
    /* HvaultGeomOverlaps  -> */ { HvaultGeomOverlaps,  false },
    /* HvaultGeomContains  -> */ { HvaultGeomContains,  false },
    /* HvaultGeomWithin    -> */ { HvaultGeomOverlaps,  false },
    /* HvaultGeomSame      -> */ { HvaultGeomContains,  false },
    /* HvaultGeomOverleft  -> */ { HvaultGeomRight,     true  },
    /* HvaultGeomOverright -> */ { HvaultGeomLeft,      true  },
    /* HvaultGeomOverabove -> */ { HvaultGeomBelow,     true  },
    /* HvaultGeomOverbelow -> */ { HvaultGeomAbove,     true  },
    /* HvaultGeomLeft      -> */ { HvaultGeomOverright, true  },
    /* HvaultGeomRight     -> */ { HvaultGeomOverleft,  true  },
    /* HvaultGeomAbove     -> */ { HvaultGeomOverbelow, true  },
    /* HvaultGeomBelow     -> */ { HvaultGeomOverabove, true  },
    /* HvaultGeomCommLeft  -> */ { HvaultGeomCommLeft,  false },
    /* HvaultGeomCommRight -> */ { HvaultGeomCommRight, false },
    /* HvaultGeomCommAbove -> */ { HvaultGeomCommAbove, false },
    /* HvaultGeomCommBelow -> */ { HvaultGeomCommBelow, false },

    /* Negative predicate map */

    /* HvaultGeomOverlaps  -> */ { HvaultGeomWithin,    true  },
    /* HvaultGeomContains  -> */ { HvaultGeomInvalidOp, false },
    /* HvaultGeomWithin    -> */ { HvaultGeomWithin,    true  },
    /* HvaultGeomSame      -> */ { HvaultGeomInvalidOp, false },
    /* HvaultGeomOverleft  -> */ { HvaultGeomOverleft,  true  },
    /* HvaultGeomOverright -> */ { HvaultGeomOverright, true  },
    /* HvaultGeomOverabove -> */ { HvaultGeomOverabove, true  },
    /* HvaultGeomOverbelow -> */ { HvaultGeomOverbelow, true  },
    /* HvaultGeomLeft      -> */ { HvaultGeomLeft,      true  },
    /* HvaultGeomRight     -> */ { HvaultGeomRight,     true  },
    /* HvaultGeomAbove     -> */ { HvaultGeomAbove,     true  },
    /* HvaultGeomBelow     -> */ { HvaultGeomBelow,     true  },
    /* HvaultGeomCommLeft  -> */ { HvaultGeomInvalidOp, false },
    /* HvaultGeomCommRight -> */ { HvaultGeomInvalidOp, false },
    /* HvaultGeomCommAbove -> */ { HvaultGeomInvalidOp, false },
    /* HvaultGeomCommBelow -> */ { HvaultGeomInvalidOp, false },
};

static inline GeomPredicateDesc
mapGeomPredicate(GeomPredicateDesc const p)
{
    return geomopmap[HvaultGeomNumAllOpers * p.isneg + p.op];
}

static Oid
getGeometryOpOid(char const *opname, SPIPlanPtr prep_stmt)
{
    Datum param[1];
    Datum val;
    bool isnull;

    param[0] = CStringGetTextDatum(opname);
    if (SPI_execute_plan(prep_stmt, param, " ", true, 1) != SPI_OK_SELECT ||
        SPI_processed != 1 || 
        SPI_tuptable->tupdesc->natts != 1 ||
        SPI_tuptable->tupdesc->attrs[0]->atttypid != OIDOID)
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Can't find geometry operator %s", opname)));
        return InvalidOid; /* Will never reach this */
    }

    val = heap_getattr(SPI_tuptable->vals[0], 1, 
                       SPI_tuptable->tupdesc, &isnull);
    if (isnull)
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Can't find geometry operator %s", opname)));
        return InvalidOid; /* Will never reach this */            
    } 
    return DatumGetObjectId(val);
}

static void
getGeometryOpers(HvaultTableInfo *table)
{
    SPIPlanPtr prep_stmt;
    Oid argtypes[1];
    int i;

    if (SPI_connect() != SPI_OK_CONNECT)
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                     errmsg("Can't connect to SPI")));
        return; /* Will never reach this */
    }
    
    argtypes[0] = TEXTOID;
    prep_stmt = SPI_prepare(GEOMETRY_OP_QUERY, 1, argtypes);
    if (!prep_stmt)
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Can't prepare geometry operator query")));
        return; /* Will never reach this */
    }

    for (i = 0; i < HvaultGeomNumRealOpers; i++)
    {
        table->geomopers[i] = getGeometryOpOid(geomopstr[i], prep_stmt);
    }
    
    if (SPI_finish() != SPI_OK_FINISH)    
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Can't finish access to SPI")));
        return; /* Will never reach this */   
    }
}

static inline HvaultGeomOperator
getGeometryOper(Oid opno, const HvaultTableInfo *table)
{
    int i;
    for(i = 0; i < HvaultGeomNumRealOpers; ++i)
        if (table->geomopers[i] == opno) 
            return i;
    return HvaultGeomInvalidOp;
}

static bool
isFootprintOpArgs(Expr *varnode, 
                  Expr *arg, 
                  const HvaultTableInfo *table, 
                  HvaultColumnType *coltype)
{
    Var *var;

    if (!IsA(varnode, Var))
        return false;

    var = (Var *) varnode;
    if (var->varno != table->relid)
        return false;

    *coltype = table->coltypes[var->varattno - 1];
    if (*coltype != HvaultColumnPoint && *coltype != HvaultColumnFootprint)
        return false;

    return true;
}

static bool
isFootprintOp(Expr *expr, 
              const HvaultTableInfo *table, 
              GeomOpQual *qual)
{
    OpExpr *opexpr;
    Expr *first, *second;

    if (!IsA(expr, OpExpr))
        return false;

    opexpr = (OpExpr *) expr;

    if (list_length(opexpr->args) != 2)
        return false;

    qual->pred.op = getGeometryOper(opexpr->opno, table);
    if (qual->pred.op == HvaultGeomInvalidOp)
        return false;

    first = linitial(opexpr->args);
    second = lsecond(opexpr->args);
    if (isFootprintOpArgs(first, second, table, &qual->coltype))
    {
        qual->var = (Var *) first;
        qual->arg = second;
    }
    else if (isFootprintOpArgs(second, first, table, &qual->coltype))
    {
        qual->var = (Var *) second;
        qual->arg = first;
        qual->pred.op = geomopcomm[qual->pred.op];
    }
    else 
    {
        /* We support only simple var = const expressions */
        return false;
    }

    if (!isCatalogQual(qual->arg, table))
        return false;

    
    return true;
}

static bool 
isFootprintNegOp(Expr *expr, 
                 const HvaultTableInfo *table,
                 GeomOpQual *qual)
{
    BoolExpr *boolexpr;

    if (!IsA(expr, BoolExpr))
        return false;

    boolexpr = (BoolExpr *) expr;

    if (boolexpr->boolop != NOT_EXPR)
        return false;

    if (list_length(boolexpr->args) != 1)
        return false;

    if (!isFootprintOp(linitial(boolexpr->args), table, qual))
        return false;

    return true;
}

static bool
isFootprintQual(Expr *expr, const HvaultTableInfo *table, GeomOpQual *qual)
{
    bool res = false;
    if (isFootprintOp(expr, table, qual)) 
    {
        qual->pred.isneg = false;
        res = true;
    }

    if (isFootprintNegOp(expr, table, qual)) 
    {
        qual->pred.isneg = true;
        res = true;
    }

    if (res)
    {
        qual->catalog_pred = mapGeomPredicate(qual->pred);
        return true;
    }
    else 
    {
        return false;
    }
}

static void
deparseFootprintExpr(GeomOpQual *qual, HvaultDeparseContext *ctx)
{
    Assert(qual->catalog_pred.op != HvaultGeomInvalidOp);

    if (qual->catalog_pred.isneg)
        appendStringInfoString(&ctx->query, "NOT ");
    
    appendStringInfoChar(&ctx->query, '(');
    if (qual->catalog_pred.op < HvaultGeomNumRealOpers) 
    {
        appendStringInfoString(&ctx->query, "footprint ");
        appendStringInfoString(&ctx->query, geomopstr[qual->catalog_pred.op]);
        appendStringInfoChar(&ctx->query, ' ');
        deparseExpr(qual->arg, ctx);
    }
    else
    {
        deparseExpr(qual->arg, ctx);  
        appendStringInfoChar(&ctx->query, ' ');
        appendStringInfoString(&ctx->query, geomopstr[qual->catalog_pred.op]);
        appendStringInfoString(&ctx->query, " footprint");
    }
    appendStringInfoChar(&ctx->query, ')');
}



#include "hvault.h"

/* PostgreSQL */
#include <access/skey.h>
#include <catalog/namespace.h>
#include <catalog/pg_type.h>
#include <commands/defrem.h>
#include <commands/explain.h>
#include <executor/spi.h>
#include <foreign/fdwapi.h>
#include <foreign/foreign.h>
#include <nodes/bitmapset.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/planmain.h>
#include <optimizer/restrictinfo.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/timestamp.h>

/* HDF */
#include "hdf.h"


#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* 
 * FDW callback routines 
 */
static void hvaultExplain(ForeignScanState *node, ExplainState *es);
static void hvaultBegin(ForeignScanState *node, int eflags);
static TupleTableSlot *hvaultIterate(ForeignScanState *node);
static void hvaultReScan(ForeignScanState *node);
static void hvaultEnd(ForeignScanState *node);
/*
static bool 
hvaultAnalyze(Relation relation, 
              AcquireSampleRowsFunc *func,
              BlockNumber *totalpages );
*/

/*
 * Footprint interpolation routines
 */
static void calc_next_footprint(HvaultExecState const *scan, 
                                HvaultHDFFile *file);

/*
 * Options routines 
 */
static void  check_column_types(List *coltypes, TupleDesc tupdesc);
static char *get_column_sds(Oid relid, AttrNumber attnum, TupleDesc tupdesc);

/* 
 * Tuple fill utilities
 */
static void fill_float_val(HvaultExecState const *scan, AttrNumber attnum);
static void fill_point_val(HvaultExecState const *scan, AttrNumber attnum);
static void fill_footprint_val(HvaultExecState const *scan, AttrNumber attnum);

static HvaultSDSBuffer *get_sds_buffer(List **buffers, char *name);
static void fetch_next_line(HvaultExecState *scan);
static bool fetch_next_file(HvaultExecState *scan);
static void init_catalog_cursor(HvaultCatalogCursor *cursor);



/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
PG_FUNCTION_INFO_V1(hvault_fdw_handler);
Datum 
hvault_fdw_handler(PG_FUNCTION_ARGS)
{
    FdwRoutine *fdwroutine = makeNode(FdwRoutine);

    fdwroutine->GetForeignRelSize   = hvaultGetRelSize;
    fdwroutine->GetForeignPaths     = hvaultGetPaths;
    fdwroutine->GetForeignPlan      = hvaultGetPlan;
    fdwroutine->ExplainForeignScan  = hvaultExplain;
    fdwroutine->BeginForeignScan    = hvaultBegin;
    fdwroutine->IterateForeignScan  = hvaultIterate;
    fdwroutine->ReScanForeignScan   = hvaultReScan;
    fdwroutine->EndForeignScan      = hvaultEnd;
    /*fdwroutine->AnalyzeForeignTable = hvaultAnalyze;*/

    PG_RETURN_POINTER(fdwroutine);
}

PG_FUNCTION_INFO_V1(hvault_fdw_validator);
Datum 
hvault_fdw_validator(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(true);
}

static void 
hvaultExplain(ForeignScanState *node, ExplainState *es)
{
    /* Print additional EXPLAIN output for a foreign table scan. This can just
       return if there is no need to print anything. Otherwise, it should call
       ExplainPropertyText and related functions to add fields to the EXPLAIN
       output. The flag fields in es can be used to determine what to print, and
       the state of the ForeignScanState node can be inspected to provide run-
       time statistics in the EXPLAIN ANALYZE case. */    
}

static void 
hvaultBegin(ForeignScanState *node, int eflags)
{
    HvaultExecState *state;
    Oid foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);
    ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
    List *fdw_plan_private = plan->fdw_private;
    TupleDesc tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
    ListCell *l;
    AttrNumber attnum;
    MemoryContext file_cxt;


    elog(DEBUG1, "in hvaultBegin");
    /*
     * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return;

    Assert(list_length(fdw_plan_private) == HvaultPlanNumParams);
    state = (HvaultExecState *) palloc(sizeof(HvaultExecState));

    state->natts = tupdesc->natts;
    state->coltypes = list_nth(fdw_plan_private, HvaultPlanColtypes);
    check_column_types(state->coltypes, tupdesc);
    state->has_footprint = false; /* Initial value, will be detected later */
    /* TODO: variable scan size, depending on image scale & user params */
    state->scan_size = 10;

    state->cursor.catalog = get_table_option(foreigntableid, "catalog");
    if (state->cursor.catalog)
    {
        Value *query;

        state->cursor.cursormemctx = AllocSetContextCreate(
            node->ss.ps.state->es_query_cxt,
            "hvault_fdw file cursor data",
            ALLOCSET_SMALL_INITSIZE,
            ALLOCSET_SMALL_INITSIZE,
            ALLOCSET_SMALL_MAXSIZE);
        query = list_nth(fdw_plan_private, HvaultPlanCatalogQuery);
        state->cursor.catalog_query = strVal(query);
        state->cursor.fdw_expr = NULL;
        foreach(l, plan->fdw_exprs)
        {
            Expr *expr = (Expr *) lfirst(l);
            state->cursor.fdw_expr = lappend(state->cursor.fdw_expr,
                                             ExecInitExpr(expr, &node->ss.ps));
        }
        state->cursor.file_cursor_name = NULL;
        state->cursor.prep_stmt = NULL;
        state->cursor.expr_ctx = node->ss.ps.ps_ExprContext;
    }
    else
    {
        state->file.filename = get_table_option(foreigntableid, "filename");
        if (!state->file.filename)
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Can't find catalog or filename option")));
        }
    }

    file_cxt = AllocSetContextCreate(node->ss.ps.state->es_query_cxt,
                                     "hvault_fdw per-file data",
                                     ALLOCSET_DEFAULT_MINSIZE,
                                     ALLOCSET_DEFAULT_INITSIZE,
                                     ALLOCSET_DEFAULT_MAXSIZE);
    state->file.filememcxt = file_cxt;
    state->file.filename = NULL;
    state->file.sds = NIL;
    state->file.prevbrdlat = NULL;
    state->file.prevbrdlon = NULL;
    state->file.nextbrdlat = NULL;
    state->file.nextbrdlon = NULL;
    state->file.sd_id = FAIL;
    state->file.num_lines = -1;
    state->file.num_samples = -1;
    state->file.open_time = 0;
    
    state->colbuffer = palloc0(sizeof(HvaultSDSBuffer *) * state->natts);
    state->lat = NULL;
    state->lon = NULL;
    /* fill required sds list & helper pointers */
    attnum = 0;
    foreach(l, state->coltypes)
    {
        switch(lfirst_int(l)) {
            case HvaultColumnFloatVal:
                state->colbuffer[attnum] = get_sds_buffer(
                    &(state->file.sds), 
                    get_column_sds(foreigntableid, attnum, tupdesc));
                break;
            case HvaultColumnPoint:
                state->lat = get_sds_buffer(&(state->file.sds), "Latitude");
                state->lon = get_sds_buffer(&(state->file.sds), "Longitude");
                break;
            case HvaultColumnFootprint:
                state->lat = get_sds_buffer(&(state->file.sds), "Latitude");
                state->lat->haswindow = true;
                state->lon = get_sds_buffer(&(state->file.sds), "Longitude");
                state->lon->haswindow = true;
                state->has_footprint = true;
                break;
        }
        attnum++;
    }
    if (list_length(state->file.sds) == 0)
    {
        /* Adding latitude column to get size of files */
        get_sds_buffer(&(state->file.sds), "Latitude");
    }
    elog(DEBUG1, "SDS buffers: %d", list_length(state->file.sds));

    state->file_time = 0;
    state->cur_file = -1;
    state->cur_line = -1;
    state->cur_sample = -1;

    state->values = palloc(sizeof(Datum) * state->natts);
    state->nulls = palloc(sizeof(bool) * state->natts);
    state->sds_vals = palloc(sizeof(double) * state->natts);

    state->point = lwpoint_make2d(SRID_UNKNOWN, 0, 0);
    state->ptarray = ptarray_construct(false, false, 5);
    state->poly = lwpoly_construct(SRID_UNKNOWN, NULL, 1, &state->ptarray);
    lwgeom_add_bbox(lwpoly_as_lwgeom(state->poly));

    node->fdw_state = state;
}

static TupleTableSlot *
hvaultIterate(ForeignScanState *node) 
{
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
    /*TupleDesc tupdesc = slot->tts_tupleDescriptor;*/
    HvaultExecState *fdw_exec_state = (HvaultExecState *) node->fdw_state;
    ListCell *l;
    AttrNumber attnum;

    if (fdw_exec_state->cur_line == 0 && fdw_exec_state->cur_sample == 0)
    {
        elog(DEBUG1, "first hvaultIterate");
    }

    ExecClearTuple(slot);

    /* Next line needed? (initial state -1 == -1) */
    if (fdw_exec_state->cur_sample == fdw_exec_state->file.num_samples)
    {
        fdw_exec_state->cur_sample = 0;
        fdw_exec_state->cur_line++;
        /* Next file needed? */
        if (fdw_exec_state->cur_line >= fdw_exec_state->file.num_lines) {
            if (fdw_exec_state->cursor.catalog)
            {
                if (!fetch_next_file(fdw_exec_state))
                {
                    /* End of scan, return empty tuple*/
                    elog(DEBUG1, "End of scan: no more files");
                    return slot;
                }
                fdw_exec_state->cur_line = 0;
            } 
            else 
            {
                /* Single file case */
                /* Initialization or end of file? */
                if (fdw_exec_state->file.sd_id == FAIL)
                {
                    char const *filename = fdw_exec_state->file.filename;
                    if (!hdf_file_open(&fdw_exec_state->file, filename, 
                                       fdw_exec_state->has_footprint))
                    {
                        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                                        errmsg("Can't open file %s", 
                                               filename)));
                        return NULL; /* Will never reach here */
                    }
                    fdw_exec_state->cur_file = 0;
                }
                else
                {
                    /* End of scan, returning empty tuple */
                    return slot;
                }
            }
        }
        fetch_next_line(fdw_exec_state);
    }

    /* Fill tuple */
    attnum = 0;
    foreach(l, fdw_exec_state->coltypes)
    {
        HvaultColumnType type = lfirst_int(l);
        switch(type)
        {
            case HvaultColumnNull:
                fdw_exec_state->nulls[attnum] = true;
                break;
            case HvaultColumnFloatVal:
                fill_float_val(fdw_exec_state, attnum);
                break;
            case HvaultColumnPoint:
                fill_point_val(fdw_exec_state, attnum);
                break;
            case HvaultColumnFootprint:
                fill_footprint_val(fdw_exec_state, attnum);
                break;
            case HvaultColumnFileIdx:
                fdw_exec_state->values[attnum] = 
                    Int32GetDatum(fdw_exec_state->cur_file);
                fdw_exec_state->nulls[attnum] = false;
                break;
            case HvaultColumnLineIdx:
                fdw_exec_state->values[attnum] = 
                    Int32GetDatum(fdw_exec_state->cur_line);
                fdw_exec_state->nulls[attnum] = false;
                break;
            case HvaultColumnSampleIdx:
                fdw_exec_state->values[attnum] = 
                    Int32GetDatum(fdw_exec_state->cur_sample);
                fdw_exec_state->nulls[attnum] = false;
                break;
            case HvaultColumnTime:
                fdw_exec_state->nulls[attnum] = fdw_exec_state->file_time == 0;
                if (!fdw_exec_state->nulls[attnum])
                {
                    fdw_exec_state->values[attnum] = 
                        TimestampGetDatum(fdw_exec_state->file_time);
                }
                break;
            default:
                ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                                errmsg("Undefined column type %d", type)));
                return NULL; /* Will never reach here */
        }
        attnum++;
    }
    slot->tts_isnull = fdw_exec_state->nulls;
    slot->tts_values = fdw_exec_state->values;
    ExecStoreVirtualTuple(slot);
    fdw_exec_state->cur_sample++;
    return slot;
}

static void 
hvaultReScan(ForeignScanState *node)
{
    HvaultExecState *state = (HvaultExecState *) node->fdw_state;
    elog(DEBUG1, "in hvaultReScan");
    if (state == NULL)
        return;

    if (state->file.sd_id != FAIL)
    {
        hdf_file_close(&state->file);
    }

    if (state->cursor.catalog)
    {
        if (SPI_connect() != SPI_OK_CONNECT)
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Can't connect to SPI")));
            return; /* Will never reach this */
        }
    
        if (state->cursor.file_cursor_name != NULL)
        {
            Portal file_cursor = SPI_cursor_find(state->cursor.file_cursor_name);
            SPI_cursor_close(file_cursor);

        }

        if (SPI_finish() != SPI_OK_FINISH)    
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Can't finish access to SPI")));
            return; /* Will never reach this */   
        }
    }

    MemoryContextReset(state->file.filememcxt);

    state->cursor.file_cursor_name = NULL;
    state->cur_file = -1;
    state->cur_line = -1;
    state->cur_sample = -1;
}


static void 
hvaultEnd(ForeignScanState *node)
{
    HvaultExecState *state = (HvaultExecState *) node->fdw_state;
    elog(DEBUG1, "in hvaultEnd");
    if (state == NULL)
        return;
    
    if (state->file.sd_id != FAIL)
    {
        hdf_file_close(&state->file);
    }

    if (state->cursor.catalog)
    {
        if (SPI_connect() != SPI_OK_CONNECT)
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Can't connect to SPI")));
            return; /* Will never reach this */
        }
    
        if (state->cursor.file_cursor_name != NULL)
        {
            Portal file_cursor = SPI_cursor_find(state->cursor.file_cursor_name);
            SPI_cursor_close(file_cursor);

        }
        if (state->cursor.prep_stmt != NULL)
        {
            SPI_freeplan(state->cursor.prep_stmt);
        }
        
        MemoryContextDelete(state->cursor.cursormemctx);

        if (SPI_finish() != SPI_OK_FINISH)    
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Can't finish access to SPI")));
            return; /* Will never reach this */   
        }
    }
    
    MemoryContextDelete(state->file.filememcxt);
}





char * 
get_table_option(Oid foreigntableid, char *option)
{
    ListCell *l;
    ForeignTable *foreigntable = GetForeignTable(foreigntableid);
    foreach(l, foreigntable->options)
    {
        DefElem *def = (DefElem *) lfirst(l);
        if (strcmp(def->defname, option) == 0)
        {
            return defGetString(def);
        }
    }
    elog(DEBUG1, "Can't find table option %s", option);
    return NULL;
}



static char *
get_column_sds(Oid relid, AttrNumber attnum, TupleDesc tupdesc)
{
    List *options;
    ListCell *o;

    Assert(attnum < tupdesc->natts);
    options = GetForeignColumnOptions(relid, attnum+1);
    elog(DEBUG1, "options for att %d: %s", attnum, nodeToString(options));
    foreach(o, options)
    {
        DefElem *def = (DefElem *) lfirst(o);
        if (strcmp(def->defname, "sds") == 0) {
            return defGetString(def);
        }
    }

    elog(DEBUG1, "Can't find sds option for att: %d relid: %d opts: %s", 
         attnum, relid, nodeToString(options));
    return NameStr(tupdesc->attrs[attnum]->attname);;
}

static void
check_column_types(List *coltypes, TupleDesc tupdesc)
{
    ListCell *l;
    AttrNumber attnum = 0;
    Oid geomtypeoid = TypenameGetTypid("geometry");
    if (geomtypeoid == InvalidOid) 
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Can't find geometry type"),
                        errhint("PostGIS must be installed to use Hvault")));
    }

    elog(DEBUG1, "check_column_types: coltypes: %s", nodeToString(coltypes));

    Assert(list_length(coltypes) == tupdesc->natts);
    foreach(l, coltypes)
    {
        switch(lfirst_int(l))
        {
            case HvaultColumnFloatVal:
                if (tupdesc->attrs[attnum]->atttypid != FLOAT8OID)
                    ereport(ERROR, 
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Invalid column type %d", attnum),
                             errhint("SDS column must have float8 type")));
                break;
            case HvaultColumnPoint:
            case HvaultColumnFootprint:
                if (tupdesc->attrs[attnum]->atttypid != geomtypeoid)
                    ereport(ERROR, 
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Invalid column type %d", attnum),
                             errhint(
                        "Point & footprint columns must have geometry type")));
                break;
            case HvaultColumnFileIdx:
            case HvaultColumnLineIdx:
            case HvaultColumnSampleIdx:
                if (tupdesc->attrs[attnum]->atttypid != INT4OID)
                    ereport(ERROR, 
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Invalid column type %d", attnum),
                             errhint("Index column must have int4 type")));
                break;
            case HvaultColumnTime:
                if (tupdesc->attrs[attnum]->atttypid != TIMESTAMPOID)
                    ereport(ERROR, 
                            (errcode(ERRCODE_FDW_ERROR),
                             errmsg("Invalid column type %d", attnum),
                             errhint("Time column must have timestamp type")));
                break;
            case HvaultColumnNull:
                /* nop */
                break;
            default:
                ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                                errmsg("Undefined column type")));
        }
        attnum++;
    }
}

static HvaultSDSBuffer *
get_sds_buffer(List **buffers, char *name)
{
    ListCell *l;
    HvaultSDSBuffer *sds = NULL;

    elog(DEBUG1, "creating buffer for sds %s", name);

    foreach(l, *buffers)
    {
        sds = (HvaultSDSBuffer *) lfirst(l);
        if (strcmp(name, sds->name) == 0) 
            return sds;
    }

    sds = palloc(sizeof(HvaultSDSBuffer));
    sds->prev = NULL;
    sds->cur = NULL;
    sds->next = NULL;
    sds->haswindow = false;
    sds->id = -1;
    sds->name = name;
    sds->type = -1;
    *buffers = lappend(*buffers, sds);
    return sds;
}

static void 
calc_next_footprint(HvaultExecState const *scan, HvaultHDFFile *file)
{
    int scan_line;
    Assert(scan->lat != NULL);
    Assert(scan->lon != NULL);
    Assert(scan->lat->type == DFNT_FLOAT32);
    Assert(scan->lon->type == DFNT_FLOAT32);
    /*
     * Three possibilities:
     * 1. first line in scan -> extrapolate prev, interpolate next
     * 2. last line in scan  -> reuse prev,       extrapolate next
     * 3. casual line        -> reuse prev,       interpolate next
     */
    scan_line = scan->cur_line % scan->scan_size;
    if (scan_line == 0)
    {
        /* extrapolate prev */
        extrapolate_line(file->num_samples, 
                         (float *) scan->lat->next, 
                         (float *) scan->lat->cur, 
                         file->prevbrdlat);
        extrapolate_line(file->num_samples, 
                         (float *) scan->lon->next, 
                         (float *) scan->lon->cur, 
                         file->prevbrdlon);
    }
    else 
    {
        /* reuse prev */
        float *buf;
        
        buf = file->prevbrdlat;
        file->prevbrdlat = file->nextbrdlat;
        file->nextbrdlat = buf;

        buf = file->prevbrdlon;
        file->prevbrdlon = file->nextbrdlon;
        file->nextbrdlon = buf;
    }

    if (scan_line == scan->scan_size - 1)
    {
        /* extrapolate next */
        extrapolate_line(file->num_samples, 
                         (float *) scan->lat->prev, 
                         (float *) scan->lat->cur, 
                         file->nextbrdlat);
        extrapolate_line(file->num_samples, 
                         (float *) scan->lon->prev, 
                         (float *) scan->lon->cur, 
                         file->nextbrdlon);
    }
    else 
    {
        /* interpolate next */
        interpolate_line(file->num_samples, 
                         (float *) scan->lat->cur,
                         (float *) scan->lat->next, 
                         file->nextbrdlat);
        interpolate_line(file->num_samples, 
                         (float *) scan->lon->cur,
                         (float *) scan->lon->next, 
                         file->nextbrdlon);
    }
}

static void 
fetch_next_line(HvaultExecState *scan)
{
    ListCell *l;

    foreach(l, scan->file.sds)
    {
        HvaultSDSBuffer *sds = (HvaultSDSBuffer *) lfirst(l);
        int32_t start[2], stride[2], end[2];
        void *buf;

        start[0] = scan->cur_line;
        start[1] = 0;
        stride[0] = 1;
        stride[1] = 1;
        end[0] = 1;
        end[1] = scan->file.num_samples;

        if (sds->haswindow)
        {
            if (scan->cur_line == 0) {
                if (SDreaddata(sds->id, start, stride, end, sds->cur) == FAIL)
                {
                    ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                                    errmsg("Can't read data from HDF file")));
                    return; /* will never reach here */
                }
            } 
            else
            {
                /* swap buffers */
                buf = sds->prev;
                sds->prev = sds->cur;
                sds->cur = sds->next;
                sds->next = buf;
            }

            start[0]++;
            buf = sds->next;
        }
        else
        {
            buf = sds->cur;
        }

        /* don't read next line for last line */
        if (start[0] < scan->file.num_lines)
        {
            if (SDreaddata(sds->id, start, stride, end, buf) == FAIL)
            {
                ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                                errmsg("Can't read data from HDF file")));
                return; /* will never reach here */       
            }    
        }
    }

    /* Calculate footprint data */
    if (scan->has_footprint)
    {
        calc_next_footprint(scan, &scan->file);
    }
}

static void
fill_float_val(HvaultExecState const *scan, AttrNumber attnum)
{
    HvaultSDSBuffer *sds = scan->colbuffer[attnum];
    if (sds->fill_val != NULL && 
        hdf_cmp(sds->type, sds->cur, scan->cur_sample, sds->fill_val))
    {
        scan->nulls[attnum] = true;
    } 
    else
    {
        scan->nulls[attnum] = false;
        scan->sds_vals[attnum] = hdf_value(
            sds->type, sds->cur, scan->cur_sample) / sds->scale - sds->offset;
        scan->values[attnum] = Float8GetDatumFast(scan->sds_vals[attnum]);
    }
}

static void
fill_point_val(HvaultExecState const *scan, AttrNumber attnum)
{
    /* This is quite dirty code that uses internal representation of LWPOINT.
       However it is a very hot place here */
    GSERIALIZED *ret;
    double *data = (double *) scan->point->point->serialized_pointlist;
    data[0] = hdf_value(scan->lat->type, scan->lat->cur, scan->cur_sample);
    data[1] = hdf_value(scan->lon->type, scan->lon->cur, scan->cur_sample);
    ret = gserialized_from_lwgeom((LWGEOM *) scan->point, true, NULL);
    scan->nulls[attnum] = false;
    scan->values[attnum] = PointerGetDatum(ret);
}

static void
fill_footprint_val(HvaultExecState const *scan, AttrNumber attnum)
{
    /* This is quite dirty code that uses internal representation of LWPOLY.
       However it is a very hot place here */
    GSERIALIZED *ret;
    double *data = (double *) scan->poly->rings[0]->serialized_pointlist;
    data[0] = scan->file.prevbrdlat[scan->cur_sample];
    data[1] = scan->file.prevbrdlon[scan->cur_sample];
    data[2] = scan->file.prevbrdlat[scan->cur_sample+1];
    data[3] = scan->file.prevbrdlon[scan->cur_sample+1];
    data[4] = scan->file.nextbrdlat[scan->cur_sample+1];
    data[5] = scan->file.nextbrdlon[scan->cur_sample+1];
    data[6] = scan->file.nextbrdlat[scan->cur_sample];
    data[7] = scan->file.nextbrdlon[scan->cur_sample];
    data[8] = scan->file.prevbrdlat[scan->cur_sample];
    data[9] = scan->file.prevbrdlon[scan->cur_sample];
    lwgeom_calculate_gbox((LWGEOM *) scan->poly, scan->poly->bbox);
    ret = gserialized_from_lwgeom((LWGEOM *) scan->poly, true, NULL);
    scan->nulls[attnum] = false;
    scan->values[attnum] = PointerGetDatum(ret);   
}

static bool
fetch_next_file(HvaultExecState *scan)
{
    Portal file_cursor;
    bool retval = false;

    elog(DEBUG1, "fetching next file");
    if (SPI_connect() != SPI_OK_CONNECT)
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Can't connect to SPI")));
        return false; /* Will never reach this */
    }

    if (scan->cursor.file_cursor_name == NULL)
    {
        init_catalog_cursor(&scan->cursor);
    } 
    else 
    {
        /* Close previous file */
        hdf_file_close(&scan->file);
    }
    
    file_cursor = SPI_cursor_find(scan->cursor.file_cursor_name);
    Assert(file_cursor);
    do 
    {
        Datum val;
        bool isnull;
        char *filename;

        SPI_cursor_fetch(file_cursor, true, 1);
        if (SPI_processed != 1 || SPI_tuptable == NULL)
        {
            /* Can't fetch more files */
            elog(DEBUG1, "No more files");
            break;
        }   

        if (SPI_tuptable->tupdesc->natts != 3 ||
            SPI_tuptable->tupdesc->attrs[0]->atttypid != INT4OID ||
            SPI_tuptable->tupdesc->attrs[1]->atttypid != TEXTOID ||
            SPI_tuptable->tupdesc->attrs[2]->atttypid != TIMESTAMPOID)
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Error in catalog query")));
            return false; /* Will never reach this */         
        }

        val = heap_getattr(SPI_tuptable->vals[0], 1, 
                           SPI_tuptable->tupdesc, &isnull);
        if (isnull)
        {
            elog(WARNING, "Wrong entry in catalog, skipping");
            continue;
        }
        scan->cur_file = DatumGetInt32(val);
        filename = SPI_getvalue(SPI_tuptable->vals[0], 
                                SPI_tuptable->tupdesc, 2);
        if (!filename)
        {
            elog(WARNING, "Wrong entry in catalog, skipping");
            continue;
        }
        val = heap_getattr(SPI_tuptable->vals[0], 3, 
                           SPI_tuptable->tupdesc, &isnull);
        scan->file_time = isnull ? 0 : DatumGetTimestamp(val);

        retval = hdf_file_open(&scan->file, filename, scan->has_footprint);
    }
    while(retval == false);

    if (SPI_finish() != SPI_OK_FINISH)    
    {
        ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                        errmsg("Can't finish access to SPI")));
        return false; /* Will never reach this */   
    }

    return retval;
}

static void 
init_catalog_cursor(HvaultCatalogCursor *cursor)
{
    ListCell *l;
    int nargs, pos;
    Portal file_cursor;
        
    Assert(cursor->catalog);
    nargs = list_length(cursor->fdw_expr);

    elog(DEBUG1, "file cursor initialization");
    if (cursor->prep_stmt == NULL)
    {
        MemoryContext oldmemcxt;
        StringInfo query_str;
        Oid *argtypes;

        Assert(cursor->cursormemctx);
        MemoryContextReset(cursor->cursormemctx);
        oldmemcxt = MemoryContextSwitchTo(cursor->cursormemctx);

        query_str = makeStringInfo();
        appendStringInfo(query_str, 
            "SELECT file_id, filename, starttime FROM %s %s ORDER BY file_id",
            cursor->catalog, cursor->catalog_query);
        argtypes = palloc(sizeof(Oid) * nargs);
        cursor->values = palloc(sizeof(Datum) * nargs);
        cursor->nulls = palloc(sizeof(char) * nargs);
        pos = 0;
        foreach(l, cursor->fdw_expr)
        {
            ExprState *expr = (ExprState *) lfirst(l);
            argtypes[pos] = exprType((Node *) expr->expr);
            pos++;
        }
        cursor->prep_stmt = SPI_prepare(query_str->data, nargs, argtypes);
        if (!cursor->prep_stmt)
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Can't prepare query for catalog %s", 
                                   cursor->catalog)));
            return; /* Will never reach this */
        }
        if (SPI_keepplan(cursor->prep_stmt) != 0)
        {
            ereport(ERROR, (errcode(ERRCODE_FDW_ERROR),
                            errmsg("Can't save prepared plan")));
            return; /* Will never reach this */
        }

        MemoryContextSwitchTo(oldmemcxt);
    }

    pos = 0;
    foreach(l, cursor->fdw_expr)
    {
        bool isnull;
        Assert(IsA(lfirst(l), ExprState));
        ExprState *expr = (ExprState *) lfirst(l);
        cursor->values[pos] = ExecEvalExpr(expr, cursor->expr_ctx, 
                                           &isnull, NULL);
        cursor->nulls[pos] = isnull ? 'n' : ' ';
        pos++;
    }

    file_cursor = SPI_cursor_open(NULL, cursor->prep_stmt, cursor->values, 
                                  cursor->nulls, true);
    cursor->file_cursor_name = file_cursor->name;
}



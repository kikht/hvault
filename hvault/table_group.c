#include "common.h"

static ArrayType *
intArrayInit(int ndims, int const * dims, int const * lbs)
{
    ArrayType * res;
    int size;
    int nbytes;

    if (ndims < 0)              /* we do allow zero-dimension arrays */
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid number of dimensions: %d", ndims)));
    if (ndims > MAXDIM)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
                        ndims, MAXDIM)));
    

    /* fast track for empty array */
    if (ndims == 0)
        return construct_empty_array(INT4OID);
    
    size = ArrayGetNItems(ndims, dims);
    nbytes = ARR_OVERHEAD_NONULLS(ndims);
    nbytes += size * 4;
    res = palloc0(nbytes);
    SET_VARSIZE(res, nbytes);
    res->ndim = ndims;
    res->elemtype = INT4OID;
    res->dataoffset = 0;
    memcpy(ARR_DIMS(res), dims, ndims * sizeof(int));
    memcpy(ARR_LBOUND(res), lbs, ndims * sizeof(int));

    return res;
}

static int 
intArrayIdx(ArrayType * array, int const * coord, bool noerror)
{
    int * lbs;
    int * dims;
    int i, res;

    lbs = ARR_LBOUND(array);
    dims = ARR_DIMS(array);

    res = 0;
    for (i = 0; i < array->ndim; i++)
    {
        int c = coord[i] - lbs[i];

        if (c < 0 || c >= dims[i])
        {
            if (noerror)
                return -1;
            else
                elog(ERROR, "array out of bounds");
        }
        res = res * dims[i] + c;
    }

    return res;
}

PG_FUNCTION_INFO_V1(hvault_table_count_step);
Datum
hvault_table_count_step(PG_FUNCTION_ARGS)
{
    MemoryContext aggmemctx, oldmemctx;
    ArrayType * ctx;
    ArrayType * idx_array;
    int i, pos_idx;
    int32_t * ctx_data;
    
    if (!AggCheckCallContext(fcinfo, &aggmemctx))
        elog(ERROR, "hvault_table_group_step called in non-aggregate context");

    
    ctx = PG_ARGISNULL(0) ? NULL : PG_GETARG_ARRAYTYPE_P(0);
    if (ctx == NULL)
    {
        int ndim;
        int32_t * bounds_data;
        int dims[MAXDIM];
        int lbs[MAXDIM]; 
        ArrayType * bounds_array;

        oldmemctx = MemoryContextSwitchTo(aggmemctx);

        if (PG_ARGISNULL(2))
            elog(ERROR, "bounds array must not be null");

//        if (!get_fn_expr_arg_stable(fcinfo->flinfo, 2))
//            elog(ERROR, "bounds array must be const");

        bounds_array = PG_GETARG_ARRAYTYPE_P(2);
        Assert(bounds_array != NULL);
        Assert(bounds_array->elemtype == INT4OID);

        if (bounds_array->ndim != 2 || ARR_DIMS(bounds_array)[1] != 2)
            elog(ERROR, "bounds array size is invalid");

        if (ARR_HASNULL(bounds_array))
        {
            int size = ARR_DIMS(bounds_array)[0] * ARR_DIMS(bounds_array)[1]; 
            for (i = 0; i < (size + 7) / 8; i++)
                if (ARR_NULLBITMAP(bounds_array)[i] != 0)
                    elog(ERROR, "bounds array must not contain NULLs");
        }

        ndim = ARR_DIMS(bounds_array)[0]; 
        if (ndim > MAXDIM)
            elog(ERROR, "too many dimensions, max supported is %d", MAXDIM);

        bounds_data = (int32_t *) ARR_DATA_PTR(bounds_array);
        for (i = 0; i < ndim; i++) 
        {
            int ubs;

            lbs[i] = bounds_data[2*i];
            ubs = bounds_data[2*i+1];
            dims[i] = ubs - lbs[i];
        }

        ctx = intArrayInit(ndim, dims, lbs);
        MemoryContextSwitchTo(oldmemctx);
    }
    
    Assert(!ARR_HASNULL(ctx));
    Assert(ctx->elemtype == INT4OID);
    
    if (PG_ARGISNULL(1))
        elog(ERROR, "group index array must not be null");

    idx_array = PG_GETARG_ARRAYTYPE_P(1);
    Assert(idx_array != NULL);
    Assert(idx_array->elemtype == INT4OID);

    if (idx_array->ndim != 1)
        elog(ERROR, "group index array must have single dimension");

    if (ARR_DIMS(idx_array)[0] != ctx->ndim)
        elog(ERROR, "group index array length is inconsistent");

    if (ARR_HASNULL(idx_array))
    {
        int size = ARR_DIMS(idx_array)[0];
        for (i = 0; i < (size + 7) / 8; i++)
            /* Skip elements with nulls */
            if (ARR_NULLBITMAP(idx_array)[i] != 0)
            {
                elog(WARNING, "index array contains NULL, skipping");
                PG_RETURN_ARRAYTYPE_P(ctx); 
            }
    }
    

    pos_idx = intArrayIdx(ctx, (int *) ARR_DATA_PTR(idx_array), true);
    if (pos_idx != -1)
    {
        Assert(pos_idx >= 0);
        if (ARR_SIZE(ctx) - ARR_DATA_OFFSET(ctx) <= pos_idx * 4)
        {
            elog(ERROR, "Array out of bounds access: %ld %d", 
                 ARR_SIZE(ctx) - ARR_DATA_OFFSET(ctx), pos_idx * 4);
        }
        ctx_data = (int32_t *) ARR_DATA_PTR(ctx);
        ctx_data[pos_idx]++;
    }
    
    PG_RETURN_ARRAYTYPE_P(ctx);
}


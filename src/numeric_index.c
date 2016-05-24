#include "numeric_index.h"
/*
A numeric index allows indexing of documents by numeric ranges, and intersection of them with
fulltext indexes.
*/

int numericFilter_Match(NumericFilter *f, double score) {
    
    // match min - -inf or x >/>= score 
    int matchMin = f->minNegInf || (f->inclusiveMin ? score >= f->min : score > f->min);
    
    if (matchMin) {
        // match max - +inf or x </<= score
        return f->maxInf || (f->inclusiveMax ? score <= f->max : score < f->max);
    }
    
    return 0;
}

#define NUMERIC_INDEX_KEY_FMT "num:%s/%s"

RedisModuleString *fmtNumericIndexKey(RedisSearchCtx *ctx, const char *field) {
    return RMUtil_CreateFormattedString(ctx->redisCtx, NUMERIC_INDEX_KEY_FMT, ctx->spec->name, field);   
}

NumericIndex *NewNumericIndex(RedisSearchCtx *ctx, FieldSpec *sp) {
    
    RedisModuleString *s = fmtNumericIndexKey(ctx, sp->name);
    RedisModuleKey *k = RedisModule_OpenKey(ctx->redisCtx, s, REDISMODULE_READ | REDISMODULE_WRITE);
    if (k == NULL || 
        (RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_EMPTY &&
        RedisModule_KeyType(k) != REDISMODULE_KEYTYPE_ZSET)) {
            k == NULL;
    }
        
    NumericIndex *ret = malloc(sizeof(NumericIndex));
    ret->ctx = ctx;
    ret->key = k;
    return ret;
    
}

void NumerIndex_Free(NumericIndex *idx) {
    
    if (idx->key) RedisModule_CloseKey(idx->key);
    free(idx);
}

int NumerIndex_Add(NumericIndex *idx, t_docId docId, double score) {
    
    if (idx->key == NULL) return REDISMODULE_ERR;
    
    return RedisModule_ZsetAdd(idx->key, score, 
                                RMUtil_CreateFormattedString(idx->ctx->redisCtx, "%u", docId), NULL);
    
}

 int NumericFilter_Read(void *ctx, IndexHit *e) {
     if (NumericFilter_HasNext(ctx)){ 
        return INDEXREAD_OK;    
     }
     
     return INDEXREAD_EOF;
}
// Skip to a docid, potentially reading the entry into hit, if the docId matches
//
// In this case we don't actually skip to a docId, just check whether it is within our range
int NumericFilter_SkipTo(void *ctx, u_int32_t docId, IndexHit *hit) {
    
    NumericFilter *f = ctx;
    if (f->idx->key == NULL) {
        return INDEXREAD_EOF;
    }
    
    RedisModuleString *s = RMUtil_CreateFormattedString(f->idx->ctx->redisCtx, "%u", docId);
    
    double score = 0;
    f->lastDocid = docId;
    // See if the filter 
    if (RedisModule_ZsetScore(f->idx->key, s, &score) == REDISMODULE_OK) {
        
        if (numericFilter_Match(f, score)) {
            
            hit->docId = docId;
            hit->flags = 0xFF;
            hit->numOffsetVecs = 0;
            hit->totalFreq = 0;
            hit->type = H_RAW;
            return INDEXREAD_OK;
        }
    } 
    
    return INDEXREAD_NOTFOUND;;
}

// the last docId read
t_docId NumericFilter_LastDocId(void *ctx) {
    NumericFilter *f = ctx;
    return f->lastDocid;
}
// can we continue iteration?
int NumericFilter_HasNext(void *ctx) {
    return 1;
    
}
// release the iterator's context and free everything needed
void NumericFilter_Free(struct indexIterator *self) {
    NumericFilter *f = self->ctx;
    free(f->idx);
    free(f);
    free(self);
}


NumericFilter *NewNumericFilter(RedisSearchCtx *ctx, FieldSpec *fs, double min, double max, 
                                int inclusiveMin, int inclusiveMax) {
    
    NumericFilter *f = malloc(sizeof(NumericFilter));
    f->idx = NewNumericIndex(ctx, fs);
    f->min = min;
    f->max = max;
    f->inclusiveMax = inclusiveMax;
    f->inclusiveMin = inclusiveMin;
    f->lastDocid = 0;
    return f;
}

IndexIterator *NewNumericFilterIterator(NumericFilter *f) {
    IndexIterator *ret = malloc(sizeof(IndexIterator));
    ret->ctx = f;
    ret->Free = NumericFilter_Free;
    ret->HasNext = NumericFilter_HasNext;
    ret->LastDocId = NumericFilter_LastDocId;
    ret->Read = NumericFilter_Read;
    ret->SkipTo = NumericFilter_SkipTo;
    return ret;
}


/*
*  Parse numeric filter arguments, in the form of:
*  <fieldname> min max
*
*  By default, the interval specified by min and max is closed (inclusive). 
*  It is possible to specify an open interval (exclusive) by prefixing the score with the character (. 
*  For example: "score (1 5"
*  Will return filter elements with 1 < score <= 5
* 
*  min and max can be -inf and +inf
*  
*  Returns a numeric filter on success, NULL if there was a problem with the arguments
*/
NumericFilter *ParseNumericFilter(RedisSearchCtx *ctx, RedisModuleString **argv, int argc) {
                                       
    if (argc != 3) {
        return NULL;
    }
    // make sure we have an index spec for this filter and it's indeed numeric
    size_t len;
    const char *f = RedisModule_StringPtrLen(argv[0], &len);
    FieldSpec *fs = IndexSpec_GetField( ctx->spec, f, len);
    if (fs == NULL || fs->type != F_NUMERIC) {
        return NULL;
    } 
    
    NumericIndex *ni = NewNumericIndex(ctx, fs);
    
    NumericFilter *nf = malloc(sizeof(NumericFilter));
    nf->idx = ni;
    nf->inclusiveMax = 1;
    nf->inclusiveMin = 1;
    nf->min = 0;
    nf->max = 0;
    nf->minNegInf = 0;
    nf->maxInf = 0;
    // Parse the min range
    
    // -inf means anything is acceptable as a minimum
    if (RMUtil_StringEqualsC(argv[1], "-inf")) {
        nf->minNegInf = 1;
    } else {
        // parse the min range value - if it's OK we just set the value
        if (RedisModule_StringToDouble(argv[1], &nf->min) != REDISMODULE_OK) {
            size_t len = 0;
            const char *p = RedisModule_StringPtrLen(argv[1], &len);
            
            // if the first character is ( we treat the minimum as exclusive
            if (*p == '(' && len > 1) {
                p++;
                nf->inclusiveMin = 0;
                // we need to create a temporary string to parse it again...
                RedisModuleString *s = RedisModule_CreateString(ctx->redisCtx, p, len-1);
                if (RedisModule_StringToDouble(s, &nf->min) != REDISMODULE_OK) {
                    RedisModule_FreeString(ctx->redisCtx, s);
                    goto error;
                }
                // free the string now that it's parsed
                RedisModule_FreeString(ctx->redisCtx, s);
                
            } else goto error; //not a number
        }
    }
    
    // check if the max range is +inf
    if (RMUtil_StringEqualsC(argv[2], "+inf")) {
        nf->maxInf = 1;
    } else {
        // parse the max range. OK means we just read it into nf->max
        if (RedisModule_StringToDouble(argv[2], &nf->max) != REDISMODULE_OK) {
            
            // check see if the first char is ( and this is an exclusive range
            size_t len = 0;
            const char *p = RedisModule_StringPtrLen(argv[2], &len);
            if (*p == '(' && len > 1) {
                p++;
                nf->inclusiveMax = 0;
                // now parse the number part of the 
                RedisModuleString *s = RedisModule_CreateString(ctx->redisCtx, p, len-1);
                if (RedisModule_StringToDouble(s, &nf->max) != REDISMODULE_OK) {
                    RedisModule_FreeString(ctx->redisCtx, s);
                    goto error;
                }
                RedisModule_FreeString(ctx->redisCtx, s);
                
            } else goto error; //not a number
        }
    }
    
    return nf;
    
    
error:
    free(nf->idx);
    free(nf);
    return NULL;
                                       
}
/*
** 2026 July 29
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file implements the matchertext syntactic discipline: the rule that
** the ASCII matcher pairs "()", "[]" and "{}" must always match.
**
** Two routines are provided, corresponding to the two algorithms of the
** matchertext development:
**
**   sqlite3MatchertextVerify()   VERIFY.  Decide whether a string is valid
**                                matchertext.  One linear scan.
**   sqlite3MatchertextEnd()      FINDEMBEDEND.  Given the opener of an
**                                embedding, locate its matching closer by
**                                matcher balance rather than by scanning
**                                for a terminator.
**
** Both are host-independent: they inspect only the six matcher characters
** and know nothing about SQL.  Neither routine allocates memory or modifies
** its input.  That is the point.  A value admitted
** by these checks is admitted by a passive verification, not rewritten by a
** content-modifying transformation, so a bug here rejects data that should
** have embedded rather than admitting a breakout.
**
** UTF-8 is scanned safely byte-by-byte.  Every matcher character is below
** 0x80, while every byte of a multi-byte UTF-8 sequence is at or above
** 0x80, so no part of a multi-byte character can be mistaken for a matcher.
** No decoding step is required, and none is performed.  The caller must
** supply the value in its final, fully decoded form: a check applied to any
** earlier, still-encoded representation is unsound, because such a value can
** acquire an unmatched matcher when a later stage decodes it.
*/
#ifdef SQLITE_ENABLE_MATCHERTEXT

#include "sqliteInt.h"

/*
** The maximum matcher nesting depth accepted.  Input nested more deeply than
** this is rejected.  Rejection is safe: the value does not embed and the
** statement fails to parse, which is the intended failure mode.  Deep nesting
** is vanishingly rare in practice; the deepest matched nesting observed in a
** 469-million-sample corpus of production source code was 500.
*/
#ifndef SQLITE_MAX_MATCHER_DEPTH
# define SQLITE_MAX_MATCHER_DEPTH 1000
#endif

/*
** The open matcher of each pending level is held on a stack of two-bit
** entries, 32 levels to a u64 word.  Level numbering is zero-based, so a
** current depth of d means levels 0 through d-1 are pending and MT_TOP()
** reads level d-1.  Every level is written by MT_PUSH() before it is read.
**
** The arguments of these macros are evaluated more than once and so must be
** free of side effects.
*/
#define MT_NWORD  ((SQLITE_MAX_MATCHER_DEPTH+31)/32)
#define MT_PUSH(A,D,K) A[(D)>>5] = (A[(D)>>5] & ~(((u64)3)<<(((D)&31)*2))) | (((u64)(K))<<(((D)&31)*2))
#define MT_TOP(A,D) ((int)((A[((D)-1)>>5] >> ((((D)-1)&31)*2)) & 3))

/*
** Classify one byte against the matcher pair set.  Returns a positive kind
** 1, 2 or 3 for an opener, the negation of that kind for the corresponding
** closer, and zero for a nonmatcher.  The sign carries open versus close and
** the magnitude carries the pair, so an opener of kind k is closed by, and
** only by, the byte that classifies as -k.
*/
static int mtClass(unsigned int c){
  switch( c ){
    case '(': return  1;
    case ')': return -1;
    case '[': return  2;
    case ']': return -2;
    case '{': return  3;
    case '}': return -3;
    default: return 0;
  }
}

/*
** Scan z[] maintaining the matcher stack.  If n is negative the scan runs to
** the first zero byte, otherwise it runs for exactly n bytes.  The scan also
** stops, short of either limit, at the first closer arriving while no opener
** is pending: that byte is the balance boundary and is not consumed.
**
** Write the number of bytes consumed into *pnUsed.  Return the matcher depth
** reached, which is never negative, or -1 if the input is not matchertext
** because a closer met an opener of the wrong kind or the depth limit was
** exceeded.
**
** A return of zero therefore means the bytes consumed are valid matchertext.
** A positive return means they are valid so far but leave openers pending.
*/
static int mtScan(const unsigned char *z, i64 n, i64 *pnUsed){
  u64 aStk[MT_NWORD];
  i64 i;
  int d = 0;
  int k;

  for(i=0; n<0 ? z[i]!=0 : i<n; i++){
    k = mtClass(z[i]);
    if( k==0 ) continue;
    if( k>0 ){
      if( d>=SQLITE_MAX_MATCHER_DEPTH ){
        *pnUsed = i;
        return -1;
      }
      MT_PUSH(aStk, d, k);
      d++;
    }else{
      if( d==0 ) break;
      if( MT_TOP(aStk, d)!= -k ){
        *pnUsed = i;
        return -1;
      }
      d--;
    }
  }
  *pnUsed = i;
  assert( d>=0 );
  return d;
}

/*
** VERIFY.  Return true if and only if z[0..n-1] is valid matchertext.
**
** The empty string is matchertext.  Zero bytes within the input are ordinary
** nonmatchers, so a value carrying them is handled like any other.
**
** A false return is not an error condition to be repaired.  It reports that
** the value cannot embed verbatim, and the caller must reject it or route it
** through an encoder that escapes its unmatched matchers.
*/
int sqlite3MatchertextVerify(const unsigned char *z, i64 n){
  i64 nUsed;
  assert( n>=0 );
  assert( z!=0 || n==0 );
  /* The scan stops early at an unmatched closer, so requiring that it
  ** consumed every byte is what rejects input such as "a)b". */
  return mtScan(z, n, &nUsed)==0 && nUsed==n;
}

/* The three routines below are host glue rather than scanner: they read the
** SQLite tokenizer and connection state, so they depend on the full internal
** header.  SQLITE_MATCHERTEXT_SCANNER_ONLY excludes them, which is what lets
** the cross-language scanner test compile this file against a stub header and
** keep the boundary routines self-contained. */
#ifndef SQLITE_MATCHERTEXT_SCANNER_ONLY

/* Verify SQL matcher balance without treating comment text as SQL input. */
int sqlite3MatchertextVerifySql(const unsigned char *z, i64 n){
  u64 aStk[MT_NWORD];
  i64 i = 0;
  int d = 0;

  assert( n>=0 );
  assert( z!=0 || n==0 );
  while( i<n ){
    int j;
    int tokenType;
    int nToken = sqlite3GetToken(z+i, &tokenType);
    if( nToken<=0 || nToken>n-i ) return 0;
    if( tokenType!=TK_COMMENT ){
      for(j=0; j<nToken; j++){
        int k = mtClass(z[i+j]);
        if( k>0 ){
          if( d>=SQLITE_MAX_MATCHER_DEPTH ) return 0;
          MT_PUSH(aStk, d, k);
          d++;
        }else if( k<0 ){
          if( d==0 || MT_TOP(aStk, d)!= -k ) return 0;
          d--;
        }
      }
    }
    i += nToken;
  }
  return d==0;
}

/* Set the migration error returned by a disabled legacy SQL entry point. */
int sqlite3MatchertextLegacyError(sqlite3 *db, const char *zReplacement){
  int rc;
  if( !sqlite3SafetyCheckOk(db) ) return SQLITE_MISUSE_BKPT;
  sqlite3_mutex_enter(db->mutex);
  sqlite3ErrorWithMsg(db, SQLITE_MISUSE,
      "matchertext requires %s; pass inputs separately", zReplacement);
  rc = sqlite3ApiExit(db, SQLITE_MISUSE);
  sqlite3_mutex_leave(db->mutex);
  return rc;
}

#ifdef SQLITE_MATCHERTEXT_STRICT
/* Legacy SQL entry points remain available only to SQLite's own parsers. */
int sqlite3MatchertextInternalAllowed(sqlite3 *db){
  Parse *pParse = db->pParse;
  return db->init.busy
      || db->nSqlExec                        /* inside OP_SqlExec: SQLite's own SQL */
      || db->nMatchertextInternal
      || (pParse!=0 && pParse->eParseMode!=PARSE_MODE_NORMAL);
}

/* Refuse a legacy SQL entry point unless SQLite itself is the caller.  The
** caller state is read under the connection mutex, which every internal path
** holds while that state is set, so one thread cannot pass for internal on the
** strength of another thread's work.  An invalid connection is left to the
** entry point's own misuse check. */
int sqlite3MatchertextLegacyCheck(sqlite3 *db, const char *zReplacement){
  int rc = SQLITE_OK;
  if( !sqlite3SafetyCheckOk(db) ) return SQLITE_OK;
  sqlite3_mutex_enter(db->mutex);
  if( !sqlite3MatchertextLegacyAllowed(db) ){
    rc = sqlite3MatchertextLegacyError(db, zReplacement);
  }
  sqlite3_mutex_leave(db->mutex);
  return rc;
}

/* The internal prepare entry points.  Each marks the prepare internal and
** holds the connection mutex across it.  sqlite3InternalExec() in legacy.c
** prepares through these. */
int sqlite3InternalPrepareV2(
  sqlite3 *db,
  const char *zSql,
  int nByte,
  sqlite3_stmt **ppStmt,
  const char **pzTail
){
  int rc;
  sqlite3_mutex_enter(db->mutex);
  db->nMatchertextInternal++;
  rc = sqlite3_prepare_v2(db, zSql, nByte, ppStmt, pzTail);
  db->nMatchertextInternal--;
  sqlite3_mutex_leave(db->mutex);
  return rc;
}
int sqlite3InternalPrepareV3(
  sqlite3 *db,
  const char *zSql,
  int nByte,
  unsigned int prepFlags,
  sqlite3_stmt **ppStmt,
  const char **pzTail
){
  int rc;
  sqlite3_mutex_enter(db->mutex);
  db->nMatchertextInternal++;
  rc = sqlite3_prepare_v3(db, zSql, nByte, prepFlags, ppStmt, pzTail);
  db->nMatchertextInternal--;
  sqlite3_mutex_leave(db->mutex);
  return rc;
}
#endif /* SQLITE_MATCHERTEXT_STRICT */

#endif /* SQLITE_MATCHERTEXT_SCANNER_ONLY */

/*
** FINDEMBEDEND.  If z[0] opens an embedding "o m c" in zero-terminated text,
** locate c by matcher balance and return the total length of the embedding,
** so that the embedded value m is the nUsed-2 bytes at z[1] where nUsed is
** the return value.  Return 0 if z[0] is not an opener at all.  Return -1 if
** z[0] opens an embedding that does not end here: the balance never returns
** to the level of z[0], a closer of the wrong kind arrives, or the text ends
** first.  The two failures differ for the caller: no opener means no hole,
** while an opener without its closer is a hole that must not be read any
** other way.
**
** Deciding whether z[0] opens an embedding is left here rather than asked of
** the caller, so that the matcher pairs stay written down in exactly one
** place.  A caller that had to test for an opener itself would be a second
** copy of the configuration, free to drift from this one.
**
** The scan of m begins one byte past the opener, and the byte that first
** drives the depth below zero is exactly the terminating closer.  This is the
** operational form of the boundary result proved in embed_boundary.lean: for
** valid matchertext m every prefix of m has non-negative depth, m itself
** returns to depth zero, and the following closer takes the depth to -1.  The
** value may therefore hold any character at all, quotes and the host's own
** terminators included, provided only that its matchers balance.
**
** This routine locates the boundary and nothing else.  Whatever encloses the
** embedding, a quote in SQL or a tag in markup, remains the caller's concern,
** which is what keeps this file free of any one host language.
*/
i64 sqlite3MatchertextEnd(const unsigned char *z){
  i64 nUsed;
  int k;

  assert( z!=0 );
  k = mtClass(z[0]);
  if( k<=0 ) return 0;

  /* Anything other than a clean return to depth zero means the value is not
  ** matchertext, or the text ran out with openers still pending. */
  if( mtScan(z+1, -1, &nUsed)!=0 ) return -1;

  /* The boundary byte closes z[0] only if it is of the matching kind.  This
  ** also rejects the case where the text ended, since a zero byte classifies
  ** as a nonmatcher and can never equal -k. */
  if( mtClass(z[1+nUsed])!= -k ) return -1;

  return nUsed+2;
}

/*
** The escape alphabet used by TOMATCHERTEXT and FROMMATCHERTEXT below.
**
**   \o()   \c()   a literal '(' or ')'
**   \o[]   \c[]   a literal '[' or ']'
**   \o{}   \c{}   a literal '{' or '}'
**
** These are the forms table 1 of the matchertext paper proposes, and the
** letters follow the Unicode character classes: 'o' for Open Punctuation and
** 'c' for Close Punctuation.
**
** Each escape names its pair by carrying that pair, matched.  So an escape is
** itself matchertext, and the escape mechanism obeys the discipline it serves
** rather than sitting outside it.  A scanner counting balance sees \o() go up
** by one and back down, never negative and never leaving anything pending.
**
** In the Lean development, to_matchertext.lean derives the output-is-
** matchertext theorems (toMatchertext_mt, toMatchertextC_mt) from
**
**   hesc : forall c, MT Pi (esc c) /\ depth Pi (esc c) = 0
**
** and esc_mt_paired discharges it for these escapes, which are matchertext
** by the nesting rule: a backslash, a tag, and a matched pair.
**
** The backslash is the introducer.  It is not escaped itself: a backslash
** decodes as an escape only before 'o' or 'c' and an adjacent matched pair,
** so the encoder escapes that pair where the input already reads as an
** escape, and every other backslash passes through as an ordinary byte.
*/
#define MT_ESC '\\'

/* Return the three characters following the introducer for matcher kind k, or
** 0 if k is not a matcher.  The first says open or close, the pair that
** follows names which matcher is meant and keeps the escape balanced. */
static const char *mtEscape(int k){
  switch( k ){
    case  1: return "o()";
    case -1: return "c()";
    case  2: return "o[]";
    case -2: return "c[]";
    case  3: return "o{}";
    case -3: return "c{}";
  }
  return 0;
}

/* The inverse of mtEscape().  Given the three characters after the
** introducer, return the byte they stand for, or 0 if they are not an escape.
** The pair must be a real matcher pair, so "\o(]" is not an escape. */
static int mtUnescape(int c1, int c2, int c3){
  int k = mtClass((unsigned int)c2);
  if( k<=0 || mtClass((unsigned int)c3)!= -k ) return 0;
  if( c1=='o' ) return c2;
  if( c1=='c' ) return c3;
  return 0;
}

/*
** TOMATCHERTEXT.  Encode z[0..n-1] so that the result is valid matchertext,
** whatever the input was.  Return a zero-terminated buffer from
** sqlite3_malloc64(), or 0 if allocation fails, and write its length to
** *pnOut when pnOut is not 0.  The caller frees it with sqlite3_free().
**
** Only unmatched matchers are escaped, and the matched pair of any run that
** already reads as an escape, a backslash, 'o' or 'c' and an adjacent matched
** pair, so that the run decodes back to itself.  A value whose matchers
** already balance and which holds no such run therefore passes through byte
** for byte, which is what keeps the common case readable in the SQL text.
** Exception: nesting at the depth limit exactly gets its deepest pair
** escaped; see pass one below.
**
** This is the total counterpart of VERIFY.  VERIFY answers whether a value
** may embed as it stands and refuses the ones that may not; this refuses
** nothing, so a free-form field carrying "smile :]" or 'printf("[")' can
** still be placed in a hole.  The price is that the value is no longer
** verbatim, so a reader has to run FROMMATCHERTEXT to get the bytes back.
** The value hole M'(...)' and the name hole [...] share this one alphabet.
*/
char *sqlite3MatchertextEncode(const char *z, i64 n, i64 *pnOut){
  unsigned char *aEsc;      /* aEsc[i] is true if z[i] must be escaped */
  char *zOut;
  i64 i, j, nOut;
  int d = 0;
  int k;
  /* Positions of openers not yet matched.  There are fewer than n and fewer
  ** than the depth cap below, so the array has the smaller of the two sizes.
  ** It shares the heap allocation of aEsc: a cap-sized array on the stack
  ** would be 8 KB, too much for SQLITE_SMALL_STACK.  Positions are i64: an
  ** int would truncate past 2 GiB. */
  i64 *aStk;
  i64 nStk;

  assert( n>=0 );
  assert( z!=0 || n==0 );

  if( n==0 ){
    zOut = sqlite3_malloc64(1);
    if( zOut ) zOut[0] = 0;
    if( pnOut ) *pnOut = 0;
    return zOut;
  }

  nStk = n<SQLITE_MAX_MATCHER_DEPTH ? n : SQLITE_MAX_MATCHER_DEPTH;
  aStk = sqlite3_malloc64(sizeof(i64)*(sqlite3_uint64)nStk + (sqlite3_uint64)n);
  if( aStk==0 ) return 0;
  aEsc = (unsigned char*)&aStk[nStk];
  memset(aEsc, 0, (size_t)n);

  /* Pass one: find the matchers that are not matched.  Nesting past
  ** SQLITE_MAX_MATCHER_DEPTH-1 is escaped as if unmatched; escapes carry
  ** one pair of their own, so the output stays within the host's limit. */
  for(i=0; i<n; i++){
    k = mtClass((unsigned char)z[i]);
    if( k==0 ) continue;
    if( k>0 ){
      if( d>=SQLITE_MAX_MATCHER_DEPTH-1 ){
        aEsc[i] = 1;
      }else{
        assert( d<nStk );
        aStk[d++] = i;
      }
    }else if( d>0 && mtClass((unsigned char)z[aStk[d-1]])== -k ){
      d--;
    }else{
      aEsc[i] = 1;
    }
  }
  while( d>0 ) aEsc[aStk[--d]] = 1;
  /* A run that already reads as an escape keeps its backslash and has its
  ** pair escaped.  An unescaped opener here was pushed in pass one, so the
  ** adjacent closer matched it, and escaping both keeps the output balanced.
  ** An opener already escaped at the depth limit breaks the run by itself. */
  for(i=0; i+3<n; i++){
    if( z[i]==MT_ESC && !aEsc[i+2]
     && mtUnescape((unsigned char)z[i+1], (unsigned char)z[i+2],
                   (unsigned char)z[i+3])!=0
    ){
      aEsc[i+2] = aEsc[i+3] = 1;
    }
  }

  /* Pass two: size the result, then fill it.  An escaped matcher costs three
  ** extra bytes. */
  nOut = n;
  for(i=0; i<n; i++){
    if( aEsc[i] ) nOut += 3;
  }
  zOut = sqlite3_malloc64((sqlite3_uint64)nOut + 1);
  if( zOut==0 ){
    sqlite3_free(aStk);
    return 0;
  }
  for(i=0, j=0; i<n; i++){
    if( aEsc[i] ){
      const char *zEsc = mtEscape(mtClass((unsigned char)z[i]));
      assert( zEsc!=0 );
      zOut[j++] = MT_ESC;
      zOut[j++] = zEsc[0];
      zOut[j++] = zEsc[1];
      zOut[j++] = zEsc[2];
    }else{
      zOut[j++] = z[i];
    }
  }
  assert( j==nOut );
  zOut[j] = 0;

  sqlite3_free(aStk);
  if( pnOut ) *pnOut = nOut;
  return zOut;
}

/*
** FROMMATCHERTEXT.  Undo TOMATCHERTEXT.  Return a zero-terminated buffer from
** sqlite3_malloc64(), or 0 if allocation fails, and write its length to
** *pnOut when pnOut is not 0.
**
** A backslash that does not introduce one of the escapes above is an
** ordinary byte, as in stock SQLite, and is copied through unchanged.  The
** encoder escapes the pair of any input run that would otherwise decode, so
** decoding an encoded value returns the original, which is what the round
** trip test asserts.
**
** This routine is a data-fidelity convenience and carries no part of the
** security argument, which rests on the encoder's output being matchertext.
**
** The in-place form is what the parser uses, so that reading a value costs no
** allocation; the allocating form is for callers holding const input.
*/
i64 sqlite3MatchertextDecodeInPlace(char *z, i64 n){
  i64 i, j;
  int c;

  assert( n>=0 );
  assert( z!=0 || n==0 );

  /* Decoding only ever shortens, so the result can overwrite the input as the
  ** scan advances: j never runs ahead of i. */
  for(i=0, j=0; i<n; ){
    if( z[i]==MT_ESC && i+3<n
           && (c = mtUnescape((unsigned char)z[i+1], (unsigned char)z[i+2],
                              (unsigned char)z[i+3]))!=0 ){
      z[j++] = (char)c;
      i += 4;
    }else{
      z[j++] = z[i];
      i++;
    }
    assert( j<=i );
  }
  z[j] = 0;
  return j;
}

/*
** Public interfaces to the two routines above, for an application that
** places a value in a hole itself rather than through the %m conversion.
** A negative n means the input is zero-terminated.
*/
int sqlite3_matchertext_verify(const char *z, sqlite3_int64 n){
#ifdef SQLITE_ENABLE_API_ARMOR
  if( z==0 ) return 0;
#endif
  if( n<0 ) n = (i64)strlen(z);
  return sqlite3MatchertextVerify((const unsigned char*)z, n);
}
char *sqlite3_matchertext_encode(const char *z, sqlite3_int64 n,
                                 sqlite3_int64 *pn){
#ifdef SQLITE_ENABLE_API_ARMOR
  if( z==0 ){ (void)SQLITE_MISUSE_BKPT; return 0; }
#endif
  if( n<0 ) n = (i64)strlen(z);
  return sqlite3MatchertextEncode(z, n, pn);
}
char *sqlite3_matchertext_decode(const char *z, sqlite3_int64 n,
                                 sqlite3_int64 *pn){
  char *zOut;
#ifdef SQLITE_ENABLE_API_ARMOR
  if( z==0 ){ (void)SQLITE_MISUSE_BKPT; return 0; }
#endif
  if( n<0 ) n = (i64)strlen(z);
  zOut = sqlite3_malloc64((sqlite3_uint64)n + 1);
  if( zOut==0 ) return 0;
  if( n>0 ) memcpy(zOut, z, (size_t)n);
  zOut[n] = 0;
  n = sqlite3MatchertextDecodeInPlace(zOut, n);
  if( pn ) *pn = n;
  return zOut;
}

#ifndef SQLITE_MATCHERTEXT_SCANNER_ONLY
/* True if stock SQLite reads the name hole [z[0..n-1]] as the same name this
** build does: with no bracket inside, both end it at the same ']', and with
** no backslash, there is no escape to decode. */
static int mtStockName(const char *z, i64 n){
  i64 i;
  for(i=0; i<n; i++){
    if( z[i]=='[' || z[i]==']' || z[i]==MT_ESC ) return 0;
  }
  return 1;
}

/*
** Rewrite the holes of zSql, a statement from sqlite3DbMalloc() about to be
** stored in the schema, into stock SQLite syntax, so that a database this
** build writes stays readable by a build without matchertext.  A value hole
** M'(...)' becomes a '...' literal of its value.  A name hole [...] stays as
** it is when stock SQLite reads the same name from it, and becomes `...`
** otherwise: stock SQLite reads a grave-accent name as a name only, where a
** "..." name that resolves to no column reads as a string under the
** double-quoted-string fallback.  Return the result, which is zSql itself when nothing changes.
** After an error zSql is freed and the return is 0: an OOM sets
** db->mallocFailed, and a result too long is reported to the parser running,
** if there is one.
*/
/* Append z to p as a name in grave accents, each accent in it doubled. */
static void mtAppendGraveName(sqlite3_str *p, const char *z){
  sqlite3_str_appendchar(p, 1, '`');
  for(; *z; z++){
    sqlite3_str_appendchar(p, *z=='`' ? 2 : 1, *z);
  }
  sqlite3_str_appendchar(p, 1, '`');
}

char *sqlite3MatchertextStockSql(sqlite3 *db, char *zSql){
  StrAccum acc;
  i64 i = 0;          /* Offset of the current token */
  i64 iCopied = 0;    /* Bytes of zSql already appended to acc */
  int bChanged = 0;   /* True once a hole is rewritten */

  if( zSql==0 ) return 0;
  sqlite3StrAccumInit(&acc, db, 0, 0, db->aLimit[SQLITE_LIMIT_LENGTH]);
  while( zSql[i] && !db->mallocFailed ){
    int tokenType;
    i64 n = sqlite3GetToken((const u8*)&zSql[i], &tokenType);
    int bName = tokenType==TK_ID && zSql[i]=='[';
    if( tokenType==TK_MTSTRING
     || (bName && !mtStockName(&zSql[i+1], n-2))
    ){
      i64 nBody = bName ? n-2 : n-5;
      char *zBody = sqlite3DbStrNDup(db, &zSql[i + (bName ? 1 : 3)], nBody);
      if( zBody ){
        sqlite3MatchertextDecodeInPlace(zBody, nBody);
        sqlite3_str_append(&acc, &zSql[iCopied], (int)(i-iCopied));
        if( bName ){
          mtAppendGraveName(&acc, zBody);
        }else{
          sqlite3_str_appendf(&acc, "%Q", zBody);
        }
        sqlite3DbFree(db, zBody);
        iCopied = i+n;
        bChanged = 1;
      }
    }
    i += n;
  }
  if( !bChanged && !db->mallocFailed ) return zSql;
  sqlite3_str_append(&acc, &zSql[iCopied], (int)(i-iCopied));
  sqlite3DbFree(db, zSql);
  if( db->mallocFailed ){
    sqlite3_str_reset(&acc);
    return 0;
  }
  return sqlite3StrAccumFinish(&acc);
}
#endif /* SQLITE_MATCHERTEXT_SCANNER_ONLY */

#endif /* SQLITE_ENABLE_MATCHERTEXT */

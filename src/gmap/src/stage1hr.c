static char rcsid[] = "$Id: stage1hr.c 131715 2014-03-28 00:14:18Z twu $";
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifndef HAVE_MEMCPY
#define memcpy(d,s,n) bcopy((s),(d),(n))
#endif
#ifndef HAVE_MEMMOVE
#define memmove(d,s,n) bcopy((s),(d),(n))
#endif

#include "stage1hr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>		/* For memset() */
#include <math.h>
#include <ctype.h>		/* for tolower() */
#include "assert.h"
#include "mem.h"
#include "reader.h"
#include "oligo.h"
#include "indexdb.h"
#include "indexdb_hr.h"
#include "list.h"
#include "listdef.h"
#include "intlist.h"
#include "intlistdef.h"
#include "splice.h"
#include "indel.h"
#include "stage3hr.h"
#include "substring.h"
#include "complement.h"
#include "compress.h"
#include "genome_hr.h"
#include "genome_sites.h"
#include "maxent.h"
#include "maxent_hr.h"
#include "iitdef.h"
#include "univinterval.h"

#include "sarray-read.h"
#include "spanningelt.h"
#include "cmet.h"
#include "atoi.h"

#include "stage2.h"
#include "stage3.h"


#ifdef WORDS_BIGENDIAN
#include "bigendian.h"
#endif


#ifdef HAVE_64_BIT
#ifdef LARGE_GENOMES
#else
#define DIAGONAL_ADD_QUERYPOS 1
#endif
#endif


/* Note: Heapsort still turns out to be a bit faster than a global
   qsort, because it takes advantage of the fact that the positions
   within each batch are already sorted.  Also, heapsort can handle
   8-byte positions. */


#define NO_EXTENSIONS_BEFORE_ZERO 1

/* #define EXTRACT_GENOMICSEG 1 */
#ifdef EXTRACT_GENOMICSEG
#define MAX_INDEXSIZE 8
#endif

/* Note: MAX_READLENGTH is defined externally by configure */
#ifndef MAX_READLENGTH
#error A default value for MAX_READLENGTH was not provided to configure
#endif


/* MAX_NALIGNMENTS of 2 vs 1 gets 1600 improvements in 275,000 reads */
/* MAX_NALIGNMENTS of 3 vs 2 gets 96 improvements in 275,000 reads */
#define MAX_NALIGNMENTS 3

static bool use_sarray_p = true;

/* Mode */
static Mode_T mode;
static bool snpp;
static int maxpaths_search;

/* For spliceable (really "joinable", if we consider indels) */
static Chrpos_T overall_max_distance;

/* Other distances */
static Chrpos_T max_middle_insertions;
static Chrpos_T max_middle_deletions;
static Chrpos_T shortsplicedist;
static Chrpos_T shortsplicedist_known;
static Chrpos_T shortsplicedist_novelend;


/* Penalties */
static int terminal_threshold;
static int terminal_output_minlength;

static bool novelsplicingp;
static bool knownsplicingp;
static bool distances_observed_p;

static Chrpos_T min_intronlength;

/* Splicing */
static Univcoord_T *splicesites;
static Splicetype_T *splicetypes;
static Chrpos_T *splicedists;
static int nsplicesites;

static int min_distantsplicing_end_matches;
static int min_distantsplicing_identity;


/* GMAP parameters */
static bool gmap_pairsearch_p;
static bool gmap_indel_knownsplice_p;
static bool gmap_terminal_p;
static bool gmap_improvement_p;
static bool gmap_rerun_p = true;

static int antistranded_penalty;

static int nullgap;
static int maxpeelback;
static int maxpeelback_distalmedial;
static int extramaterial_end;
static int extramaterial_paired;
static int trigger_score_for_gmap;
static int gmap_allowance;
static int max_gmap_pairsearch;
static int max_gmap_terminal;
static int max_gmap_improvement;

static int sufflookback = 60;
static int nsufflookback = 5;
static int extraband_single = 3;
static int extraband_end = 3;  /* Shouldn't differ from 0, since onesidegapp is true? */
static int extraband_paired = 7;
static int minendexon = 9;
static int ngap = 3;  /* 0? */




#define A_CHAR 0x0
#define C_CHAR 0x1
#define G_CHAR 0x2
#define T_CHAR 0x3

#define NOT_APPLICABLE true

/* Originally allowed only 1, to print only unique translocations.
   But need to allow enough to avoid missing some translocations. */
#define MAXCHIMERAPATHS 100

#define NREQUIRED_FAST 2	/* For candidate generation using
				   multimiss.  A value of 2 implies 
				   specificity of a 24-mer, which
				   should be low for a human-sized
				   genome */

#define MAX_INDEX1INTERVAL 3
#define STAGE2_MIN_OLIGO 3	/* Actually 6, but we are adding index1interval to this */
#define GOOD_GMAP_END 6
#define GMAP_TERMINAL_TRIM 6

#define GREEDY_SHORTSPLICEDIST 30000

static int index1part;
static int index1interval;
static int spansize;
static int two_index1intervals;
static int min_readlength;
static Univ_IIT_T chromosome_iit;
static int circular_typeint;

static Univcoord_T *chroffsets;
static Univcoord_T *chrhighs;
static Chrpos_T *chrlengths; /* May differ from chrhigh - chroffset in circular chromosomes */
static int nchromosomes;

static int leftreadshift;
static unsigned int oligobase_mask; /* same as kmer_mask */
static int one_miss_querylength;

static int end_miss_one;	/* Used for computing max_terminal_length */
static int end_miss_two;	/* Used for computing max_terminal_length */


/* On 5' end, x = querypos.  On 3' end, x = (query_lastpos - querypos). */
#define FLOOR_END(x) ((x < index1interval) ? 0 : (x + spansize - index1interval)/spansize)

/* Here, x = (querypos - last_querypos).  Was (x-3)/12, but the new formula handles indels. */
#define FLOOR_MIDDLE(x) ((x < two_index1intervals) ? 0 : (x + spansize - two_index1intervals)/spansize)


#define MAX_LOCALSPLICING_HITS 10000
#define MAX_LOCALSPLICING_POTENTIAL 50


/* Overall flow */
#ifdef DEBUG
#define debug(x) x
#else
#define debug(x)
#endif

/* identify_segments */
#ifdef DEBUG1
#define debug1(x) x
#else
#define debug1(x)
#endif

/* Indels */ 
#ifdef DEBUG2
#define debug2(x) x
#else
#define debug2(x)
#endif

/* Indels, end */ 
#ifdef DEBUG2E
#define debug2e(x) x
#else
#define debug2e(x)
#endif

/* Floors */
#ifdef DEBUG3
#define debug3(x) x
#else
#define debug3(x)
#endif

/* find_singlesplices */ 
#ifdef DEBUG4P
#define debug4p(x) x
#else
#define debug4p(x)
#endif

/* find_doublesplices */
#ifdef DEBUG4D
#define debug4d(x) x
#else
#define debug4d(x)
#endif

/* find_splicesplices */
#ifdef DEBUG4S
#define debug4s(x) x
#else
#define debug4s(x)
#endif

/* find_known_doublesplices */
#ifdef DEBUG4K
#define debug4k(x) x
#else
#define debug4k(x)
#endif


/* find_splicepairs_distant */
#ifdef DEBUG4L
#define debug4l(x) x
#else
#define debug4l(x)
#endif

/* find_splicepairs_distant (details) */
#ifdef DEBUG4LD
#define debug4ld(x) x
#else
#define debug4ld(x)
#endif

/* find_spliceends_shortend and find_spliceends_distant */
#ifdef DEBUG4E
#define debug4e(x) x
#else
#define debug4e(x)
#endif

/* Terminals */
#ifdef DEBUG4T
#define debug4t(x) x
#else
#define debug4t(x)
#endif

/* Short overlaps */ 
#ifdef DEBUG4H
#define debug4h(x) x
#else
#define debug4h(x)
#endif

/* Heapify */
#ifdef DEBUG6
#define debug6(x) x
#else
#define debug6(x)
#endif

/* Identify exact/onemiss/multimiss matches */
#ifdef DEBUG7
#define debug7(x) x
#else
#define debug7(x)
#endif

/* Identify onemiss matches, list contents */
#ifdef DEBUG7A
#define debug7a(x) x
#else
#define debug7a(x)
#endif

/* binary_search */
#ifdef DEBUG10
#define debug10(x) x
#else
#define debug10(x)
#endif

/* straddling at beginning of genome.  May want to turn on DEBUG11 in indexdb_hr.c */
#ifdef DEBUG11
#define debug11(x) x
#else
#define debug11(x)
#endif

/* dual_search for known splice sites */
#ifdef DEBUG12
#define debug12(x) x
#else
#define debug12(x)
#endif

/* GMAP */ 
#ifdef DEBUG13
#define debug13(x) x
#else
#define debug13(x)
#endif

/* GMAP dump pairs */ 
#ifdef DEBUG13A
#define debug13a(x) x
#else
#define debug13a(x)
#endif

/* GMAP details of diagonals used */ 
#ifdef DEBUG13B
#define debug13b(x) x
#else
#define debug13b(x)
#endif

/* identify_all_segments */
#ifdef DEBUG14
#define debug14(x) x
#else
#define debug14(x)
#endif

/* binary search method for updating chrnum */
#ifdef DEBUG15
#define debug15(x) x
#else
#define debug15(x)
#endif


/* consolidate_paired_results */ 
#ifdef DEBUG16
#define debug16(x) x
#else
#define debug16(x)
#endif


typedef struct Segment_T *Segment_T;
struct Segment_T {
  int splicesites_i;		/* if no splicesites_iit, then splicesites_i is -1 */
  Univcoord_T diagonal;
  Univcoord_T chroffset;
  Univcoord_T chrhigh;
  Chrpos_T chrlength;
  Chrnum_T chrnum;

  int querypos5;
  int querypos3;

  int floor;
  int floor_xfirst;
  int floor_xlast;

  int floor_left;
  int floor_right;

  int leftmost;			/* For segmenti of local splice */
  int rightmost;		/* For segmentj of local splice */
  bool left_splice_p; /* Set by find_singlesplices, used by find_doublesplices for speed */
  bool right_splice_p; /* Set by find_singlesplices, used by find_doublesplices for speed */

  bool usedp;

#if 0
  int leftspan;			/* For segmentm of double splice */
  int rightspan;
#endif
};


struct Floors_T {
  int *allocated0;
  int *prev_omitted;

  int **allocated2;
  int *allocated1;
  int **scorefrom;		/* [from][to] */

  int **allocated4;
  int *allocated3;
  int **scoreto;		/* [to][from] */
};


void
Floors_free (Floors_T *old) {
  FREE((*old)->allocated1);
  FREE((*old)->allocated2);

  FREE((*old)->allocated3);
  FREE((*old)->allocated4);

  if ((*old)->allocated0) {
    FREE((*old)->allocated0);
  }

  FREE(*old);

  return;
}

void
Floors_free_keep (Floors_T *old) {

  FREE_KEEP((*old)->allocated1);
  FREE_KEEP((*old)->allocated2);

  FREE_KEEP((*old)->allocated3);
  FREE_KEEP((*old)->allocated4);

  if ((*old)->allocated0) {
    FREE_KEEP((*old)->allocated0);
  }

  FREE_KEEP(*old);

  return;
}

#ifdef DEBUG3
static void
Floors_print (Floors_T floors, int query_lastpos) {
  int from, to;

  if (floors->prev_omitted) {
    for (to = -index1interval; to <= query_lastpos+index1interval; to++) {
      printf("querypos %d, prev_omitted %d\n",to,floors->prev_omitted[to]);
    }
  }

  for (from = -index1interval; from <= query_lastpos+index1interval; from++) {
    for (to = from+1; to <= query_lastpos+index1interval; to++) {
      printf("from %d to %d, floor_score %d or %d",
	     from,to,floors->scorefrom[from][to],floors->scoreto[to][from]);
      if (floors->prev_omitted) {
	printf(" (prev %d)",floors->prev_omitted[to]);
      }
      printf("\n");
    }
  }

  return;
}
#endif


static Floors_T
Floors_new_standard (int querylength, int max_end_insertions, bool keep_floors_p) {
  Floors_T new;
  int query_lastpos, pos, from, to;
  int halfextra, extra;

  if (max_end_insertions < index1interval) {
    halfextra = index1interval;
  } else {
    halfextra = max_end_insertions;
  }
  extra = 1 + halfextra + halfextra;


  query_lastpos = querylength - index1part;

  if (keep_floors_p == true) {
    new = (Floors_T) MALLOC_KEEP(sizeof(*new));
  } else {
    new = (Floors_T) MALLOC(sizeof(*new));
  }
  new->allocated0 = (int *) NULL;
  new->prev_omitted = (int *) NULL;

  if (keep_floors_p == true) {
    new->allocated2 = (int **) CALLOC_KEEP(query_lastpos+extra,sizeof(int *));
    new->allocated1 = (int *) CALLOC_KEEP((query_lastpos+extra)*(query_lastpos+extra),sizeof(int));
  } else {
    new->allocated2 = (int **) CALLOC(query_lastpos+extra,sizeof(int *));
    new->allocated1 = (int *) CALLOC((query_lastpos+extra)*(query_lastpos+extra),sizeof(int));
  }
  new->allocated2[0] = &(new->allocated1[halfextra]);
  for (pos = 1; pos < query_lastpos+extra; pos++) {
    new->allocated2[pos] = &(new->allocated2[pos-1][query_lastpos+extra]);
  }
  new->scorefrom = &(new->allocated2[halfextra]);


  if (keep_floors_p == true) {
    new->allocated4 = (int **) CALLOC_KEEP(query_lastpos+extra,sizeof(int *));
    new->allocated3 = (int *) CALLOC_KEEP((query_lastpos+extra)*(query_lastpos+extra),sizeof(int));
  } else {
    new->allocated4 = (int **) CALLOC(query_lastpos+extra,sizeof(int *));
    new->allocated3 = (int *) CALLOC((query_lastpos+extra)*(query_lastpos+extra),sizeof(int));
  }
  new->allocated4[0] = &(new->allocated3[halfextra]);
  for (pos = 1; pos < query_lastpos+extra; pos++) {
    new->allocated4[pos] = &(new->allocated4[pos-1][query_lastpos+extra]);
  }
  new->scoreto = &(new->allocated4[halfextra]);

  for (to = -halfextra; to <= query_lastpos+halfextra; to++) {
    for (from = -halfextra; from < to; from++) {
      new->scorefrom[from][to] = new->scoreto[to][from] = FLOOR_MIDDLE(to - from);
    }
  }

  debug3(printf("Floors standard:\n"));
  debug3(Floors_print(new,query_lastpos));
  return new;
}


static Floors_T
Floors_new_omitted (int querylength, int max_end_insertions, bool *omitted) {
  Floors_T new;
  int query_lastpos, querypos, pos, from, to;
  int prev;
  int halfextra, extra;

  if (max_end_insertions < index1interval) {
    halfextra = index1interval;
  } else {
    halfextra = max_end_insertions;
  }
  extra = 1 + halfextra + halfextra;

  query_lastpos = querylength - index1part;
  new = (Floors_T) MALLOC(sizeof(*new));
  new->allocated0 = (int *) CALLOC(query_lastpos+extra,sizeof(int));
  new->prev_omitted = &(new->allocated0[halfextra]);

  new->allocated2 = (int **) CALLOC(query_lastpos+extra,sizeof(int *));
  new->allocated1 = (int *) CALLOC((query_lastpos+extra)*(query_lastpos+extra),sizeof(int));
  new->allocated2[0] = &(new->allocated1[halfextra]);
  for (pos = 1; pos < query_lastpos+extra; pos++) {
    new->allocated2[pos] = &(new->allocated2[pos-1][query_lastpos+extra]);
  }
  new->scorefrom = &(new->allocated2[halfextra]);


  new->allocated4 = (int **) CALLOC(query_lastpos+extra,sizeof(int *));
  new->allocated3 = (int *) CALLOC((query_lastpos+extra)*(query_lastpos+extra),sizeof(int));
  new->allocated4[0] = &(new->allocated3[halfextra]);
  for (pos = 1; pos < query_lastpos+extra; pos++) {
    new->allocated4[pos] = &(new->allocated4[pos-1][query_lastpos+extra]);
  }
  new->scoreto = &(new->allocated4[halfextra]);


  /* Set up omitted.  Save for middle_indels computation. */
  prev = -1;
  for (querypos = -halfextra; querypos < 0; querypos++) {
    new->prev_omitted[querypos] = -1;
  }
  for ( ; querypos <= query_lastpos; querypos++) {
    new->prev_omitted[querypos] = prev;
    if (omitted[querypos] == true) {
      prev = querypos;
    }
  }
  for ( ; querypos <= query_lastpos+halfextra; querypos++) {
    new->prev_omitted[querypos] = prev;
  }


  for (to = -halfextra; to <= query_lastpos+halfextra; to++) {
    prev = new->prev_omitted[to];
    for (from = -halfextra; from < prev; from++) {
      new->scorefrom[from][to] = new->scorefrom[from][prev] + FLOOR_MIDDLE(to - prev);
    }
    for ( ; from < to; from++) {
      new->scorefrom[from][to] = FLOOR_MIDDLE(to - from);
    }
  }

  for (to = -halfextra; to <= query_lastpos+halfextra; to++) {
    for (from = -halfextra; from < to; from++) {
      new->scoreto[to][from] = new->scorefrom[from][to];
    }
  }

  debug3(
	 printf("Floors omitted:");
	 for (pos = 0; pos <= query_lastpos; pos++) {
	   if (omitted[pos] == true) {
	     printf(" %d",pos);
	   }
	 }
	 printf("\n");
	 )
  debug3(Floors_print(new,query_lastpos));
  return new;
}


/************************************************************************/


#define T Stage1_T
struct T {
  List_T plus_spanningset[MAX_INDEX1INTERVAL];
  List_T minus_spanningset[MAX_INDEX1INTERVAL];

  Univcoord_T **plus_positions_allocated;
  Univcoord_T **plus_positions; /* points to above[index1interval-1] */
  Univcoord_T **minus_positions_allocated;
  Univcoord_T **minus_positions; /* points to above[index1interval-1] */

  int *plus_npositions_allocated;
  int *plus_npositions;		/* points to above[index1interval-1] */

  int *minus_npositions_allocated;
  int *minus_npositions;	/* points to above[index1interval-1] */

  bool *plus_retrievedp_allocated;
  bool *plus_retrievedp;	/* points to above[index1interval-1] */
  bool *minus_retrievedp_allocated;
  bool *minus_retrievedp;	/* points to above[index1interval-1] */

#ifdef USE_ALLOCP
  bool *plus_allocp_allocated;
  bool *plus_allocp;		/* points to above[index1interval-1] */
  bool *minus_allocp_allocated;
  bool *minus_allocp;		/* points to above[index1interval-1] */
#endif

#ifdef USE_VALIDP
  bool *validp;
#endif
  bool *omitted;

  Storedoligomer_T *forward_oligos_allocated;
  Storedoligomer_T *forward_oligos; /* points to above[index1interval-1] */
  Storedoligomer_T *revcomp_oligos_allocated;
  Storedoligomer_T *revcomp_oligos; /* points to above[index1interval-1] */

  struct Segment_T *plus_segments;
  struct Segment_T *minus_segments;
  int plus_nsegments;
  int minus_nsegments;

  Segment_T *plus_spliceable; /* plus_segments with a following diagonal within shortsplicedist or splicedists[j] */
  Segment_T *minus_spliceable; /* minus_segments with a following diagonal within shortsplicedist or splicedists[j] */
  int plus_nspliceable;
  int minus_nspliceable;

  bool all_positions_fetched_p;
};


static void
stage3list_gc (List_T *old) {
  List_T p;
  Stage3end_T hit;

  for (p = *old; p != NULL; p = p->rest) {
    hit = (Stage3end_T) p->first;
    Stage3end_free(&hit);
  }
  List_free(&(*old));
  return;
}

static void
substringlist_gc (List_T *old) {
  List_T p;
  Substring_T hit;

  for (p = *old; p != NULL; p = p->rest) {
    hit = (Substring_T) p->first;
    Substring_free(&hit);
  }
  List_free(&(*old));
  return;
}


static bool free_positions_p;

void
Stage1_init_positions_free (bool positions_fileio_p) {
  if (positions_fileio_p == true) {
    free_positions_p = true;
  } else {
    free_positions_p = false;
  }
  return;
}


void
Stage1_free (T *old, int querylength) {
  List_T p;
  Spanningelt_T spanningelt;
  int mod, i;

  /* Stage1hr_check(*old); */

  if (*old) {
    FREE((*old)->plus_spliceable);
    FREE((*old)->minus_spliceable);

    FREE((*old)->plus_segments);
    FREE((*old)->minus_segments);

    for (mod = 0; mod < index1interval; mod++) {
      for (p = (*old)->plus_spanningset[mod]; p; p = p->rest) {
	spanningelt = (Spanningelt_T) p->first;
	Spanningelt_free(&spanningelt);
      }
      List_free(&((*old)->plus_spanningset[mod]));

      for (p = (*old)->minus_spanningset[mod]; p; p = p->rest) {
	spanningelt = (Spanningelt_T) p->first;
	Spanningelt_free(&spanningelt);
      }
      List_free(&((*old)->minus_spanningset[mod]));
    }

    if (free_positions_p == true) {
      for (i = -index1interval+1; i < querylength; i++) {
	if ((*old)->plus_retrievedp[i] == true) {
	  FREE((*old)->plus_positions[i]);
	}
	if ((*old)->minus_retrievedp[i] == true) {
	  FREE((*old)->minus_positions[i]);
	}
      }
#ifdef USE_ALLOCP
    } else {
      for (i = -index1interval+1; i < querylength; i++) {
	if ((*old)->plus_allocp[i] == true) {
	  FREE((*old)->plus_positions[i]);
	}
	if ((*old)->minus_allocp[i] == true) {
	  FREE((*old)->minus_positions[i]);
	}
      }
#endif
    }

    FREE((*old)->revcomp_oligos_allocated);
    FREE((*old)->forward_oligos_allocated);
    FREE((*old)->omitted);
#ifdef USE_VALIDP
    FREE((*old)->validp);
#endif
    FREE((*old)->plus_positions_allocated);
    FREE((*old)->minus_positions_allocated);
    FREE((*old)->plus_npositions_allocated);
    FREE((*old)->minus_npositions_allocated);
#ifdef USE_ALLOCP
    FREE((*old)->plus_allocp_allocated);
    FREE((*old)->minus_allocp_allocated);
#endif
    FREE((*old)->plus_retrievedp_allocated);
    FREE((*old)->minus_retrievedp_allocated);

    FREE(*old);
  }

  return;
}


/************************************************************************/

static bool
check_dinucleotides (char *sequence, int querylength) {
  int index, firsti;
  int n = 0, i;
  int c1, c2;
  bool validp;
  int dinucl_counts[16], first_count, second_count;

  for (index = 0; index < 16; index++) {
    dinucl_counts[index] = 0;
  }

  for (i = 0; i < querylength - 1; i++) {
    c1 = sequence[i];
    c2 = sequence[i+1];
    validp = true;

    switch (c1) {
    case 'A': index = 0; break;
    case 'C': index = 4; break;
    case 'G': index = 8; break;
    case 'T': index = 12; break;
    default: validp = false;
    }

    switch (c2) {
    case 'A': break;
    case 'C': index += 1; break;
    case 'G': index += 2; break;
    case 'T': index += 3; break;
    default: validp = false;
    }
      
    if (validp == true) {
      dinucl_counts[index] += 1;
      n++;
    }
  }

  if (n == 0) {
    return false;
  } else {
    first_count = 0;
    for (index = 0; index < 16; index++) {
      debug(printf("%d: %d\n",index,dinucl_counts[index]));
      if (dinucl_counts[index] > first_count) {
	first_count = dinucl_counts[index];
	firsti = index;
      }
    }

    second_count = 0;
    for (index = 0; index < 16; index++) {
      if (index == firsti) {
	/* Skip */
      } else if (dinucl_counts[index] > second_count) {
	second_count = dinucl_counts[index];
      }
    }

    debug(printf("first count: %d, second count: %d",first_count,second_count));
    if (first_count + second_count > 0.80 * (double) n) {
      debug(printf(" > 0.80*%d = %.2f\n",n,0.80*n));
      return false;
    } else {
      debug(printf("\n"));
      return true;
    }
  }
}



static int
read_oligos (bool *allvalidp, T this, char *queryuc_ptr, int querylength,
	     int query_lastpos, int genestrand) {
  Reader_T reader;
  int querypos, noligos = 0;
  Oligostate_T last_state = INIT;
  Storedoligomer_T forward = 0U, revcomp = 0U;

  /* This estimate may be too high */
  /* this->maxfloor = 1 + querylength/oligobase * 2; */

  reader = Reader_new(queryuc_ptr,/*querystart*/0,/*queryend*/querylength);

  /* Prevents us from processing invalid query 12-mers */
  for (querypos = 0; querypos <= query_lastpos; querypos++) {
    this->plus_retrievedp[querypos] = true;
    this->plus_positions[querypos] = (Univcoord_T *) NULL;
    this->plus_npositions[querypos] = 0;

    this->minus_retrievedp[querypos] = true;
    this->minus_positions[querypos] = (Univcoord_T *) NULL;
    this->minus_npositions[querypos] = 0;
  }

  /* Note: leftshifting is done here, rather than in Oligo_lookup */
  debug(printf("oligobase_mask: %08X\n",oligobase_mask));
#if 0
  *any_omitted_p = false;
  *all_omitted_p = true;
#endif
  if (mode == STANDARD) {
    while ((last_state = Oligo_next(last_state,&querypos,&forward,&revcomp,
				    reader,/*cdnaend*/FIVE)) != DONE) {
      this->plus_positions[querypos] = (Univcoord_T *) NULL;
      this->minus_positions[querypos] = (Univcoord_T *) NULL;
      this->plus_npositions[querypos] = 0;
      this->minus_npositions[querypos] = 0;

      if (last_state == VALID) {
#ifdef USE_VALIDP
	this->validp[querypos] = true;
#endif
	this->plus_retrievedp[querypos] = false;
	this->minus_retrievedp[querypos] = false;

	this->forward_oligos[querypos] = forward & oligobase_mask;
	this->revcomp_oligos[querypos] = (revcomp >> leftreadshift) & oligobase_mask;

	debug(printf("At querypos %d, read oligo = %06X\n",querypos,this->forward_oligos[querypos]));
	noligos++;
      }
    }

  } else if (mode == CMET_STRANDED) {
    while ((last_state = Oligo_next(last_state,&querypos,&forward,&revcomp,
				    reader,/*cdnaend*/FIVE)) != DONE) {
      this->plus_positions[querypos] = (Univcoord_T *) NULL;
      this->minus_positions[querypos] = (Univcoord_T *) NULL;
      this->plus_npositions[querypos] = 0;
      this->minus_npositions[querypos] = 0;

      if (last_state == VALID) {
#ifdef USE_VALIDP
	this->validp[querypos] = true;
#endif
	this->plus_retrievedp[querypos] = false;
	this->minus_retrievedp[querypos] = false;

	this->forward_oligos[querypos] = Cmet_reduce_ct(forward) & oligobase_mask;
	this->revcomp_oligos[querypos] = Cmet_reduce_ga(revcomp >> leftreadshift) & oligobase_mask;

	debug(printf("At querypos %d, read oligo = %06X\n",querypos,this->forward_oligos[querypos]));
	noligos++;
      }
    }

  } else if (mode == CMET_NONSTRANDED) {
    if (genestrand == +1) {
      while ((last_state = Oligo_next(last_state,&querypos,&forward,&revcomp,
				      reader,/*cdnaend*/FIVE)) != DONE) {
	this->plus_positions[querypos] = (Univcoord_T *) NULL;
	this->minus_positions[querypos] = (Univcoord_T *) NULL;
	this->plus_npositions[querypos] = 0;
	this->minus_npositions[querypos] = 0;

	if (last_state == VALID) {
#ifdef USE_VALIDP
	  this->validp[querypos] = true;
#endif
	  this->plus_retrievedp[querypos] = false;
	  this->minus_retrievedp[querypos] = false;

	  this->forward_oligos[querypos] = Cmet_reduce_ct(forward) & oligobase_mask;
	  this->revcomp_oligos[querypos] = Cmet_reduce_ct(revcomp >> leftreadshift) & oligobase_mask;

	  debug(printf("At querypos %d, read oligo = %06X\n",querypos,this->forward_oligos[querypos]));
	  noligos++;
	}
      }
    } else {
      while ((last_state = Oligo_next(last_state,&querypos,&forward,&revcomp,
				      reader,/*cdnaend*/FIVE)) != DONE) {
	this->plus_positions[querypos] = (Univcoord_T *) NULL;
	this->minus_positions[querypos] = (Univcoord_T *) NULL;
	this->plus_npositions[querypos] = 0;
	this->minus_npositions[querypos] = 0;

	if (last_state == VALID) {
#ifdef USE_VALIDP
	  this->validp[querypos] = true;
#endif
	  this->plus_retrievedp[querypos] = false;
	  this->minus_retrievedp[querypos] = false;

	  this->forward_oligos[querypos] = Cmet_reduce_ga(forward) & oligobase_mask;
	  this->revcomp_oligos[querypos] = Cmet_reduce_ga(revcomp >> leftreadshift) & oligobase_mask;

	  debug(printf("At querypos %d, read oligo = %06X\n",querypos,this->forward_oligos[querypos]));
	  noligos++;
	}
      }
    }

  } else if (mode == ATOI_STRANDED) {
    while ((last_state = Oligo_next(last_state,&querypos,&forward,&revcomp,
				    reader,/*cdnaend*/FIVE)) != DONE) {
      this->plus_positions[querypos] = (Univcoord_T *) NULL;
      this->minus_positions[querypos] = (Univcoord_T *) NULL;
      this->plus_npositions[querypos] = 0;
      this->minus_npositions[querypos] = 0;

      if (last_state == VALID) {
#ifdef USE_VALIDP
	this->validp[querypos] = true;
#endif
	this->plus_retrievedp[querypos] = false;
	this->minus_retrievedp[querypos] = false;

	this->forward_oligos[querypos] = Atoi_reduce_ag(forward) & oligobase_mask;
	this->revcomp_oligos[querypos] = Atoi_reduce_tc(revcomp >> leftreadshift) & oligobase_mask;

	debug(printf("At querypos %d, read oligo = %06X\n",querypos,this->forward_oligos[querypos]));
	noligos++;
      }
    }

  } else if (mode == ATOI_NONSTRANDED) {
    if (genestrand == +1) {
      while ((last_state = Oligo_next(last_state,&querypos,&forward,&revcomp,
				      reader,/*cdnaend*/FIVE)) != DONE) {
	this->plus_positions[querypos] = (Univcoord_T *) NULL;
	this->minus_positions[querypos] = (Univcoord_T *) NULL;
	this->plus_npositions[querypos] = 0;
	this->minus_npositions[querypos] = 0;

	if (last_state == VALID) {
#ifdef USE_VALIDP
	  this->validp[querypos] = true;
#endif
	  this->plus_retrievedp[querypos] = false;
	  this->minus_retrievedp[querypos] = false;
	  
	  this->forward_oligos[querypos] = Atoi_reduce_ag(forward) & oligobase_mask;
	  this->revcomp_oligos[querypos] = Atoi_reduce_ag(revcomp >> leftreadshift) & oligobase_mask;

	  debug(printf("At querypos %d, read oligo = %06X\n",querypos,this->forward_oligos[querypos]));
	  noligos++;
	}
      }
    } else {
      while ((last_state = Oligo_next(last_state,&querypos,&forward,&revcomp,
				      reader,/*cdnaend*/FIVE)) != DONE) {
	this->plus_positions[querypos] = (Univcoord_T *) NULL;
	this->minus_positions[querypos] = (Univcoord_T *) NULL;
	this->plus_npositions[querypos] = 0;
	this->minus_npositions[querypos] = 0;

	if (last_state == VALID) {
#ifdef USE_VALIDP
	  this->validp[querypos] = true;
#endif
	  this->plus_retrievedp[querypos] = false;
	  this->minus_retrievedp[querypos] = false;
	  
	  this->forward_oligos[querypos] = Atoi_reduce_tc(forward) & oligobase_mask;
	  this->revcomp_oligos[querypos] = Atoi_reduce_tc(revcomp >> leftreadshift) & oligobase_mask;

	  debug(printf("At querypos %d, read oligo = %06X\n",querypos,this->forward_oligos[querypos]));
	  noligos++;
	}
      }
    }
  }

  if (noligos < query_lastpos + 1) {
    debug(printf("Read only %d oligos due to non-ACGT; expected %d\n",noligos,query_lastpos + 1));
    *allvalidp = false;
  } else {
    *allvalidp = true;
  }

  Reader_free(&reader);

  return noligos;
}




/************************************************************************
 *   Omitted:
 *   In all cases, want to omit poly-AT.
 *   For purposes of finding mismatches, may want to omit frequent oligomers also
 *   For purposes of finding indels, may want to omit repetitive oligomers at ends also
 ************************************************************************/

static void
omit_oligos_clear (T this, int query_lastpos) {
  int querypos;

  for (querypos = 0; querypos <= query_lastpos; querypos++) {
    this->omitted[querypos] = false;
  }
  return;
}


#if 0
static void
omit_oligos_polyat (bool *all_omitted_p, bool *any_omitted_p, T this, int query_lastpos) {
  int querypos;

  *all_omitted_p = true;
  *any_omitted_p = false;
  for (querypos = 0; querypos <= query_lastpos; querypos++) {
    if (this->forward_oligos[querypos] == 0U || this->revcomp_oligos[querypos] == 0U) {
      this->omitted[querypos] = true;
      *any_omitted_p = true;
    } else {
      this->omitted[querypos] = false;
      *all_omitted_p = false;
    }
  }

  return;
}
#endif


static void
omit_oligos (bool *all_omitted_p, bool *any_omitted_p, T this, int query_lastpos,
	     int indexdb_size_threshold, bool frequentp, bool repetitivep) {
  int querypos;
  bool still_repetitive_p;

  *any_omitted_p = false;

  /* Always omit poly-AT */
  for (querypos = 0; querypos <= query_lastpos; querypos++) {
    if (this->forward_oligos[querypos] == 0U || this->revcomp_oligos[querypos] == 0U) {
      debug(printf("Querypos %d is poly-A or poly-T\n",querypos));
      this->omitted[querypos] = true;
      *any_omitted_p = true;
    } else {
      this->omitted[querypos] = false;
    }
  }

  if (frequentp == true) {
    /* Omit frequent oligos, but only in the middle */
    for (querypos = index1interval; querypos <= query_lastpos-index1interval; querypos++) {
      if (this->plus_npositions[querypos] > indexdb_size_threshold &&
	  this->minus_npositions[querypos] > indexdb_size_threshold) {
	debug(printf("Querypos %d is frequent with %d plus positions > %d and %d minus positions > %d\n",
		     querypos,this->plus_npositions[querypos],indexdb_size_threshold,
		     this->minus_npositions[querypos],indexdb_size_threshold));
	this->omitted[querypos] = true;
	*any_omitted_p = true;
      }
    }

#if 0
    /* This avoids too many consecutive omitted, but slows down finding double splicing significantly */
    if (*any_omitted_p == true) {
      nconsecutive = 0;
      for (querypos = index1interval; querypos <= query_lastpos-index1interval; querypos += index1interval) {
	if (this->omitted[querypos] == false) {
	  nconsecutive = 0;
	} else {
	  nconsecutive++;
	  if (nconsecutive == 4) { /* corresponds to 4*3 = 12 positions */
	    debug(printf("Consecutive frequent from %d to %d.  ",querypos-11,querypos));
	    this->omitted[querypos] = false;
	    nconsecutive = 0;
	  }
	}
      }

      nconsecutive = 0;
      for (querypos = index1interval+1; querypos <= query_lastpos-index1interval; querypos += index1interval) {
	if (this->omitted[querypos] == false) {
	  nconsecutive = 0;
	} else {
	  nconsecutive++;
	  if (nconsecutive == 4) { /* corresponds to 4*3 = 12 positions */
	    debug(printf("Consecutive frequent from %d to %d.  ",querypos-11,querypos));
	    this->omitted[querypos] = false;
	    nconsecutive = 0;
	  }
	}
      }

      nconsecutive = 0;
      for (querypos = index1interval+index1interval-1; querypos <= query_lastpos-index1interval; querypos += index1interval) {
	if (this->omitted[querypos] == false) {
	  nconsecutive = 0;
	} else {
	  nconsecutive++;
	  if (nconsecutive == 4) { /* corresponds to 4*3 = 12 positions */
	    debug(printf("Consecutive frequent from %d to %d.  ",querypos-11,querypos));
	    this->omitted[querypos] = false;
	    nconsecutive = 0;
	  }
	}
      }
    }
#endif
  }

  if (repetitivep == true) {
    /* Omit repetitive oligos at the ends */
    still_repetitive_p = true;
    querypos = 0;
    while (querypos <= query_lastpos && still_repetitive_p == true) {
      if (Oligo_repetitive_p(this->forward_oligos[querypos])) {
	debug(printf("Querypos %d is repetitive\n",querypos));
	this->omitted[querypos] = true;
	*any_omitted_p = true;
      } else {
	still_repetitive_p = false;
      }
      querypos++;
    }

    still_repetitive_p = true;
    querypos = query_lastpos;
    while (querypos >= 0 && still_repetitive_p == true) {
      if (Oligo_repetitive_p(this->forward_oligos[querypos])) {
	debug(printf("Querypos %d is repetitive\n",querypos));
	this->omitted[querypos] = true;
	*any_omitted_p = true;
      } else {
	still_repetitive_p = false;
      }
      querypos--;
    }
  }

  if (*any_omitted_p == false) {
    debug(printf("No oligos are omitted\n"));
    *all_omitted_p = false;
  } else {
    debug(
	  printf("Omitted oligos:");
	  for (querypos = 0; querypos <= query_lastpos; querypos++) {
	    if (this->omitted[querypos] == true) {
	      printf(" %d",querypos);
	    }
	  }
	  printf("\n"));

    *all_omitted_p = true;
    for (querypos = 0; querypos <= query_lastpos; querypos++) {
      if (this->omitted[querypos] == false) {
	*all_omitted_p = false;
      }
    }
  }

  return;
}


#if 0
static void
omit_oligos_repetitive (bool *all_omitted_p, bool *any_omitted_p, T this, int query_lastpos) {
  int querypos;

  *all_omitted_p = true;
  *any_omitted_p = false;
  for (querypos = 0; querypos <= query_lastpos; querypos++) {
    if (Oligo_repetitive_p(this->forward_oligos[querypos])) {
      this->omitted[querypos] = true;
      *any_omitted_p = true;
    } else {
      this->omitted[querypos] = false;
      *all_omitted_p = false;
    }
  }

  return;
}
#endif


static T
Stage1_new (int querylength) {
  T new = (T) MALLOC(sizeof(*new));
  int mod;
  int overhang = index1interval-1;

  for (mod = 0; mod < index1interval; mod++) {
    new->plus_spanningset[mod] = (List_T) NULL;
    new->minus_spanningset[mod] = (List_T) NULL;
  }

  new->plus_positions_allocated = (Univcoord_T **) MALLOC((querylength+overhang) * sizeof(Univcoord_T *));
  new->plus_positions = &(new->plus_positions_allocated[overhang]);
  new->minus_positions_allocated = (Univcoord_T **) MALLOC((querylength+overhang) *sizeof(Univcoord_T *));
  new->minus_positions = &(new->minus_positions_allocated[overhang]);

  new->plus_npositions_allocated = (int *) MALLOC((querylength+overhang) * sizeof(int));
  new->plus_npositions = &(new->plus_npositions_allocated[overhang]);
  new->minus_npositions_allocated = (int *) MALLOC((querylength+overhang) * sizeof(int));
  new->minus_npositions = &(new->minus_npositions_allocated[overhang]);

#if 0
  /* No need to initialize, since we assign all values below */
  for (querypos = -index1interval+1; querypos < querylength; querypos++) {
    new->plus_positions[querypos] = (Univcoord_T *) NULL;
    new->plus_npositions[querypos] = 0;
    new->minus_positions[querypos] = (Univcoord_T *) NULL;
    new->minus_npositions[querypos] = 0;
  }
#endif

  /* Can be MALLOC, since we initialize in read_oligos() */
  new->plus_retrievedp_allocated = (bool *) MALLOC((querylength+overhang) * sizeof(bool));
  new->minus_retrievedp_allocated = (bool *) MALLOC((querylength+overhang) * sizeof(bool));
  new->plus_retrievedp = &(new->plus_retrievedp_allocated[overhang]);
  new->minus_retrievedp = &(new->minus_retrievedp_allocated[overhang]);

#ifdef USE_ALLOCP
  /* Never set to true, so never used */
  new->plus_allocp_allocated = (bool *) CALLOC(querylength+overhang,sizeof(bool));
  new->minus_allocp_allocated = (bool *) CALLOC(querylength+overhang,sizeof(bool));
  new->plus_allocp = &(new->plus_allocp_allocated[overhang]);
  new->minus_allocp = &(new->minus_allocp_allocated[overhang]);
#endif

#ifdef USE_VALIDP
  new->validp = (bool *) CALLOC(querylength,sizeof(bool));
#endif
  new->omitted = (bool *) CALLOC(querylength,sizeof(bool));

  new->forward_oligos_allocated = (Storedoligomer_T *) CALLOC(querylength+overhang,sizeof(Storedoligomer_T));
  new->forward_oligos = &(new->forward_oligos_allocated[overhang]);
  new->revcomp_oligos_allocated = (Storedoligomer_T *) CALLOC(querylength+overhang,sizeof(Storedoligomer_T));
  new->revcomp_oligos = &(new->revcomp_oligos_allocated[overhang]);

  new->plus_segments = (struct Segment_T *) NULL;
  new->minus_segments = (struct Segment_T *) NULL;
  new->plus_nsegments = 0;
  new->minus_nsegments = 0;

  new->plus_spliceable = (Segment_T *) NULL;
  new->minus_spliceable = (Segment_T *) NULL;
  new->plus_nspliceable = 0;
  new->minus_nspliceable = 0;

  new->all_positions_fetched_p = false;

  return new;
}


/************************************************************************/

static char complCode[128] = COMPLEMENT_LC;

static void
make_complement_buffered (char *complement, char *sequence, unsigned int length) {
  int i, j;

  /* complement = (char *) CALLOC(length+1,sizeof(char)); */
  for (i = length-1, j = 0; i >= 0; i--, j++) {
    complement[j] = complCode[(int) sequence[i]];
  }
  complement[length] = '\0';
  return;
}


#if 0
static void
make_complement_inplace (char *sequence, unsigned int length) {
  char temp;
  unsigned int i, j;

  for (i = 0, j = length-1; i < length/2; i++, j--) {
    temp = complCode[(int) sequence[i]];
    sequence[i] = complCode[(int) sequence[j]];
    sequence[j] = temp;
  }
  if (i == j) {
    sequence[i] = complCode[(int) sequence[i]];
  }

  return;
}
#endif

/************************************************************************/

#define PARENT(i) (i >> 1)
#define LEFT(i) (i << 1)
#define RIGHT(i) ((i << 1) | 1)


typedef struct Batch_T *Batch_T;

struct Batch_T {
  int querypos;
  int diagterm;
  int npositions;

  Univcoord_T diagonal;
  Univcoord_T *positions;
#ifdef DIAGONAL_ADD_QUERYPOS
  UINT8 diagonal_add_querypos;
#endif
};


static void
Batch_init (Batch_T batch, int querypos, int diagterm, Univcoord_T *positions, int npositions, int querylength) {

  batch->querypos = querypos;
  batch->diagterm = diagterm;
  batch->positions = positions;
#ifdef WORDS_BIGENDIAN
  batch->diagonal = Bigendian_convert_univcoord(*positions) + diagterm;
#else
  batch->diagonal = *positions + diagterm;
#endif
  batch->npositions = npositions;

#ifdef NO_EXTENSIONS_BEFORE_ZERO
  /* This prevents us from finding insertions at the beginning of the genome */
  while (batch->npositions > 0 && batch->diagonal < (unsigned int) querylength) {
    debug11(printf("Eliminating diagonal %lu as straddling beginning of genome (Batch_init)\n",batch->diagonal));
    batch->npositions--;
    if (batch->npositions > 0) {
#ifdef WORDS_BIGENDIAN
      batch->diagonal = Bigendian_convert_univcoord(*(++batch->positions)) + diagterm;
#else
      batch->diagonal = *(++batch->positions) + diagterm;
#endif
    }
  }
#endif


#ifdef DIAGONAL_ADD_QUERYPOS
  batch->diagonal_add_querypos = (UINT8) batch->diagonal;
  batch->diagonal_add_querypos <<= 32;
  batch->diagonal_add_querypos |= querypos /* Previously added 2 because querypos was -2: + 2*/;
#endif

  return;
}


static void
Batch_init_simple (Batch_T batch, Univcoord_T *diagonals, int ndiagonals, int querylength, int querypos) {

  batch->querypos = querypos;
  batch->positions = diagonals;
  batch->diagonal = *diagonals;	/* Already in correct endianness */
  batch->npositions = ndiagonals;

  while (batch->npositions > 0 && batch->diagonal < (unsigned int) querylength) {
    debug11(printf("Eliminating diagonal %lu as straddling beginning of genome (Batch_init)\n",batch->diagonal));
    batch->npositions--;
    if (batch->npositions > 0) {
      /* positions are really diagonals, already in correct endianness */
      batch->diagonal = *(++batch->positions);
    }
  }

  return;
}


static void
min_heap_insert (Batch_T *heap, int *heapsize, Batch_T batch) {
  int i;
#ifdef DIAGONAL_ADD_QUERYPOS
  UINT8 diagonal_add_querypos;
#else
  int querypos;
  Univcoord_T diagonal;
#endif

  i = ++(*heapsize);
#ifdef DIAGONAL_ADD_QUERYPOS
  diagonal_add_querypos = batch->diagonal_add_querypos;
  while (i > 1 && (heap[PARENT(i)]->diagonal_add_querypos > diagonal_add_querypos)) {
    heap[i] = heap[PARENT(i)];
    i = PARENT(i);
  }
#else
  querypos = batch->querypos;
  diagonal = batch->diagonal;
  /* sort primarily by diagonal, then by querypos */
  while (i > 1 && (heap[PARENT(i)]->diagonal > diagonal ||
		   (heap[PARENT(i)]->diagonal == diagonal && heap[PARENT(i)]->querypos > querypos))) {
    heap[i] = heap[PARENT(i)];
    i = PARENT(i);
  }
#endif
  heap[i] = batch;

  return;
}


static void
min_heap_insert_simple (Batch_T *heap, int *heapsize, Batch_T batch) {
  int i;
  Univcoord_T diagonal;

  i = ++(*heapsize);
  diagonal = batch->diagonal;
  while (i > 1 && (heap[PARENT(i)]->diagonal > diagonal)) {
    heap[i] = heap[PARENT(i)];
    i = PARENT(i);
  }
  heap[i] = batch;

  return;
}



/* Note FORMULA: formulas for querypos <-> diagonal (diagterm in call to Indexdb_read) are:

plus: diagonal = position + querylength - querypos
minus: diagonal = position + querypos + index1part

For minus, the index1part is needed in call to Indexdb_read because
position is stored at beginning of plus oligomer, which corresponds to
end of minus oligomer.  As a result, we have the following formulas:

high genomic position = diagonal (corresponds to querypos =
querylength for plus, and querypos = 0 for minus)

low genomic position = diagonal - querylength (corresponds to querypos
= 0 for plus, and querypos = querylength for minus)

*/


static List_T
report_perfect_segment (int *found_score, int *nhits, List_T hits, Univcoord_T left,
			Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh,
			Chrpos_T chrlength, int querylength, Compress_T query_compress,
			int nmisses_allowed, bool plusp, int genestrand) {
  Stage3end_T hit;
  int nmismatches;

  if (snpp == true) {
    if ((hit = Stage3end_new_substitution(&(*found_score),/*nmismatches*/0,
					  left,/*genomiclength*/querylength,query_compress,
					  plusp,genestrand,chrnum,chroffset,chrhigh,chrlength,
					  /*sarrayp*/false)) == NULL) {
      return hits;
    } else {
      *nhits += 1;
      return List_push(hits,(void *) hit);
    }

  } else if (mode != STANDARD || spansize != index1part) {
    /* Count actual number of mismatches.  May not be a perfect segment. */
    nmismatches = Genome_count_mismatches_limit(query_compress,left,/*pos5*/0,/*pos3*/querylength,
						/*max_mismatches_allowed*/nmisses_allowed,plusp,
						genestrand);
    if (nmismatches > nmisses_allowed) {
      return hits;
    } else {
      /* Don't use Stage3end_new_exact, because need to mark mismatches */
      if ((hit = Stage3end_new_substitution(&(*found_score),nmismatches,
					    left,/*genomiclength*/querylength,
					    query_compress,plusp,genestrand,
					    chrnum,chroffset,chrhigh,chrlength,
					    /*sarrayp*/false)) == NULL) {
	return hits;
      } else {
	*nhits += 1;
	return List_push(hits,(void *) hit);
      }
    }

  } else {
    /* mode == STANDARD && spansize == index1part */
    if ((hit = Stage3end_new_exact(&(*found_score),left,/*genomiclength*/querylength,
				   query_compress,plusp,genestrand,chrnum,chroffset,chrhigh,chrlength,
				   /*sarrayp*/false)) == NULL) {
      return hits;
    } else {
      *nhits += 1;
      return List_push(hits,(void *) hit);
    }
  }
}


#if 0
static List_T
report_perfect_segment_dibase (int *found_score, int *nhits, List_T hits, Univcoord_T left, Univcoord_T diagonal,
			       Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh, Chrpos_T chrlength,
			       char *queryptr, int querylength, Compress_T query_compress,
			       int nmisses_allowed, bool plusp) {
  Stage3end_T hit;

#if 0
  int ncolordiffs;
  Dibase_count_mismatches_substring(&ncolordiffs,query,pos5,pos3,blocks,
				    /*startpos*/left+pos5,/*endpos*/left+pos3);
#endif

  /* Need to fill buffer with nucleotide genome anyway */
  if ((hit = Stage3end_new_substitution(&(*found_score),/*nmismatches*/0,
					left,/*genomiclength*/querylength,
					query_compress,plusp,genestrand,
					chrnum,chroffset,chrhigh,chrlength,
					/*sarrayp*/false)) == NULL) {
    return hits;
  } else {
    *nhits += 1;
    return List_push(hits,(void *) hit);
  }
}
#endif


/* Called only by exact/sub:1 procedures, so need to do Bigendian conversion */
#ifdef WORDS_BIGENDIAN
static int
binary_search_bigendian (int lowi, int highi, Univcoord_T *positions, Univcoord_T goal) {
  int middlei;

  debug10(printf("entered binary search with lowi=%d, highi=%d, goal=%lu\n",lowi,highi,goal));

  while (lowi < highi) {
    middlei = (lowi+highi)/2;
    debug10(printf("  binary: %d:%lu %d:%lu %d:%lu   vs. %lu\n",
		   lowi,Bigendian_convert_univcoord(positions[lowi]),middlei,Bigendian_convert_univcoord(positions[middlei]),
		   highi,Bigendian_convert_univcoord(positions[highi]),goal));
    if (goal < Bigendian_convert_univcoord(positions[middlei])) {
      highi = middlei;
    } else if (goal > Bigendian_convert_univcoord(positions[middlei])) {
      lowi = middlei + 1;
    } else {
      debug10(printf("binary search returns %d\n",middlei));
      return middlei;
    }
  }

  debug10(printf("binary search returns %d\n",highi));
  return highi;
}
#endif


static int
binary_search (int lowi, int highi, Univcoord_T *positions, Univcoord_T goal) {
  int middlei;

  debug10(printf("entered binary search with lowi=%d, highi=%d, goal=%lu\n",lowi,highi,goal));

  while (lowi < highi) {
    middlei = (lowi+highi)/2;
    debug10(printf("  binary: %d:%lu %d:%lu %d:%lu   vs. %lu\n",
		   lowi,positions[lowi],middlei,positions[middlei],
		   highi,positions[highi],goal));
    if (goal < positions[middlei]) {
      highi = middlei;
    } else if (goal > positions[middlei]) {
      lowi = middlei + 1;
    } else {
      debug10(printf("binary search returns %d\n",middlei));
      return middlei;
    }
  }

  debug10(printf("binary search returns %d\n",highi));
  return highi;
}


static int
binary_search_segments (int lowi, int highi, struct Segment_T *segments, Univcoord_T goal) {
  int middlei, middlei_up, middlei_down;

  debug10(printf("entered binary search with lowi=%d, highi=%d, goal=%lu\n",lowi,highi,goal));

  while (lowi < highi) {
    middlei = (lowi+highi)/2;
    if (segments[middlei].diagonal == (Univcoord_T) -1) {
      middlei_up = middlei + 1;
      middlei_down = middlei - 1;
    } else {
      middlei_up = middlei_down = middlei;
    }
    debug10(printf("  binary: %d:%lu %d:%lu %d:%lu   vs. %lu\n",
		   lowi,segments[lowi].diagonal,middlei,segments[middlei].diagonal,
		   highi,segments[highi].diagonal,goal));
    if (goal < segments[middlei_down].diagonal) {
      highi = middlei_down;
    } else if (goal > segments[middlei_up].diagonal) {
      lowi = middlei_up + 1;
    } else {
      debug10(printf("binary search returns %d\n",middlei));
      return middlei;
    }
  }

  debug10(printf("binary search returns %d\n",highi));
  return highi;
}



/* Generalization of identify_exact_iter and identify_onemiss_iter */
static List_T
identify_multimiss_iter (int *found_score, Chrnum_T *chrnum, Univcoord_T *chroffset, Univcoord_T *chrhigh,
			 Chrpos_T *chrlength, int *nhits, List_T hits, Univcoord_T goal, List_T prev, int *nempty,
			 int *global_miss_querypos5, int *global_miss_querypos3,
			 int querylength, Compress_T query_compress, bool plusp, int genestrand,
			 int nmisses_allowed, int nmisses_seen, int miss_querypos5, int miss_querypos3) {
  List_T spanningset;
  Stage3end_T hit;
  void *ignore;
  Spanningelt_T elt;
  Compoundpos_T compoundpos;
  Univcoord_T local_goal, left;
  Univcoord_T position;
  int nmismatches, j;


  debug7(printf("identify_multimiss_iter on diagonal %lu with %d misses seen initially\n",
		goal,nmisses_seen));

  if (nmisses_seen > nmisses_allowed) {
    debug7(printf("Result: skipping because %d misses seen > %d allowed\n",nmisses_seen,nmisses_allowed));
    return hits;
  }

  for (spanningset = prev->rest; spanningset /* != NULL */; prev = spanningset, spanningset = spanningset->rest) {
    elt = (Spanningelt_T) spanningset->first;
    debug7(printf("nmisses seen %d, allowed %d, remaining %d, goal %lu: ",
		  nmisses_seen,nmisses_allowed,List_length(prev->rest),goal));

    if (elt->intersection_diagonals != NULL) {
      /* Intersection diagonals already computed */
      if (elt->intersection_ndiagonals > 0 && *elt->intersection_diagonals < goal) {
	debug7(printf("  (%d>>",elt->intersection_ndiagonals));
	j = 1;
	while (j < elt->intersection_ndiagonals && elt->intersection_diagonals[j] < goal) {
	  j <<= 1;		/* gallop by 2 */
	}
	if (j >= elt->intersection_ndiagonals) {
	  j = binary_search(j >> 1,elt->intersection_ndiagonals,elt->intersection_diagonals,goal);
	} else {
	  j = binary_search(j >> 1,j,elt->intersection_diagonals,goal);
	}
	elt->intersection_diagonals += j;
	elt->intersection_ndiagonals -= j;
	debug7(printf("  >>%d)",elt->intersection_ndiagonals));
      }

      if (elt->intersection_ndiagonals <= 0) {
	/* List is empty, so modify spanningset and continue with one more miss seen. */
	prev->rest = List_pop(spanningset,&ignore);
	spanningset = prev;
	*nempty += 1;
	if (elt->miss_querypos5 < *global_miss_querypos5) *global_miss_querypos5 = elt->miss_querypos5;
	if (elt->miss_querypos3 > *global_miss_querypos3) *global_miss_querypos3 = elt->miss_querypos3;

	debug7(printf(" intersection empty, counts as one miss --"));
	if (++nmisses_seen > nmisses_allowed) {
	  debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	  return hits;
	} else {
	  debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	  if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	  if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	  /* continue; -- naturally falls to end of loop */
	}
      } else if (*elt->intersection_diagonals > local_goal) {
	/* Already advanced past goal, so continue with one more miss seen. */
	debug7(printf(" one miss --"));
	if (++nmisses_seen > nmisses_allowed) {
	  debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	  return hits;
	} else {
	  debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	  if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	  if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	  /* continue; -- naturally falls to end of loop */
	}
      } else {
	/* Found goal.  Advance past goal and continue with loop. */
	debug7(printf(" advancing\n"));
	++elt->intersection_diagonals;
	--elt->intersection_ndiagonals;
	/* continue; -- naturally falls to end of loop */
      }

    } else {
      if (elt->partnerp == true) {
	/* Partner is guaranteed to be atomic */
	local_goal = goal - elt->partner_diagterm;

#ifdef WORDS_BIGENDIAN
	if (elt->partner_npositions > 0 && Bigendian_convert_univcoord(*elt->partner_positions) < local_goal) {
	  debug7(printf("  (%d>>",elt->partner_npositions));
	  j = 1;
	  while (j < elt->partner_npositions && Bigendian_convert_univcoord(elt->partner_positions[j]) < local_goal) {
	    j <<= 1;		/* gallop by 2 */
	  }
	  if (j >= elt->partner_npositions) {
	    j = binary_search_bigendian(j >> 1,elt->partner_npositions,elt->partner_positions,local_goal);
	  } else {
	    j = binary_search_bigendian(j >> 1,j,elt->partner_positions,local_goal);
	  }
	  elt->partner_positions += j;
	  elt->partner_npositions -= j;
	  debug7(printf("  >>%d)",elt->partner_npositions));
	}
#else
	if (elt->partner_npositions > 0 && *elt->partner_positions < local_goal) {
	  debug7(printf("  (%d>>",elt->partner_npositions));
	  j = 1;
	  while (j < elt->partner_npositions && elt->partner_positions[j] < local_goal) {
	    j <<= 1;		/* gallop by 2 */
	  }
	  if (j >= elt->partner_npositions) {
	    j = binary_search(j >> 1,elt->partner_npositions,elt->partner_positions,local_goal);
	  } else {
	    j = binary_search(j >> 1,j,elt->partner_positions,local_goal);
	  }
	  elt->partner_positions += j;
	  elt->partner_npositions -= j;
	  debug7(printf("  >>%d)",elt->partner_npositions));
	}
#endif

	if (elt->partner_npositions <= 0) {
	  /* Empty, so modify spanningset and continue with one more miss seen. */
	  prev->rest = List_pop(spanningset,&ignore);
	  spanningset = prev;
	  *nempty += 1;
	  if (elt->miss_querypos5 < *global_miss_querypos5) *global_miss_querypos5 = elt->miss_querypos5;
	  if (elt->miss_querypos3 > *global_miss_querypos3) *global_miss_querypos3 = elt->miss_querypos3;

	  debug7(printf(" partner empty --"));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    continue;		/* Don't need to check main list below */
	  }
#ifdef WORDS_BIGENDIAN
	} else if (Bigendian_convert_univcoord(*elt->partner_positions) > local_goal) {
	  /* Advanced past local_goal, so continue with one more miss seen. */
	  debug7(printf(" not in partner --"));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    continue;		/* Don't need to check main list below */
	  }
#else
	} else if (*elt->partner_positions > local_goal) {
	  /* Advanced past local_goal, so continue with one more miss seen. */
	  debug7(printf(" not in partner --"));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    continue;		/* Don't need to check main list below */
	  }
#endif
	} else {
	  /* Found local_goal.  Advance past local_goal and continue with rest of compound querypos */
	  debug7(printf(" found in partner, so continue with rest of compound querypos\n"));
	  ++elt->partner_positions;
	  --elt->partner_npositions;
	  /* Continue below with main list */
	}
      }

      if ((compoundpos = elt->compoundpos) != NULL) {
	local_goal = goal - elt->compoundpos_diagterm;
	if (Compoundpos_search(&position,compoundpos,local_goal) <= 0) {
	  /* Empty, so modify spanningset and continue with one more miss seen. */
	  prev->rest = List_pop(spanningset,&ignore);
	  spanningset = prev;
	  *nempty += 1;
	  if (elt->miss_querypos5 < *global_miss_querypos5) *global_miss_querypos5 = elt->miss_querypos5;
	  if (elt->miss_querypos3 > *global_miss_querypos3) *global_miss_querypos3 = elt->miss_querypos3;

	  debug7(printf("  compoundpos empty --"));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    /* continue; -- Naturally falls to end of loop */
	  }
	} else if (position > local_goal) {
	  /* Advanced past goal.  Continue with one more miss seen. */
	  debug7(printf("  compoundpos failed %lu > %lu --",position,local_goal));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    /* continue; -- Naturally falls to end of loop */
	  }
	} else {
	  /* Found goal.  Advance past goal and continue with loop.  */
	  debug7(printf("  found %lu, advancing...",local_goal));
	  /* continue; -- Naturally falls to end of loop */
	}

      } else {
	/* Ordinary querypos */
	local_goal = goal - elt->diagterm;

#ifdef WORDS_BIGENDIAN
	if (elt->npositions > 0 && Bigendian_convert_univcoord(*elt->positions) < local_goal) {
	  debug7(printf("  (%d>>",elt->npositions));
	  j = 1;
	  while (j < elt->npositions && Bigendian_convert_univcoord(elt->positions[j]) < local_goal) {
	    j <<= 1;		/* gallop by 2 */
	  }
	  if (j >= elt->npositions) {
	    j = binary_search_bigendian(j >> 1,elt->npositions,elt->positions,local_goal);
	  } else {
	    j = binary_search_bigendian(j >> 1,j,elt->positions,local_goal);
	  }
	  elt->positions += j;
	  elt->npositions -= j;
	  debug7(printf("  >>%d)",elt->npositions));
	}
#else
	if (elt->npositions > 0 && *elt->positions < local_goal) {
	  debug7(printf("  (%d>>",elt->npositions));
	  j = 1;
	  while (j < elt->npositions && elt->positions[j] < local_goal) {
	    j <<= 1;		/* gallop by 2 */
	  }
	  if (j >= elt->npositions) {
	    j = binary_search(j >> 1,elt->npositions,elt->positions,local_goal);
	  } else {
	    j = binary_search(j >> 1,j,elt->positions,local_goal);
	  }
	  elt->positions += j;
	  elt->npositions -= j;
	  debug7(printf("  >>%d)",elt->npositions));
	}
#endif

	if (elt->npositions <= 0) {
	  /* List is empty, so continue with one more miss seen. */
	  prev->rest = List_pop(spanningset,&ignore);
	  spanningset = prev;
	  *nempty += 1;
	  if (elt->miss_querypos5 < *global_miss_querypos5) *global_miss_querypos5 = elt->miss_querypos5;
	  if (elt->miss_querypos3 > *global_miss_querypos3) *global_miss_querypos3 = elt->miss_querypos3;

	  debug7(printf(" positions empty, counts as one miss --"));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    /* continue; -- Naturally falls to end of loop */
	  }
#ifdef WORDS_BIGENDIAN
	} else if (Bigendian_convert_univcoord(*elt->positions) > local_goal) {
	  /* Already advanced past goal, so continue with one more miss seen. */
	  debug7(printf(" one miss %lu > %lu --",Bigendian_convert_univcoord(*elt->positions),local_goal));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    /* continue; -- Naturally falls to end of loop */
	  }
#else
	} else if (*elt->positions > local_goal) {
	  /* Already advanced past goal, so continue with one more miss seen. */
	  debug7(printf(" one miss %lu > %lu --",*elt->positions,local_goal));
	  if (++nmisses_seen > nmisses_allowed) {
	    debug7(printf(" nmisses seen %d > allowed %d, so returning\n",nmisses_seen,nmisses_allowed));
	    return hits;
	  } else {
	    debug7(printf("  nmisses seen %d <= allowed %d, so continuing\n",nmisses_seen,nmisses_allowed));
	    if (elt->miss_querypos5 < miss_querypos5) miss_querypos5 = elt->miss_querypos5;
	    if (elt->miss_querypos3 > miss_querypos3) miss_querypos3 = elt->miss_querypos3;
	    /* continue; -- Naturally falls to end of loop */
	  }
#endif
	} else {
	  /* Found goal.  Advance past goal and continue with loop. */
	  debug7(printf(" advancing\n"));
	  ++elt->positions;
	  --elt->npositions;
	  /* continue; -- Naturally falls to end of loop */
	}
      }
    }
    /* End of loop */
  }

  /* success */
  debug7(printf("  successful candidate found, with >= %d misses, %d allowed\n",nmisses_seen,nmisses_allowed));
  if (nmisses_seen == 0) {
    left = goal - querylength;
    if (goal > *chrhigh) {
      *chrnum = Univ_IIT_get_one(chromosome_iit,left,left);
      Univ_IIT_interval_bounds(&(*chroffset),&(*chrhigh),&(*chrlength),chromosome_iit,*chrnum,circular_typeint);
      /* *chrhigh += 1; */
    }
    debug(printf("Reporting perfect segment at left %lu and diagonal %lu, with chroffset %lu and chrhigh %lu\n",
		 left,goal,*chroffset,*chrhigh));
    if (goal > *chrhigh) {
      /* Query goes over end of chromosome */
      debug(printf("  Ignore: goes over end of chromosome\n"));
      return hits;
    } else {
      return report_perfect_segment(&(*found_score),&(*nhits),hits,left,*chrnum,*chroffset,*chrhigh,*chrlength,
				    querylength,query_compress,nmisses_allowed,plusp,genestrand);
    }
  } else {
    if (goal < (unsigned int) querylength) {
      debug7(printf("  Goes over beginning of chromosome\n"));
      return hits;
    } else {
      left = goal - querylength;	/* goal here is diagonal */
    }

    if (goal > *chrhigh) {
      *chrnum = Univ_IIT_get_one(chromosome_iit,left,left);
      Univ_IIT_interval_bounds(&(*chroffset),&(*chrhigh),&(*chrlength),chromosome_iit,*chrnum,circular_typeint);
      /* *chrhigh += 1; */
    }
    if (goal > *chrhigh) {
      debug7(printf("  Goes over end of chromosome\n"));
      return hits;

    } else {
      if (snpp || mode != STANDARD || spansize != index1part) {
	debug7(printf("  Testing in entire query\n"));
	nmismatches = Genome_count_mismatches_substring(query_compress,left,/*pos5*/0,/*pos3*/querylength,
							plusp,genestrand);
      } else {
	debug7(printf("  Testing in query bounds %d..%d\n",miss_querypos5,miss_querypos3));
	nmismatches = Genome_count_mismatches_substring(query_compress,left,/*pos5*/miss_querypos5,/*pos3*/miss_querypos3,
							plusp,genestrand);

      }
      debug7(printf("nmismatches = %d (vs %d misses allowed)\n",nmismatches,nmisses_allowed));

      if (nmismatches > nmisses_allowed) {
	debug7(printf("Result: too many mismatches\n"));
	return hits;
      } else {
	debug7(printf("Result: successful hit saved\n"));
	debug(printf("Reporting hit with %d mismatches\n",nmismatches));
	if ((hit = Stage3end_new_substitution(&(*found_score),nmismatches,
					      left,/*genomiclength*/querylength,
					      query_compress,plusp,genestrand,
					      *chrnum,*chroffset,*chrhigh,*chrlength,
					      /*sarrayp*/false)) == NULL) {
	  return hits;
	} else {
	  *nhits += 1;
	  return List_push(hits,(void *) hit);
	}
      }
    }
  }
}


/* Since querypos -1 and query_lastpos+1 are now
   stored as compoundpos, we no longer want to use them for boosting */
static void
most_specific_oligomer_1 (int *best_plus_querypos, int *best_minus_querypos, T this,
			  int query_lastpos, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev) {
  int querypos;
  int best_plus_count, best_minus_count;

  /* Not needed, since this is the first procedure called */
  /* Block_restore(this->block5); */

  best_plus_querypos[0] = -1;
  best_minus_querypos[0] = -1;

  best_plus_count = 0;
  best_minus_count = 0;

  for (querypos = 0; querypos <= query_lastpos; querypos++) {
#if 0
    if (this->validp[querypos] == true) {
#endif
      this->plus_npositions[querypos] = Indexdb_count_no_subst(indexdb_fwd,this->forward_oligos[querypos]);
      this->minus_npositions[querypos] = Indexdb_count_no_subst(indexdb_rev,this->revcomp_oligos[querypos]);
      debug(printf("Counting at querypos %d, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		   querypos,this->plus_npositions[querypos],this->forward_oligos[querypos],
		   this->minus_npositions[querypos],this->revcomp_oligos[querypos]));

      if (best_plus_querypos[0] < 0 || this->plus_npositions[querypos] < best_plus_count) {
	best_plus_querypos[0] = querypos;
	best_plus_count = this->plus_npositions[querypos];
      }
      if (best_minus_querypos[0] < 0 || this->minus_npositions[querypos] < best_minus_count) {
	best_minus_querypos[0] = querypos;
	best_minus_count = this->minus_npositions[querypos];
      }
#if 0
    }
#endif
  }

  return;
}


/* Since querypos -1 and query_lastpos+1 are now
   stored as compoundpos, we no longer want to use them for boosting */
static void
most_specific_oligomer_2 (int *best_plus_querypos, int *best_minus_querypos, T this,
			  int query_lastpos, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev) {
  int querypos, mod;
  int best_plus_count[2], best_minus_count[2];

  /* Not needed, since this is the first procedure called */
  /* Block_restore(this->block5); */

  best_plus_querypos[0] = -2;
  best_plus_querypos[1] = -2;
  best_minus_querypos[0] = -2;
  best_minus_querypos[1] = -2;

  best_plus_count[0] = best_plus_count[1] = 0;
  best_minus_count[0] = best_minus_count[1] = 0;

#if 0
  if (this->validp[0] == false) {
    debug(printf("Not counting at querypos 0, neg 2 or neg 1 because validp is false at querypos 0\n"));
    this->plus_npositions[-1] = 0;
    this->minus_npositions[-1] = 0;
  } else {
#endif
    this->plus_npositions[-1] = Indexdb_count_left_subst_1(indexdb_fwd,this->forward_oligos[0]);
    this->minus_npositions[-1] = Indexdb_count_right_subst_1(indexdb_rev,this->revcomp_oligos[0]);
    debug(printf("Counting at querypos 0, neg 1, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		 this->plus_npositions[-1],this->forward_oligos[0],this->minus_npositions[-1],this->revcomp_oligos[0]));
    best_plus_count[1] = this->plus_npositions[-1];
    best_minus_count[1] = this->minus_npositions[-1];
#if 0
  }
#endif

  for (querypos = 0; querypos <= query_lastpos; querypos++) {
#if 0
    if (this->validp[querypos] == true) {
#endif
      mod = querypos % 2;
      this->plus_npositions[querypos] = Indexdb_count_no_subst(indexdb_fwd,this->forward_oligos[querypos]);
      this->minus_npositions[querypos] = Indexdb_count_no_subst(indexdb_rev,this->revcomp_oligos[querypos]);
      debug(printf("Counting at querypos %d, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		   querypos,this->plus_npositions[querypos],this->forward_oligos[querypos],
		   this->minus_npositions[querypos],this->revcomp_oligos[querypos]));

      if (best_plus_querypos[mod] < 0 || this->plus_npositions[querypos] < best_plus_count[mod]) {
	best_plus_querypos[mod] = querypos;
	best_plus_count[mod] = this->plus_npositions[querypos];
      }
      if (best_minus_querypos[mod] < 0 || this->minus_npositions[querypos] < best_minus_count[mod]) {
	best_minus_querypos[mod] = querypos;
	best_minus_count[mod] = this->minus_npositions[querypos];
      }
#if 0
    }
#endif
  }

  querypos = query_lastpos;
#if 0
  if (this->validp[querypos] == false) {
    debug(printf("Not counting at querypos %d (pos 1) or %d (pos 2) because validp is false at querypos %d\n",
		 querypos+1,querypos+2,querypos));
    this->plus_npositions[querypos+1] = 0;
    this->minus_npositions[querypos+1] = 0;
  } else {
#endif
    mod = (querypos+1) % 2;
    this->plus_npositions[querypos+1] = Indexdb_count_right_subst_1(indexdb_fwd,this->forward_oligos[querypos]);
    this->minus_npositions[querypos+1] = Indexdb_count_left_subst_1(indexdb_rev,this->revcomp_oligos[querypos]);
    debug(printf("Counting at querypos %d, pos 1, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		 querypos,this->plus_npositions[querypos+1],this->forward_oligos[querypos],
		 this->minus_npositions[querypos+1],this->revcomp_oligos[querypos]));

#if 0
    /* Don't want boostpos to be a compoundpos */
    if (best_plus_querypos[mod] < 0 || this->plus_npositions[querypos+1] < best_plus_count[mod]) {
      best_plus_querypos[mod] = querypos+1;
      best_plus_count[mod] = this->plus_npositions[querypos+1];
    }
    if (best_minus_querypos[mod] < 0 || this->minus_npositions[querypos+1] < best_minus_count[mod]) {
      best_minus_querypos[mod] = querypos+1;
      best_minus_count[mod] = this->minus_npositions[querypos+1];
    }
#endif

#if 0
  }
#endif

  return;
}

/* Since querypos -2, -1, query_lastpos+1, and query_lastpos+2 are now
   stored as compoundpos, we no longer want to use them for boosting */
static void
most_specific_oligomer_3 (int *best_plus_querypos, int *best_minus_querypos, T this,
			  int query_lastpos, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev) {
  int querypos, mod;
  int best_plus_count[3], best_minus_count[3];

  /* Not needed, since this is the first procedure called */
  /* Block_restore(this->block5); */

  best_plus_querypos[0] = -3;
  best_plus_querypos[1] = -3;
  best_plus_querypos[2] = -3;
  best_minus_querypos[0] = -3;
  best_minus_querypos[1] = -3;
  best_minus_querypos[2] = -3;

  best_plus_count[0] = best_plus_count[1] = best_plus_count[2] = 0;
  best_minus_count[0] = best_minus_count[1] = best_minus_count[2] = 0;

#if 0
  if (this->validp[0] == false) {
    debug(printf("Not counting at querypos 0, neg 2 or neg 1 because validp is false at querypos 0\n"));
    this->plus_npositions[-2] = 0;
    this->plus_npositions[-1] = 0;
    this->minus_npositions[-2] = 0;
    this->minus_npositions[-1] = 0;
  } else {
#endif
    this->plus_npositions[-2] = Indexdb_count_left_subst_2(indexdb_fwd,this->forward_oligos[0]);
    this->minus_npositions[-2] = Indexdb_count_right_subst_2(indexdb_rev,this->revcomp_oligos[0]);
    debug(printf("Counting at querypos 0, neg 2, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		 this->plus_npositions[-2],this->forward_oligos[0],this->minus_npositions[-2],this->revcomp_oligos[0]));
    best_plus_count[1] = this->plus_npositions[-2];
    best_minus_count[1] = this->minus_npositions[-2];

    this->plus_npositions[-1] = Indexdb_count_left_subst_1(indexdb_fwd,this->forward_oligos[0]);
    this->minus_npositions[-1] = Indexdb_count_right_subst_1(indexdb_rev,this->revcomp_oligos[0]);
    debug(printf("Counting at querypos 0, neg 1, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		 this->plus_npositions[-1],this->forward_oligos[0],this->minus_npositions[-1],this->revcomp_oligos[0]));
    best_plus_count[2] = this->plus_npositions[-1];
    best_minus_count[2] = this->minus_npositions[-1];
#if 0
  }
#endif

  for (querypos = 0; querypos <= query_lastpos; querypos++) {
#if 0
    if (this->validp[querypos] == true) {
#endif
      mod = querypos % 3;
      this->plus_npositions[querypos] = Indexdb_count_no_subst(indexdb_fwd,this->forward_oligos[querypos]);
      this->minus_npositions[querypos] = Indexdb_count_no_subst(indexdb_rev,this->revcomp_oligos[querypos]);
      debug(printf("Counting at querypos %d, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		   querypos,this->plus_npositions[querypos],this->forward_oligos[querypos],
		   this->minus_npositions[querypos],this->revcomp_oligos[querypos]));

      if (best_plus_querypos[mod] < 0 || this->plus_npositions[querypos] < best_plus_count[mod]) {
	best_plus_querypos[mod] = querypos;
	best_plus_count[mod] = this->plus_npositions[querypos];
      }
      if (best_minus_querypos[mod] < 0 || this->minus_npositions[querypos] < best_minus_count[mod]) {
	best_minus_querypos[mod] = querypos;
	best_minus_count[mod] = this->minus_npositions[querypos];
      }
#if 0
    }
#endif
  }

  querypos = query_lastpos;
#if 0
  if (this->validp[querypos] == false) {
    debug(printf("Not counting at querypos %d (pos 1) or %d (pos 2) because validp is false at querypos %d\n",
		 querypos+1,querypos+2,querypos));
    this->plus_npositions[querypos+1] = 0;
    this->plus_npositions[querypos+2] = 0;
    this->minus_npositions[querypos+1] = 0;
    this->minus_npositions[querypos+2] = 0;
  } else {
#endif
    mod = (querypos+1) % 3;
    this->plus_npositions[querypos+1] = Indexdb_count_right_subst_1(indexdb_fwd,this->forward_oligos[querypos]);
    this->minus_npositions[querypos+1] = Indexdb_count_left_subst_1(indexdb_rev,this->revcomp_oligos[querypos]);
    debug(printf("Counting at querypos %d, pos 1, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		 querypos,this->plus_npositions[querypos+1],this->forward_oligos[querypos],
		 this->minus_npositions[querypos+1],this->revcomp_oligos[querypos]));

#if 0
    /* Don't want boostpos to be a compoundpos */
    if (best_plus_querypos[mod] < 0 || this->plus_npositions[querypos+1] < best_plus_count[mod]) {
      best_plus_querypos[mod] = querypos+1;
      best_plus_count[mod] = this->plus_npositions[querypos+1];
    }
    if (best_minus_querypos[mod] < 0 || this->minus_npositions[querypos+1] < best_minus_count[mod]) {
      best_minus_querypos[mod] = querypos+1;
      best_minus_count[mod] = this->minus_npositions[querypos+1];
    }
#endif

    mod = (querypos+2) % 3;
    this->plus_npositions[querypos+2] = Indexdb_count_right_subst_2(indexdb_fwd,this->forward_oligos[querypos]);
    this->minus_npositions[querypos+2] = Indexdb_count_left_subst_2(indexdb_rev,this->revcomp_oligos[querypos]);
    debug(printf("Counting at querypos %d, pos 2, plus_npositions = %d (oligo %06X), minus_npositions = %d (oligo %06X)\n",
		 querypos,this->plus_npositions[querypos+2],this->forward_oligos[querypos],
		 this->minus_npositions[querypos+2],this->revcomp_oligos[querypos]));

#if 0
    /* Don't want boostpos to be a compoundpos */
    if (best_plus_querypos[mod] < 0 || this->plus_npositions[querypos+2] < best_plus_count[mod]) {
      best_plus_querypos[mod] = querypos+2;
      best_plus_count[mod] = this->plus_npositions[querypos+2];
    }
    if (best_minus_querypos[mod] < 0 || this->minus_npositions[querypos+2] < best_minus_count[mod]) {
      best_minus_querypos[mod] = querypos+2;
      best_minus_count[mod] = this->minus_npositions[querypos+2];
    }
#endif

#if 0
  }
#endif

  return;
}


static List_T
find_spanning_exact_matches (int *found_score, int *nhits, List_T hits, T this, int genestrand,
			     int querylength, int query_lastpos, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev,
			     Compress_T query_compress_fwd, Compress_T query_compress_rev) {
  List_T spanningset, sorted;
  Spanningelt_T *array;
  int best_plus_querypos[MAX_INDEX1INTERVAL], best_minus_querypos[MAX_INDEX1INTERVAL];
  Univcoord_T *diagonals0, *positions0, diagonal0;
  int diagterm0, ndiagonals0, npositions0;
  int boostpos, mod, nelts, minscore, i;
  int global_miss_querypos5, global_miss_querypos3, elt_miss_querypos5, elt_miss_querypos3;
  int nempty;
  Chrnum_T chrnum;
  Univcoord_T chroffset, chrhigh;
  Chrpos_T chrlength;

  debug(printf("Starting find_spanning_exact_matches\n"));

  /* Use shortest list for candidate generation */
  if (index1interval == 3) {
    most_specific_oligomer_3(best_plus_querypos,best_minus_querypos,this,query_lastpos,indexdb_fwd,indexdb_rev);
  } else if (index1interval == 2) {
    most_specific_oligomer_2(best_plus_querypos,best_minus_querypos,this,query_lastpos,indexdb_fwd,indexdb_rev);
  } else {
    most_specific_oligomer_1(best_plus_querypos,best_minus_querypos,this,query_lastpos,indexdb_fwd,indexdb_rev);
  }

  /* Plus */
  for (mod = 0; mod < index1interval; mod++) {
    chrhigh = 0U;
    spanningset = Spanningelt_set(&minscore,this->forward_oligos,&this->plus_retrievedp,&this->plus_positions,
				  &this->plus_npositions,indexdb_fwd,query_lastpos,querylength,mod,/*plusp*/true);
    nelts = List_length(spanningset);
    array = (Spanningelt_T *) List_to_array(spanningset,NULL);
    List_free(&spanningset);

    boostpos = best_plus_querypos[mod];
    debug(printf("exact_matches, plus mod %d: proposed boostpos is %d\n",mod,boostpos));
    if (this->plus_npositions[boostpos] < minscore &&
	this->plus_retrievedp[boostpos] == false) {
      /* Boost */
      qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
      sorted = (List_T) NULL;
      for (i = nelts-1; i >= 0; --i) {
	sorted = List_push(sorted,array[i]);
      }
      FREE(array);
      this->plus_spanningset[mod] = sorted;

      /* Get boost positions */
      this->plus_positions[boostpos] =
	Indexdb_read_inplace(&(this->plus_npositions[boostpos]),indexdb_fwd,this->forward_oligos[boostpos]);
      this->plus_retrievedp[boostpos] = true;
      positions0 = this->plus_positions[boostpos];
      npositions0 = this->plus_npositions[boostpos];
      diagterm0 = querylength - boostpos; /* FORMULA */

      debug(printf("*** find_spanning_exact_matches, plus mod %d, with boost @ %d (%d positions)\n",
		   mod,boostpos,npositions0);
	    Spanningelt_print_set(sorted));

      spanningset = List_push(List_copy(sorted),(void **) NULL); /* Add a dummy list elt to front */
      nempty = 0;
      global_miss_querypos5 = querylength;
      global_miss_querypos3 = 0;

      while (--npositions0 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
#ifdef WORDS_BIGENDIAN
	debug7(printf("diag0 %d:%lu+%d advancing\n",npositions0,Bigendian_convert_univcoord(*positions0),diagterm0));
	diagonal0 = Bigendian_convert_univcoord(*positions0++) + diagterm0;
#else
	debug7(printf("diag0 %d:%lu+%d advancing\n",npositions0,(*positions0),diagterm0));
	diagonal0 = (*positions0++) + diagterm0;
#endif
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal0,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_fwd,
				       /*plusp*/true,genestrand,/*nmisses_allowed*/0,
				       /*nmisses_seen*/0,global_miss_querypos5,global_miss_querypos3);
      }
      List_free(&spanningset);

    } else {
      qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_candidates_cmp);
      if (nelts > 1) {
	qsort(&(array[1]),nelts-1,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
      }
      sorted = (List_T) NULL;
      for (i = nelts-1; i >= 0; --i) {
	sorted = List_push(sorted,array[i]);
      }
      FREE(array);
      this->plus_spanningset[mod] = sorted;

      debug(printf("*** find_spanning_exact_matches, plus mod %d, no boosting\n",mod));
      debug(Spanningelt_print_set(this->plus_spanningset[mod]));

      /* diagonals0 is now in correct endianness */
      diagonals0 = Spanningelt_diagonals(&ndiagonals0,(Spanningelt_T) sorted->first,&elt_miss_querypos5,&elt_miss_querypos3);
      spanningset = List_push(List_copy(sorted->rest),(void **) NULL); /* Add a dummy list elt to front */
      nempty = 0;
      global_miss_querypos5 = querylength;
      global_miss_querypos3 = 0;

      while (--ndiagonals0 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
	debug7(printf("diag0 %d:%lu advancing\n",ndiagonals0,(*diagonals0)));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,*diagonals0++,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_fwd,
				       /*plusp*/true,genestrand,/*nmisses_allowed*/0,
				       /*nmisses_seen*/0,global_miss_querypos5,global_miss_querypos3);
      }
      List_free(&spanningset);
    }
  }

  /* Minus */
  for (mod = 0; mod < index1interval; mod++) {
    chrhigh = 0U;
    spanningset = Spanningelt_set(&minscore,this->revcomp_oligos,&this->minus_retrievedp,&this->minus_positions,
				  &this->minus_npositions,indexdb_rev,query_lastpos,querylength,mod,/*plusp*/false);
    nelts = List_length(spanningset);
    array = (Spanningelt_T *) List_to_array(spanningset,NULL);
    List_free(&spanningset);

    boostpos = best_minus_querypos[mod];
    debug(printf("exact_matches, minus mod %d: proposed boostpos is %d\n",mod,boostpos));
    if (this->minus_npositions[boostpos] < minscore &&
	this->minus_retrievedp[boostpos] == false) {
      /* Boost */
      qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
      sorted = (List_T) NULL;
      for (i = nelts-1; i >= 0; --i) {
	sorted = List_push(sorted,array[i]);
      }
      FREE(array);
      this->minus_spanningset[mod] = sorted;

      /* Get boost positions */
      this->minus_positions[boostpos] =
	Indexdb_read_inplace(&(this->minus_npositions[boostpos]),indexdb_rev,this->revcomp_oligos[boostpos]);
      this->minus_retrievedp[boostpos] = true;
      positions0 = this->minus_positions[boostpos];
      npositions0 = this->minus_npositions[boostpos];
      diagterm0 = boostpos + index1part; /* FORMULA */

      debug(printf("*** find_spanning_exact_matches, minus mod %d, with boost @ %d (%d positions)\n",
		   mod,boostpos,npositions0);
	    Spanningelt_print_set(sorted));

      spanningset = List_push(List_copy(sorted),(void **) NULL);/* Add a dummy list elt to front */
      nempty = 0;
      global_miss_querypos5 = querylength;
      global_miss_querypos3 = 0;

      while (--npositions0 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
#ifdef WORDS_BIGENDIAN
	debug7(printf("diag0 %d:%lu+%d advancing\n",npositions0,Bigendian_convert_univcoord(*positions0),diagterm0));
	diagonal0 = Bigendian_convert_univcoord(*positions0++) + diagterm0;
#else
	debug7(printf("diag0 %d:%lu+%d advancing\n",npositions0,(*positions0),diagterm0));
	diagonal0 = (*positions0++) + diagterm0;
#endif
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal0,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_rev,
				       /*plusp*/false,genestrand,/*nmisses_allowed*/0,
				       /*nmisses_seen*/0,global_miss_querypos5,global_miss_querypos3);
      }
      List_free(&spanningset);

    } else {
      qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_candidates_cmp);
      if (nelts > 1) {
	qsort(&(array[1]),nelts-1,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
      }
      sorted = (List_T) NULL;
      for (i = nelts-1; i >= 0; --i) {
	sorted = List_push(sorted,array[i]);
      }
      FREE(array);
      this->minus_spanningset[mod] = sorted;

      debug(printf("*** find_spanning_exact_matches, minus mod %d, no boosting\n",mod));
      debug(Spanningelt_print_set(this->minus_spanningset[mod]));

      /* diagonals0 is now in correct endianness */
      diagonals0 = Spanningelt_diagonals(&ndiagonals0,(Spanningelt_T) sorted->first,&elt_miss_querypos5,&elt_miss_querypos3);
      spanningset = List_push(List_copy(sorted->rest),(void **) NULL); /* Add a dummy list elt to front */
      nempty = 0;
      global_miss_querypos5 = querylength;
      global_miss_querypos3 = 0;

      while (--ndiagonals0 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
	debug7(printf("diag0 %d:%lu advancing\n",ndiagonals0,(*diagonals0)));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,*diagonals0++,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_rev,
				       /*plusp*/false,genestrand,/*nmisses_allowed*/0,
				       /*nmisses_seen*/0,global_miss_querypos5,global_miss_querypos3);
      }
      List_free(&spanningset);
    }
  }

  return hits;
}


static List_T
find_spanning_onemiss_matches (int *found_score, int *nhits, List_T hits, T this, int genestrand, int querylength,
			       Compress_T query_compress_fwd, Compress_T query_compress_rev) {
  List_T spanningset, sorted;
  Spanningelt_T *array;
  Univcoord_T *diagonals0, *diagonals1, diagonal0, diagonal1;
  int global_miss_querypos5, global_miss_querypos3;
  int miss0_querypos5, miss0_querypos3, miss1_querypos5, miss1_querypos3;
  int mod, nelts, i;
  int ndiagonals0, ndiagonals1;
  int nempty;
  Chrnum_T chrnum;
  Univcoord_T chroffset, chrhigh;
  Chrpos_T chrlength;

  debug(printf("Starting find_spanning_onemiss_matches\n"));

  /* Plus */
  for (mod = 0; mod < index1interval; mod++) {
    debug(printf("Onemiss plus mod %d\n",mod));

    spanningset = this->plus_spanningset[mod];
    nelts = List_length(spanningset);
    array = (Spanningelt_T *) List_to_array(spanningset,NULL);
    /* List_free(&spanningset); */

    qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_candidates_cmp);
    if (nelts > 2) {
      qsort(&(array[2]),nelts-2,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
    }
    sorted = (List_T) NULL;
    for (i = nelts-1; i >= 0; --i) {
      sorted = List_push(sorted,Spanningelt_reset(array[i]));
    }
    FREE(array);

    debug(printf("*** find_spanning_onemiss_matches, plus mod %d\n",mod));
    debug(Spanningelt_print_set(sorted));

    /* diagonals0 and diagonals1 are now in correct endianness */
    diagonals0 = Spanningelt_diagonals(&ndiagonals0,(Spanningelt_T) sorted->first,&miss0_querypos5,&miss0_querypos3);
    diagonals1 = Spanningelt_diagonals(&ndiagonals1,(Spanningelt_T) sorted->rest->first,&miss1_querypos5,&miss1_querypos3);
    spanningset = List_push(List_copy(sorted->rest->rest),(void **) NULL); /* Add a dummy list elt to front */
    nempty = 0;
    global_miss_querypos5 = querylength;
    global_miss_querypos3 = 0;
    List_free(&sorted);
    chrhigh = 0U;

    while (ndiagonals0 > 0 && ndiagonals1 > 0 && nempty <= 1 && *nhits <= maxpaths_search) {
      if ((diagonal0 = (*diagonals0)) < (diagonal1 = (*diagonals1))) {
	debug7(printf("diag0 %d:%lu advancing\n",ndiagonals0,diagonal0));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal0,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_fwd,
				       /*plusp*/true,genestrand,/*nmisses_allowed*/1,
				       /*nmisses_seen*/1+nempty,miss1_querypos5,miss1_querypos3);
	++diagonals0;
	--ndiagonals0;

      } else if (diagonal1 < diagonal0) {
	debug7(printf("diag1 %d:%lu advancing\n",ndiagonals1,diagonal1));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal1,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_fwd,
				       /*plusp*/true,genestrand,/*nmisses_allowed*/1,
				       /*nmisses_seen*/1+nempty,miss0_querypos5,miss0_querypos3);
	++diagonals1;
	--ndiagonals1;

      } else {
	debug7(printf("diag0&1 %d:%lu == %d:%lu advancing\n",ndiagonals0,diagonal0,ndiagonals1,diagonal1));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal0,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_fwd,
				       /*plusp*/true,genestrand,/*nmisses_allowed*/1,
				       /*nmisses_seen*/nempty,global_miss_querypos5,global_miss_querypos3);
	++diagonals0;
	--ndiagonals0;
	++diagonals1;
	--ndiagonals1;
      }
    }

    while (--ndiagonals0 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
      debug7(printf("diag0 %d:%lu advancing\n",ndiagonals0+1,(*diagonals0)));
      hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,*diagonals0++,
				     /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				     querylength,/*query_compress*/query_compress_fwd,
				     /*plusp*/true,genestrand,/*nmisses_allowed*/1,
				     /*nmisses_seen*/1+nempty,miss1_querypos5,miss1_querypos3);
    }

    while (--ndiagonals1 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
      debug7(printf("diag1 %d:%lu advancing\n",ndiagonals1+1,(*diagonals1)));
      hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,*diagonals1++,
				     /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				     querylength,/*query_compress*/query_compress_fwd,
				     /*plusp*/true,genestrand,/*nmisses_allowed*/1,
				     /*nmisses_seen*/1+nempty,miss0_querypos5,miss0_querypos3);
    }

    List_free(&spanningset);
  }

  /* Minus */
  for (mod = 0; mod < index1interval; mod++) {
    debug(printf("Onemiss minus mod %d\n",mod));

    spanningset = this->minus_spanningset[mod];
    nelts = List_length(spanningset);
    array = (Spanningelt_T *) List_to_array(spanningset,NULL);
    /* List_free(&spanningset); */

    qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_candidates_cmp);
    if (nelts > 2) {
      qsort(&(array[2]),nelts-2,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
    }
    sorted = (List_T) NULL;
    for (i = nelts-1; i >= 0; --i) {
      sorted = List_push(sorted,Spanningelt_reset(array[i]));
    }
    FREE(array);

    debug(printf("*** find_spanning_onemiss_matches, minus mod %d\n",mod));
    debug(Spanningelt_print_set(sorted));

    /* diagonals0 and diagonals1 are now in correct endianness */
    diagonals0 = Spanningelt_diagonals(&ndiagonals0,(Spanningelt_T) sorted->first,&miss0_querypos5,&miss0_querypos3);
    diagonals1 = Spanningelt_diagonals(&ndiagonals1,(Spanningelt_T) sorted->rest->first,&miss1_querypos5,&miss1_querypos3);
    spanningset = List_push(List_copy(sorted->rest->rest),(void **) NULL); /* Add a dummy list to front */
    nempty = 0;
    global_miss_querypos5 = querylength;
    global_miss_querypos3 = 0;
    List_free(&sorted);
    chrhigh = 0U;

    while (ndiagonals0 > 0 && ndiagonals1 > 0 && nempty <= 1 && *nhits <= maxpaths_search) {
      if ((diagonal0 = (*diagonals0)) < (diagonal1 = (*diagonals1))) {
	debug7(printf("diag0 %d:%lu advancing\n",ndiagonals0,(*diagonals0)));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal0,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_rev,
				       /*plusp*/false,genestrand,/*nmisses_allowed*/1,
				       /*nmisses_seen*/1+nempty,miss1_querypos5,miss1_querypos3);
	++diagonals0;
	--ndiagonals0;

      } else if (diagonal1 < diagonal0) {
	debug7(printf("diag1 %d:%lu advancing\n",ndiagonals1,(*diagonals1)));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal1,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_rev,
				       /*plusp*/false,genestrand,/*nmisses_allowed*/1,
				       /*nmisses_seen*/1+nempty,miss0_querypos5,miss0_querypos3);
	++diagonals1;
	--ndiagonals1;

      } else {
	debug7(printf("diag0&1 %d:%lu == %d:%lu advancing\n",ndiagonals0,diagonal0,ndiagonals1,diagonal1));
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal0,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_rev,
				       /*plusp*/false,genestrand,/*nmisses_allowed*/1,
				       /*nmisses_seen*/nempty,global_miss_querypos5,global_miss_querypos3);
	++diagonals0;
	--ndiagonals0;
	++diagonals1;
	--ndiagonals1;
      }
    }

    while (--ndiagonals0 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
      debug7(printf("diag0 %d:%lu advancing\n",ndiagonals0+1,(*diagonals0)));
      hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,*diagonals0++,
				     /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				     querylength,/*query_compress*/query_compress_rev,
				     /*plusp*/false,genestrand,/*nmisses_allowed*/1,
				     /*nmisses_seen*/1+nempty,miss1_querypos5,miss1_querypos3);
    }

    while (--ndiagonals1 >= 0 && nempty == 0 && *nhits <= maxpaths_search) {
      debug7(printf("diag1 %d:%lu advancing\n",ndiagonals1+1,(*diagonals1)));
      hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,*diagonals1++,
				     /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				     querylength,/*query_compress*/query_compress_rev,
				     /*plusp*/false,genestrand,/*nmisses_allowed*/1,
				     /*nmisses_seen*/1+nempty,miss0_querypos5,miss0_querypos3);
    }

    List_free(&spanningset);
  }

  return hits;
}


static List_T
find_spanning_multimiss_matches (int *found_score, int *nhits, List_T hits, T this, int genestrand, int nrequired, int querylength,
				 Compress_T query_compress_fwd, Compress_T query_compress_rev,
				 int nmisses_allowed) {
  Univcoord_T *diagonals, diagonal;
  List_T spanningset, sorted;
  Spanningelt_T *array;
  int nunion = nmisses_allowed + nrequired, nelts;
  int heapsize, count, mod, i;
  int ndiagonals, nempty;
  int parenti, smallesti, righti;
  int global_miss_querypos5, global_miss_querypos3;
  int elt_miss_querypos5, elt_miss_querypos3;
  struct Batch_T *batchpool, sentinel_struct;
  Batch_T *heap, batch, sentinel;
  Univcoord_T chroffset, chrhigh;
  Chrpos_T chrlength;
  Chrnum_T chrnum;

  debug(printf("Starting find_spanning_multimiss_matches with %d misses allowed\n",nmisses_allowed));

  sentinel_struct.diagonal = (Univcoord_T) -1; /* infinity */
  sentinel = &sentinel_struct;

  batchpool = (struct Batch_T *) CALLOC(nunion,sizeof(struct Batch_T));
  heap = (Batch_T *) CALLOC(2*(nunion+1)+1+1,sizeof(Batch_T)); /* being liberal with allocation */

  /* Plus */
  for (mod = 0; mod < index1interval; mod++) {
    debug(printf("Multimiss plus mod %d\n",mod));

    spanningset = this->plus_spanningset[mod];
    nelts = List_length(spanningset);
    array = (Spanningelt_T *) List_to_array(spanningset,NULL);
    /* List_free(&spanningset); */

    qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_candidates_cmp);
    if (nelts > nunion) {
      qsort(&(array[nunion]),nelts-nunion,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
    }
    sorted = (List_T) NULL;
    for (i = nelts-1; i >= 0; --i) {
      sorted = List_push(sorted,Spanningelt_reset(array[i]));
    }
    FREE(array);

    debug(printf("*** find_spanning_multimiss_matches, %d misses allowed, plus mod %d\n",nmisses_allowed,mod));
    debug(Spanningelt_print_set(sorted));

    /* Put first few pointers into heap */
    heapsize = 0;
    spanningset = sorted;
    global_miss_querypos5 = querylength;
    global_miss_querypos3 = 0;
    for (i = 0; i < nunion && spanningset; i++, spanningset = spanningset->rest) {
      /* Get list as a special one, and perform conversion if necessary */
      diagonals = Spanningelt_diagonals(&ndiagonals,(Spanningelt_T) spanningset->first,&elt_miss_querypos5,&elt_miss_querypos3);
      if (elt_miss_querypos5 < global_miss_querypos5) global_miss_querypos5 = elt_miss_querypos5;
      if (elt_miss_querypos3 > global_miss_querypos3) global_miss_querypos3 = elt_miss_querypos3;

      batch = &(batchpool[i]);
      debug(printf("Adding batch %d of size %d...",i,ndiagonals));
      if (ndiagonals > 0) {
	Batch_init_simple(batch,diagonals,ndiagonals,querylength,/*querypos*/i);
	if (batch->npositions > 0) {
	  debug(printf("inserting into heap"));
	  min_heap_insert_simple(heap,&heapsize,batch);
	}
      }
      debug(printf("\n"));
    }
    debug(printf("heapsize is %d\n",heapsize));
    if (heapsize == 0) {
      List_free(&sorted);
    } else {
      spanningset = List_push(List_copy(spanningset),(void **) NULL); /* Add a dummy list elt to front */
      nempty = 0;
      List_free(&sorted);

      /* Set up rest of heap */
      for (i = heapsize+1; i <= 2*heapsize+1; i++) {
	heap[i] = sentinel;
      }

      debug7(printf("*** multimiss mod %d plus:\n",mod));

      /* Initialize loop */
      batch = heap[1];
      diagonal = batch->diagonal;
      count = 1;
      debug7(printf("at #%d, initial diagonal is %lu\n",batch->querypos,diagonal));

      /* Update batch */
      if (--batch->npositions <= 0) {
	/* Use last entry in heap for heapify */
	batch = heap[heapsize];
	heap[heapsize--] = sentinel;
      } else {
	/* Use this batch for heapify */
	/* These positions are diagonals, and already in correct endianness */
	batch->diagonal = *(++batch->positions);
      }

      /* Heapify down */
      debug6(printf("Starting heapify with %lu\n",diagonal));
      parenti = 1;
      smallesti = (heap[3]->diagonal < heap[2]->diagonal) ? 3 : 2;
      debug6(printf("Comparing left %d/right %d: %lu and %lu\n",2,3,heap[2]->diagonal,heap[3]->diagonal));
      while (batch->diagonal > heap[smallesti]->diagonal) {
	heap[parenti] = heap[smallesti];
	parenti = smallesti;
	smallesti = LEFT(parenti);
	righti = smallesti+1;
	debug6(printf("Comparing left %d/right %d: %lu and %lu\n",
		      smallesti,righti,heap[smallesti]->diagonal,heap[righti]->diagonal));
	if (heap[righti]->diagonal < heap[smallesti]->diagonal) {
	  smallesti = righti;
	}
      }
      heap[parenti] = batch;
      debug6(printf("Inserting at %d\n\n",parenti));

      /* Iterate through heap */
      chrhigh = 0U;
      while (heapsize > 0 && *nhits <= maxpaths_search) {
	batch = heap[1];

	if (batch->diagonal == diagonal) {
	  count++;
	  debug7(printf("at #%d, incrementing diagonal %lu to count %d\n",batch->querypos,diagonal,count));
	} else {
	  /* End of diagonal */
	  if (count >= nrequired) {
	    /* printf("Testing %d..%d\n",miss_querypos5,miss_querypos3); */
	    hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal,
					   /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
					   querylength,/*query_compress*/query_compress_fwd,
					   /*plusp*/true,genestrand,nmisses_allowed,
					   /*nmisses_seen*/nunion-count+nempty,global_miss_querypos5,global_miss_querypos3);
	  }
	  diagonal = batch->diagonal;
	  count = 1;
	  debug7(printf("at #%d, next diagonal is %lu\n",batch->querypos,diagonal));
	}

	/* Update batch */
	if (--batch->npositions <= 0) {
	  /* Use last entry in heap for heapify */
	  batch = heap[heapsize];
	  heap[heapsize--] = sentinel;
	} else {
	  /* Use this batch for heapify */
	  /* These positions are diagonals, and already in correct endianness */
	  batch->diagonal = *(++batch->positions);
	}

	/* Heapify down */
	debug6(printf("Starting heapify with %lu\n",diagonal));
	parenti = 1;
	smallesti = (heap[3]->diagonal < heap[2]->diagonal) ? 3 : 2;
	debug6(printf("Comparing left %d/right %d: %lu and %lu\n",2,3,heap[2]->diagonal,heap[3]->diagonal));
	while (batch->diagonal > heap[smallesti]->diagonal) {
	  heap[parenti] = heap[smallesti];
	  parenti = smallesti;
	  smallesti = LEFT(parenti);
	  righti = smallesti+1;
	  debug6(printf("Comparing left %d/right %d: %lu and %lu\n",
			smallesti,righti,heap[smallesti]->diagonal,heap[righti]->diagonal));
	  if (heap[righti]->diagonal < heap[smallesti]->diagonal) {
	    smallesti = righti;
	  }
	}
	heap[parenti] = batch;
	debug6(printf("Inserting at %d\n\n",parenti));
      }

      /* Terminate loop */
      if (count >= nrequired && *nhits <= maxpaths_search) {
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_fwd,
				       /*plusp*/true,genestrand,nmisses_allowed,
				       /*nmisses_seen*/nunion-count+nempty,global_miss_querypos5,global_miss_querypos3);
      }
      List_free(&spanningset);
    }
  }

  /* Minus */
  for (mod = 0; mod < index1interval; mod++) {
    debug(printf("Multimiss minus mod %d\n",mod));

    spanningset = this->minus_spanningset[mod];
    nelts = List_length(spanningset);
    array = (Spanningelt_T *) List_to_array(spanningset,NULL);
    /* List_free(&spanningset); */

    qsort(array,nelts,sizeof(Spanningelt_T),Spanningelt_candidates_cmp);
    if (nelts > nunion) {
      qsort(&(array[nunion]),nelts-nunion,sizeof(Spanningelt_T),Spanningelt_pruning_cmp);
    }
    sorted = (List_T) NULL;
    for (i = nelts-1; i >= 0; --i) {
      sorted = List_push(sorted,Spanningelt_reset(array[i]));
    }
    FREE(array);

    debug(printf("*** find_spanning_multimiss_matches, %d misses_allowed, minus mod %d\n",nmisses_allowed,mod));
    debug(Spanningelt_print_set(sorted));

    /* Put first few pointers into heap */
    heapsize = 0;
    spanningset = sorted;
    global_miss_querypos5 = querylength;
    global_miss_querypos3 = 0;
    for (i = 0; i < nunion && spanningset; i++, spanningset = spanningset->rest) {
      /* Get list as a special one, and perform conversion if necessary */
      diagonals = Spanningelt_diagonals(&ndiagonals,(Spanningelt_T) spanningset->first,&elt_miss_querypos5,&elt_miss_querypos3);
      if (elt_miss_querypos5 < global_miss_querypos5) global_miss_querypos5 = elt_miss_querypos5;
      if (elt_miss_querypos3 > global_miss_querypos3) global_miss_querypos3 = elt_miss_querypos3;

      batch = &(batchpool[i]);
      debug(printf("Adding batch %d of size %d...",i,ndiagonals));
      if (ndiagonals > 0) {
	Batch_init_simple(batch,diagonals,ndiagonals,querylength,/*querypos*/i);
	if (batch->npositions > 0) {
	  debug(printf("inserting into heap"));
	  min_heap_insert_simple(heap,&heapsize,batch);
	}
      }
      debug(printf("\n"));
    }
    debug(printf("heapsize is %d\n",heapsize));

    if (heapsize == 0) {
      List_free(&sorted);
    } else {
      spanningset = List_push(List_copy(spanningset),(void **) NULL); /* Add a dummy list elt to front */
      nempty = 0;
      List_free(&sorted);

      /* Set up rest of heap */
      for (i = heapsize+1; i <= 2*heapsize+1; i++) {
	heap[i] = sentinel;
      }

      debug7(printf("*** multimiss mod %d minus:\n",mod));

      /* Initialize loop */
      batch = heap[1];
      diagonal = batch->diagonal;
      count = 1;
      debug7(printf("at #%d, initial diagonal is %lu\n",batch->querypos,diagonal));

      /* Update batch */
      if (--batch->npositions <= 0) {
	/* Use last entry in heap for heapify */
	batch = heap[heapsize];
	heap[heapsize--] = sentinel;
      } else {
	/* Use this batch for heapify */
	/* These positions are diagonals, and already in correct endianness */
	batch->diagonal = *(++batch->positions);
      }

      /* Heapify down */
      debug6(printf("Starting heapify with %lu\n",diagonal));
      parenti = 1;
      smallesti = (heap[3]->diagonal < heap[2]->diagonal) ? 3 : 2;
      debug6(printf("Comparing left %d/right %d: %lu and %lu\n",2,3,heap[2]->diagonal,heap[3]->diagonal));
      while (batch->diagonal > heap[smallesti]->diagonal) {
	heap[parenti] = heap[smallesti];
	parenti = smallesti;
	smallesti = LEFT(parenti);
	righti = smallesti+1;
	debug6(printf("Comparing left %d/right %d: %lu and %lu\n",
		      smallesti,righti,heap[smallesti]->diagonal,heap[righti]->diagonal));
	if (heap[righti]->diagonal < heap[smallesti]->diagonal) {
	  smallesti = righti;
	}
      }
      heap[parenti] = batch;
      debug6(printf("Inserting at %d\n\n",parenti));

      /* Iterate through heap */
      chrhigh = 0U;
      while (heapsize > 0 && *nhits <= maxpaths_search) {
	batch = heap[1];

	if (batch->diagonal == diagonal) {
	  count++;
	  debug7(printf("at #%d, incrementing diagonal %lu to count %d\n",batch->querypos,diagonal,count));
	} else {
	  /* End of diagonal */
	  if (count >= nrequired) {
	    hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal,
					   /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
					   querylength,/*query_compress*/query_compress_rev,
					   /*plusp*/false,genestrand,nmisses_allowed,
					   /*nmisses_seen*/nunion-count+nempty,global_miss_querypos5,global_miss_querypos3);
	  }
	  diagonal = batch->diagonal;
	  count = 1;
	  debug7(printf("at #%d, next diagonal is %lu\n",batch->querypos,diagonal));
	}

	/* Update batch */
	if (--batch->npositions <= 0) {
	  /* Use last entry in heap for heapify */
	  batch = heap[heapsize];
	  heap[heapsize--] = sentinel;
	} else {
	  /* Use this batch for heapify */
	  /* These positions are diagonals, and already in correct endianness */
	  batch->diagonal = *(++batch->positions);
	}

	/* Heapify down */
	debug6(printf("Starting heapify with %lu\n",diagonal));
	parenti = 1;
	smallesti = (heap[3]->diagonal < heap[2]->diagonal) ? 3 : 2;
	debug6(printf("Comparing left %d/right %d: %lu and %lu\n",2,3,heap[2]->diagonal,heap[3]->diagonal));
	while (batch->diagonal > heap[smallesti]->diagonal) {
	  heap[parenti] = heap[smallesti];
	  parenti = smallesti;
	  smallesti = LEFT(parenti);
	  righti = smallesti+1;
	  debug6(printf("Comparing left %d/right %d: %lu and %lu\n",
			smallesti,righti,heap[smallesti]->diagonal,heap[righti]->diagonal));
	  if (heap[righti]->diagonal < heap[smallesti]->diagonal) {
	    smallesti = righti;
	  }
	}
	heap[parenti] = batch;
	debug6(printf("Inserting at %d\n\n",parenti));
      }

      /* Terminate loop */
      if (count >= nrequired && *nhits <= maxpaths_search) {
	hits = identify_multimiss_iter(&(*found_score),&chrnum,&chroffset,&chrhigh,&chrlength,&(*nhits),hits,diagonal,
				       /*prev*/spanningset,&nempty,&global_miss_querypos5,&global_miss_querypos3,
				       querylength,/*query_compress*/query_compress_rev,
				       /*plusp*/false,genestrand,nmisses_allowed,
				       /*nmisses_seen*/nunion-count+nempty,global_miss_querypos5,global_miss_querypos3);
      }
      List_free(&spanningset);
    }
  }

  FREE(heap);
  FREE(batchpool);
  return hits;
}


/************************************************************************/


#if 0
static void
trim_ends_unknowns_only (int *trim5, int *trim3, char *sequence1, char *sequence2, int length) {
  int pos;

  pos = 0;
  while (pos < length && sequence2[pos] == OUTOFBOUNDS) {
    pos++;
  }
  debug8(printf("outofbounds: trim 5': at %d: %c != %c\n",pos,sequence2[pos],OUTOFBOUNDS));
  *trim5 = pos;

  pos = length-1;
  debug8(printf("outofbounds: trim 3': %d:%c\n",pos,sequence2[pos]));
  while (pos >= 0 && sequence2[pos] == OUTOFBOUNDS) {
    pos--;
  }
  *trim3 = pos+1;
  debug8(printf("outofbounds: trim 3': %d - %d\n",length,*trim3));
  *trim3 = length - (*trim3);

  debug8(
	 printf("At query ->: %.*s\n",length,sequence1);
	 printf("At genome->: %.*s\n",length,sequence2);
	 printf("%02d %02d    ->: ",*trim5,*trim3);
	 for (pos = 0; pos < *trim5; pos++) {
	   printf(" ");
	 }
	 for ( ; pos < length - (*trim3); pos++) {
	   printf("*");
	 }
	 for ( ; pos < length; pos++) {
	   printf(" ");
	 }
	 printf("\n");
	 );

  return;
}
#endif


/************************************************************************/


/* Returns a master pointer (segments) to the block of segments */
/* If end_indel_mismatches_allowed set to 0, won't save any segments for end indels. */
static List_T
find_complete_mm (int *found_score, int *nhits, List_T hits, struct Segment_T *segments, int nsegments,
		  int querylength, Compress_T query_compress,
		  int max_mismatches_allowed, bool plusp, int genestrand) {
  Stage3end_T hit;
  int nmismatches;
  Univcoord_T left;
  Segment_T segmenti;

  for (segmenti = segments; segmenti < &(segments[nsegments]); segmenti++) {
    if (segmenti->diagonal == (Univcoord_T) -1) {
      /* Skip chr marker segment */
    } else if (segmenti->floor <= max_mismatches_allowed) {
      left = segmenti->diagonal - querylength;
      nmismatches = Genome_count_mismatches_limit(query_compress,left,/*pos5*/0,/*pos3*/querylength,
						  max_mismatches_allowed,plusp,genestrand);
      if (nmismatches <= max_mismatches_allowed) {
	if ((hit = Stage3end_new_substitution(&(*found_score),nmismatches,
					      left,/*genomiclength*/querylength,
					      query_compress,plusp,genestrand,segmenti->chrnum,
					      segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
					      /*sarrayp*/false)) != NULL) {
	  segmenti->usedp = true;
	  *nhits += 1;
	  hits = List_push(hits,(void *) hit);
	}
      }
    }
  }

  return hits;
}


static struct Segment_T *
identify_all_segments (int *nsegments, Segment_T **spliceable, int *nspliceable,
		       Univcoord_T **positions, int *npositions,
		       bool *omitted, int querylength, int query_lastpos, Floors_T floors,
		       bool plusp) {
  struct Segment_T *segments = NULL;
  Batch_T batch, sentinel;
  struct Batch_T sentinel_struct, *batchpool;
  Batch_T *heap;
  int heapsize = 0;
  int parenti, smallesti, righti, i;
  int querypos, first_querypos, last_querypos;
  int floor, floor_xfirst, floor_xlast;
  int floor_left, floor_right, floor_incr;
  int *floors_from_neg3, *floors_from_xfirst, *floors_to_xlast, *floors_to_pos3;
  /* int exclude_xfirst, exclude_xlast; */
  Univcoord_T diagonal, segment_left, last_diagonal, chroffset = 0U, chrhigh = 0U;
  Chrpos_T chrlength, max_distance;
  Chrnum_T chrnum = 1;
#ifdef OLD_FLOOR_ENDS
  int halfquerylength, halfquery_lastpos;
#endif

#ifdef DIAGONAL_ADD_QUERYPOS
  UINT8 diagonal_add_querypos;
#endif
  int total_npositions = 0;
  int joffset = 0, j;

  Segment_T ptr, ptr_chrstart;
  Segment_T *ptr_spliceable;
#ifdef DEBUG19
  Segment_T ptr0;
  int k;
#endif
#ifndef SLOW_CHR_UPDATE
  Univcoord_T goal;
  int nchromosomes_local = nchromosomes;
  Univcoord_T *chrhighs_local = chrhighs;
#endif

  Univcoord_T *splicesites_local;
  int nsplicesites_local;

  debug(printf("*** Starting identify_all_segments ***\n"));

  if (splicesites == NULL) {
    splicesites_local = (Univcoord_T *) CALLOC(1,sizeof(Univcoord_T));
    splicesites_local[0] = (Univcoord_T) -1;
    nsplicesites_local = 0;
  } else {
    splicesites_local = splicesites;
    nsplicesites_local = nsplicesites;
  }

#ifdef OLD_FLOOR_ENDS
  halfquerylength = querylength / 2;
  halfquery_lastpos = halfquerylength - index1part;
#endif

  /* Create sentinel */
#ifdef DIAGONAL_ADD_QUERYPOS
  sentinel_struct.diagonal_add_querypos = (Univcoord_T) -1; /* infinity */
  sentinel_struct.diagonal_add_querypos <<= 32;
#else
  sentinel_struct.querypos = querylength; /* essentially infinity */
  sentinel_struct.diagonal = (Univcoord_T) -1; /* infinity */
#endif
  sentinel = &sentinel_struct;

  /* Set up batches */
  batchpool = (struct Batch_T *) MALLOC((query_lastpos+1) * sizeof(struct Batch_T));
  heap = (Batch_T *) MALLOC((2*(query_lastpos+1)+1+1) * sizeof(Batch_T));

  /* Don't add entries for compoundpos positions (skip querypos -2, -1, lastpos+1, lastpos+2) */
  if (plusp) {
    for (querypos = 0, i = 0; querypos <= query_lastpos; querypos++) {
      if (omitted[querypos] == true) {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      } else if (npositions[querypos] > 0) {
	debug1(printf("Adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
	batch = &(batchpool[i]);
	Batch_init(batch,querypos,/*diagterm*/querylength - querypos,positions[querypos],npositions[querypos],querylength);
	total_npositions += npositions[querypos];
	if (batch->npositions > 0) {
	  min_heap_insert(heap,&heapsize,batch);
	  i++;
	}
      } else {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      }
    }
  } else {
    for (querypos = 0, i = 0; querypos <= query_lastpos; querypos++) {
      if (omitted[querypos] == true) {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      } else if (npositions[querypos] > 0) {
	debug1(printf("Adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
	batch = &(batchpool[i]);
	Batch_init(batch,querypos,/*diagterm*/querypos + index1part,positions[querypos],npositions[querypos],querylength);
	total_npositions += npositions[querypos];
	if (batch->npositions > 0) {
	  min_heap_insert(heap,&heapsize,batch);
	  i++;
	}
      } else {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      }
    }
  }
  debug14(printf("Initial total_npositions = %d\n",total_npositions));


  if (i == 0) {
    FREE(heap);
    FREE(batchpool);
    *nsegments = 0;
    if (splicesites == NULL) {
      FREE(splicesites_local);
    }
    return (struct Segment_T *) NULL;
  }

  /* Set up rest of heap */
  for (i = heapsize+1; i <= 2*heapsize+1; i++) {
    heap[i] = sentinel;
  }

  /* Putting chr marker "segments" after each chromosome */
  segments = (struct Segment_T *) MALLOC((total_npositions + nchromosomes) * sizeof(struct Segment_T));
  ptr_chrstart = ptr = &(segments[0]);
  if (overall_max_distance == 0) {
    ptr_spliceable = *spliceable = (Segment_T *) NULL;
  } else {
    ptr_spliceable = *spliceable = (Segment_T *) CALLOC(total_npositions,sizeof(Segment_T));
  }

  /*
  if ((exclude_xfirst = firstbound-2-index1part-max_end_insertions) < 3) {
    exclude_xfirst = 3;
  }
  if ((exclude_xlast = lastbound+1+max_end_insertions) > query_lastpos-3) {
    exclude_xlast = query_lastpos-3;
  }
  */

#if 0
  /* Should account for firstbound and lastbound */
  floors_from_xfirst = floors->scorefrom[/* xfirst_from = */ firstbound-index1interval+max_end_insertions];
  floors_to_xlast = floors->scoreto[/* xlast_to = */ lastbound+1+index1interval-index1part-max_end_insertions];
#else
  if (spansize /* +max_end_insertions */ > query_lastpos + index1interval) {
    floors_from_xfirst = floors->scorefrom[query_lastpos+index1interval];
  } else {
    floors_from_xfirst = floors->scorefrom[spansize /* +max_end_insertions */];
  }
  if (query_lastpos-spansize /* -max_end_insertions */ < -index1interval) {
    floors_to_xlast = floors->scoreto[-index1interval];
  } else {
    floors_to_xlast = floors->scoreto[query_lastpos-spansize /* -max_end_insertions */];
  }
#endif
  floors_from_neg3 = floors->scorefrom[-index1interval];
  floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];


  /* Initialize loop */
  batch = heap[1];
  first_querypos = last_querypos = querypos = batch->querypos;
  last_diagonal = diagonal = batch->diagonal;

  floor_incr = floors_from_neg3[first_querypos];
  floor = floor_incr;
  floor_xlast = floor_incr;
  floor_xfirst = floors_from_xfirst[first_querypos] /* floors->scorefrom[xfirst_from][first_querypos] */;

#ifdef OLD_FLOOR_ENDS
  if (querypos < halfquery_lastpos) {
    floor_left = floor_incr;
  } else {
    floor_left = floors->scorefrom[-index1interval][halfquery_lastpos];
  }
  if (querypos < halfquerylength) {
    floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos];
  } else {
    floor_right = floors->scorefrom[halfquerylength-index1interval][first_querypos];
  }
#else
  floor_left = floor_incr;
#ifdef DEBUG1
  floor_right = -99;
#endif
#endif


  debug1(printf("multiple_mm_%s, diagonal %lu, querypos %d\n",plusp ? "plus" : "minus",diagonal,querypos));
  debug1(printf("first_querypos = %d => initial values: floor %d, floor_xfirst %d, floor_xlast %d, floor_left %d, floor_right %d\n",
	        first_querypos,floor,floor_xfirst,floor_xlast,floor_left,floor_right));

  if (--batch->npositions <= 0) {
    /* Use last entry in heap for insertion */
    batch = heap[heapsize];
    querypos = batch->querypos;
    heap[heapsize--] = sentinel;

  } else {
    /* Use this batch for insertion (same querypos) */
#ifdef WORDS_BIGENDIAN
    batch->diagonal = Bigendian_convert_univcoord(*(++batch->positions)) + batch->diagterm;
#else
    batch->diagonal = *(++batch->positions) + batch->diagterm;
#endif
#ifdef DIAGONAL_ADD_QUERYPOS
    batch->diagonal_add_querypos = (UINT8) batch->diagonal;
    batch->diagonal_add_querypos <<= 32;
    batch->diagonal_add_querypos |= querypos /* Previously added 2 because querypos was -2: + 2*/;
#endif
  }

  /* heapify */
  parenti = 1;
#ifdef DIAGONAL_ADD_QUERYPOS
  diagonal_add_querypos = batch->diagonal_add_querypos;
  smallesti = (heap[3]->diagonal_add_querypos < heap[2]->diagonal_add_querypos) ? 3 : 2;
  while (diagonal_add_querypos > heap[smallesti]->diagonal_add_querypos) {
    heap[parenti] = heap[smallesti];
    parenti = smallesti;
    smallesti = LEFT(parenti);
    righti = smallesti+1;
    if (heap[righti]->diagonal_add_querypos < heap[smallesti]->diagonal_add_querypos) {
      smallesti = righti;
    }
  }
#else
  diagonal = batch->diagonal;
  smallesti = ((heap[3]->diagonal < heap[2]->diagonal) ||
	       ((heap[3]->diagonal == heap[2]->diagonal) &&
		(heap[3]->querypos < heap[2]->querypos))) ? 3 : 2;
  /* Note that diagonal/querypos will never exceed a sentinel diagonal/querypos */
  while (diagonal > heap[smallesti]->diagonal ||
	 (diagonal == heap[smallesti]->diagonal &&
	  querypos > heap[smallesti]->querypos)) {
    heap[parenti] = heap[smallesti];
    parenti = smallesti;
    smallesti = LEFT(parenti);
    righti = smallesti+1;
    if ((heap[righti]->diagonal < heap[smallesti]->diagonal) ||
		  ((heap[righti]->diagonal == heap[smallesti]->diagonal) &&
		   (heap[righti]->querypos < heap[smallesti]->querypos))) {
      smallesti = righti;
    }
  }
#endif
  heap[parenti] = batch;


  /* Continue after initialization */
  while (heapsize > 0) {
    batch = heap[1];
    querypos = batch->querypos;
    diagonal = batch->diagonal;
    debug14(printf("diagonal = %u, querypos = %d\n",last_diagonal,last_querypos));

    if (diagonal == last_diagonal) {
      /* Continuing exact match or substitution */
      floor_incr = floors->scorefrom[last_querypos][querypos];
      floor += floor_incr;
      floor_xfirst += floor_incr;
      floor_xlast += floor_incr;

#ifdef OLD_FLOOR_ENDS
      /* Why is this here?  Just set floor_left at start and floor_right at end. */
      if (querypos < halfquery_lastpos) {
	floor_left += floor_incr;
      } else if (last_querypos < halfquery_lastpos) {
	/* Finish floor_left */
	floor_left += floors->scorefrom[last_querypos][halfquery_lastpos+index1interval];
      }
      if (querypos >= halfquerylength) {
	if (last_querypos < halfquerylength) {
	  /* Start floor_right */
	  floor_right = floors->scorefrom[halfquerylength-index1interval][querypos];
	} else {
	  floor_right += floor_incr;
	}
      }
#endif

      debug1(printf("diagonal %lu unchanged: last_querypos = %d, querypos = %d => floor increments by %d\n",
		    diagonal,last_querypos,querypos,floor_incr));
      debug1(printf("*multiple_mm_%s, diagonal %lu, querypos %d, floor %d, floor_xfirst %d, floor_xlast %d, floor_left %d, floor_right %d\n",
		    plusp ? "plus" : "minus",diagonal,querypos,floor,floor_xfirst,floor_xlast,floor_left,floor_right));
    } else {
      /* End of diagonal */
      floor_incr = floors_to_pos3[last_querypos]  /* floors->score[last_querypos][query_lastpos+index1interval] */;
      floor += floor_incr;
      floor_xfirst += floor_incr;
      floor_xlast += floors_to_xlast[last_querypos];  /* floors->score[last_querypos][xlast_to]; */

#ifdef OLD_FLOOR_ENDS
      if (last_querypos < halfquery_lastpos) {
	floor_left += floors->scorefrom[last_querypos][halfquery_lastpos+index1interval];
	floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos+index1interval];
      }
      if (last_querypos >= halfquerylength) {
	floor_right += floor_incr;
      }
#else
      floor_right = floor_incr;
#endif

      debug1(printf("new diagonal %lu > last diagonal %lu: last_querypos = %d => final values: floor %d, floor_xfirst %d, floor_xlast %d, floor_left %d, floor_right %d\n",
		    diagonal,last_diagonal,last_querypos,floor,floor_xfirst,floor_xlast,floor_left,floor_right));

      if (last_diagonal > chrhigh) {
	if (ptr > ptr_chrstart) {
	  /* Add chr marker segment */
	  debug1(printf("=== ptr %p > ptr_chrstart %p, so adding chr marker segment\n",ptr,ptr_chrstart));
	  ptr->diagonal = (Univcoord_T) -1;
	  ptr_chrstart = ++ptr;
	}

	/* update chromosome bounds, based on low end */
#ifdef SLOW_CHR_UPDATE
	chrnum = Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength);
	Univ_IIT_interval_bounds(&chroffset,&chrhigh,&chrlength,chromosome_iit,chrnum,circular_typeint);
	/* chrhigh += 1; */
#else
	j = 1;
#ifdef NO_EXTENSIONS_BEFORE_ZERO
	goal = last_diagonal - querylength + 1;
#else
	goal = last_diagonal + 1;
#endif
	while (j < nchromosomes_local && chrhighs_local[j] < goal) {
	  j <<= 1;			/* gallop by 2 */
	}
	if (j >= nchromosomes_local) {
	  j = binary_search(j >> 1,nchromosomes_local,chrhighs_local,goal);
	} else {
	  j = binary_search(j >> 1,j,chrhighs_local,goal);
	}
	chrnum += j;
#ifdef DEBUG15
	if (chrnum != Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength)) {
	  fprintf(stderr,"Got chrnum %d, but wanted %d\n",
		  chrnum,Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength));
	  abort();
	}
#endif
	chroffset = chroffsets[chrnum-1];
	chrhigh = chrhighs[chrnum-1];
	chrlength = chrlengths[chrnum-1];
	chrhighs_local += j;
	nchromosomes_local -= j;
#endif
      }
      if (last_diagonal <= chrhigh) { /* FORMULA for high position */
	/* position of high end is within current chromosome */
	debug1(printf("  => multiple_mm, diagonal %lu, query %d..%d, chrbounds %lu..%lu, floor %d, floor_xfirst %d, floor_xlast %d, floor_left %d, floor_right %d\n",
		      last_diagonal,first_querypos,last_querypos,chroffset,chrhigh,floor,floor_xfirst,floor_xlast,floor_left,floor_right));

	/* Save segment, but first advance splicesites past segment_left */
	segment_left = last_diagonal - querylength;
	max_distance = overall_max_distance;
	if (splicesites_local[0] >= last_diagonal) {
	  ptr->splicesites_i = -1;
	} else if (Splicetrie_splicesite_p(segment_left,/*pos5*/1,/*pos3*/querylength) == false) {
	  ptr->splicesites_i = -1;
	} else {
	  if (splicesites_local[0] < segment_left) {
	    j = 1;
	    while (j < nsplicesites_local && splicesites_local[j] < segment_left) {
	      j <<= 1;		/* gallop by 2 */
	    }
	    if (j >= nsplicesites_local) {
	      j = binary_search(j >> 1,nsplicesites_local,splicesites_local,segment_left);
	    } else {
	      j = binary_search(j >> 1,j,splicesites_local,segment_left);
	    }
	    joffset += j;
	    splicesites_local += j;
	    nsplicesites_local -= j;
	  }
	    
	  if (splicesites_local[0] >= last_diagonal) {
	    ptr->splicesites_i = -1;
	  } else {
	    ptr->splicesites_i = joffset;
	    j = joffset;
	    while (j < nsplicesites && splicesites[j] < last_diagonal) {
	      if (splicedists[j] > max_distance) {
		max_distance = splicedists[j];
	      }
	      j++;
	    }
	  }
	}

	/* Save segment */
	ptr->diagonal = last_diagonal;
	ptr->chrnum = chrnum;
	ptr->chroffset = chroffset;
	ptr->chrhigh = chrhigh;
	ptr->chrlength = chrlength;
	ptr->querypos5 = first_querypos;
	ptr->querypos3 = last_querypos;
	ptr->floor = floor;
	ptr->floor_xfirst = floor_xfirst;
	ptr->floor_xlast = floor_xlast;
	ptr->floor_left = floor_left;
	ptr->floor_right = floor_right;
	ptr->leftmost = ptr->rightmost = -1;
	ptr->left_splice_p = ptr->right_splice_p = false;
	ptr->usedp = false;
#if 0
	ptr->leftspan = ptr->rightspan = -1;
#endif

#if 0
	/* Not doing this, because the max_distance test is already good enough */
	if (plusp) {
	  /* For plus-strand splicing, require segmenti->querypos3 < segmentj->querypos5,
	     so if segmenti->querypos3 is too high, then it is not spliceable */
	  if (last_querypos > query_lastpos) {
	    /* Not spliceable */
	  } else if (diagonal <= last_diagonal + max_distance) {
	    *ptr_spliceable++ = ptr;
	  }
	} else {
	  /* For minus-strand splicing, require segmenti->querypos5 > segmentj->querypos3,
	     so if segmenti->querypos5 is too low, then it is not spliceable */
	  if (first_querypos < index1part) {
	    /* Not spliceable */
	  } else if (diagonal <= last_diagonal + max_distance) {
	    *ptr_spliceable++ = ptr;
	  }
	}
#endif
	if (diagonal <= last_diagonal + max_distance) {
	  *ptr_spliceable++ = ptr;
	} else {
	  debug4s(printf("plus diagonal %u is not spliceable because next one is at %u\n",
			 last_diagonal,diagonal));
	}
	ptr++;
      }

      /* Prepare next diagonal */
      first_querypos = querypos;
      last_diagonal = diagonal;
      floor_incr = floors_from_neg3[first_querypos] /* floors->score[-index1interval][first_querypos] */;
      floor = floor_incr;
      floor_xlast = floor_incr;
      floor_xfirst = floors_from_xfirst[first_querypos];  /* floors->score[xfirst_from][first_querypos]; */

#ifdef OLD_FLOOR_ENDS
      if (querypos < halfquery_lastpos) {
	floor_left = floor_incr;
      } else {
	floor_left = floors->scorefrom[-index1interval][halfquery_lastpos];
      }
      if (querypos < halfquerylength) {
	floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos];
      } else {
	floor_right = floors->scorefrom[halfquerylength-index1interval][first_querypos];
      }
#else
      floor_left = floor_incr;
#ifdef DEBUG1
      floor_right = -99;	/* For debugging output */
#endif
#endif

      debug1(printf("*multiple_mm_%s, diagonal %lu, querypos %d\n",plusp ? "plus" : "minus",diagonal,querypos));
      debug1(printf("start of diagonal %lu, first_querypos = %d => initial values: floor %d, floor_xfirst %d, floor_xlast %d, floor_left %d, floor_right %d\n",
		    diagonal,first_querypos,floor,floor_xfirst,floor_xlast,floor_left,floor_right));
    }
    last_querypos = querypos;


    if (--batch->npositions <= 0) {
      /* Use last entry in heap for insertion */
      batch = heap[heapsize];
      querypos = batch->querypos;
      heap[heapsize--] = sentinel;

    } else {
      /* Use this batch for insertion (same querypos) */
#ifdef WORDS_BIGENDIAN
      batch->diagonal = Bigendian_convert_univcoord(*(++batch->positions)) + batch->diagterm;
#else
      batch->diagonal = *(++batch->positions) + batch->diagterm;
#endif
#ifdef DIAGONAL_ADD_QUERYPOS
      batch->diagonal_add_querypos = (UINT8) batch->diagonal;
      batch->diagonal_add_querypos <<= 32;
      batch->diagonal_add_querypos |= querypos /* Previously added 2 because querypos was -2: + 2*/;
#endif
    }

    /* heapify */
    parenti = 1;
#ifdef DIAGONAL_ADD_QUERYPOS
    diagonal_add_querypos = batch->diagonal_add_querypos;
    smallesti = (heap[3]->diagonal_add_querypos < heap[2]->diagonal_add_querypos) ? 3 : 2;
    while (diagonal_add_querypos > heap[smallesti]->diagonal_add_querypos) {
      heap[parenti] = heap[smallesti];
      parenti = smallesti;
      smallesti = LEFT(parenti);
      righti = smallesti+1;
      if (heap[righti]->diagonal_add_querypos < heap[smallesti]->diagonal_add_querypos) {
	smallesti = righti;
      }
    }
#else
    diagonal = batch->diagonal;
    smallesti = ((heap[3]->diagonal < heap[2]->diagonal) ||
		 ((heap[3]->diagonal == heap[2]->diagonal) &&
		  (heap[3]->querypos < heap[2]->querypos))) ? 3 : 2;
    /* Note that diagonal/querypos will never exceed a sentinel diagonal/querypos */
    while (diagonal > heap[smallesti]->diagonal ||
	   (diagonal == heap[smallesti]->diagonal &&
	    querypos > heap[smallesti]->querypos)) {
      heap[parenti] = heap[smallesti];
      parenti = smallesti;
      smallesti = LEFT(parenti);
      righti = smallesti+1;
      if ((heap[righti]->diagonal < heap[smallesti]->diagonal) ||
	  ((heap[righti]->diagonal == heap[smallesti]->diagonal) &&
	   (heap[righti]->querypos < heap[smallesti]->querypos))) {
	smallesti = righti;
      }
    }
#endif
    heap[parenti] = batch;
  }
  debug14(printf("diagonal = %u, querypos = %d\n",last_diagonal,last_querypos));
  debug14(printf("\n"));

  /* Terminate loop. */
  floor_incr = floors_to_pos3[last_querypos];   /* floors->score[last_querypos][query_lastpos+index1interval]; */
  floor += floor_incr;
  floor_xfirst += floor_incr;
  floor_xlast += floors_to_xlast[last_querypos];  /* floors->score[last_querypos][xlast_to]; */

#ifdef OLD_FLOOR_ENDS
  if (last_querypos < halfquery_lastpos) {
    floor_left += floors->scorefrom[last_querypos][halfquery_lastpos+index1interval];
    floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos+index1interval];
  }
  if (last_querypos >= halfquerylength) {
    floor_right += floor_incr;
  }
#else
  floor_right = floor_incr;
#endif
  
  debug1(printf("no more diagonals: last_querypos = %d => terminal values: floor %d, floor_xfirst %d, floor_xlast %d, floor_left %d, floor_right %d\n",
		last_querypos,floor,floor_xfirst,floor_xlast,floor_left,floor_right));

  debug1(printf("last_diagonal %u vs chrhigh %u (looking for >)\n",last_diagonal,chrhigh));
  if (last_diagonal > chrhigh) {
    if (ptr > ptr_chrstart) {
      /* Add chr marker segment */
      debug1(printf("=== ptr %p > ptr_chrstart %p, so adding chr marker segment\n",ptr,ptr_chrstart));
      ptr->diagonal = (Univcoord_T) -1;
      ptr_chrstart = ++ptr;
    }

    /* update chromosome bounds, based on low end */
#ifdef SLOW_CHR_UPDATE
    chrnum = Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength);
    Univ_IIT_interval_bounds(&chroffset,&chrhigh,&chrlength,chromosome_iit,chrnum,circular_typeint);
    /* chrhigh += 1; */
#else
    j = 1;
#ifdef NO_EXTENSIONS_BEFORE_ZERO
    goal = last_diagonal - querylength + 1;
#else
    goal = last_diagonal + 1;
#endif
    while (j < nchromosomes_local && chrhighs_local[j] < goal) {
      j <<= 1;			/* gallop by 2 */
    }
    if (j >= nchromosomes_local) {
      j = binary_search(j >> 1,nchromosomes_local,chrhighs_local,goal);
    } else {
      j = binary_search(j >> 1,j,chrhighs_local,goal);
    }
    chrnum += j;
#ifdef DEBUG15
    if (chrnum != Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength)) {
      fprintf(stderr,"Got chrnum %d, but wanted %d\n",
	      chrnum,Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength));
      abort();
    }
#endif
    chroffset = chroffsets[chrnum-1];
    chrhigh = chrhighs[chrnum-1];
    chrlength = chrlengths[chrnum-1];
    chrhighs_local += j;
    nchromosomes_local -= j;
#endif
  }

  debug1(printf("last_diagonal %u vs chrhigh %u (looking for <=)\n",last_diagonal,chrhigh));
  if (last_diagonal <= chrhigh) { /* FORMULA for high position */
    /* position of high end is within current chromosome */
    debug1(printf("  => multiple_mm, diagonal %lu, query %d..%d, chrbounds %lu..%lu, floor %d, floor_xfirst %d, floor_xlast %d, floor_left %d, floor_right %d\n",
		  last_diagonal,first_querypos,last_querypos,chroffset,chrhigh,floor,floor_xfirst,floor_xlast,floor_left,floor_right));

    /* Save segment, but first advance splicesites past segment_left */
    segment_left = last_diagonal - querylength;
#if 0
    /* Last segment is not spliceable */
    max_distance = overall_max_distance;
#endif
    if (splicesites_local[0] >= last_diagonal) {
      ptr->splicesites_i = -1;
    } else if (Splicetrie_splicesite_p(segment_left,/*pos5*/1,/*pos3*/querylength) == false) {
      ptr->splicesites_i = -1;
    } else {
      if (splicesites_local[0] < segment_left) {
	j = 1;
	while (j < nsplicesites_local && splicesites_local[j] < segment_left) {
	  j <<= 1;		/* gallop by 2 */
	}
	if (j >= nsplicesites_local) {
	  j = binary_search(j >> 1,nsplicesites_local,splicesites_local,segment_left);
	} else {
	  j = binary_search(j >> 1,j,splicesites_local,segment_left);
	}
	joffset += j;
	splicesites_local += j;
	nsplicesites_local -= j;
      }

      if (splicesites_local[0] >= last_diagonal) {
	ptr->splicesites_i = -1;
      } else {
	ptr->splicesites_i = joffset;
#if 0
	/* Last segment is not spliceable */
	if (splicedists[joffset] > overall_max_distance) {
	  max_distance = splicedists[joffset];
	}
#endif
      }
    }

    /* Save segment */
    ptr->diagonal = last_diagonal;
    ptr->chrnum = chrnum;
    ptr->chroffset = chroffset;
    ptr->chrhigh = chrhigh;
    ptr->chrlength = chrlength;
    ptr->querypos5 = first_querypos;
    ptr->querypos3 = last_querypos;
    ptr->floor = floor;
    ptr->floor_xfirst = floor_xfirst;
    ptr->floor_xlast = floor_xlast;
    ptr->floor_left = floor_left;
    ptr->floor_right = floor_right;
    ptr->leftmost = ptr->rightmost = -1;
    ptr->left_splice_p = ptr->right_splice_p = false;
    ptr->usedp = false;
#if 0
    ptr->leftspan = ptr->rightspan = -1;
#endif
    /* Last segment is not spliceable */
    ptr++;
  }


  if (ptr > ptr_chrstart) {
    /* Final chr marker segment */
    debug1(printf("=== ptr %p > ptr_chrstart %p, so adding final chr marker segment\n",ptr,ptr_chrstart));
    ptr->diagonal = (Univcoord_T) -1;
    /* ptr_chrstart = */ ++ptr;
  }

#ifdef DEBUG19
  for (k = 0, ptr0 = segments; ptr0 < ptr; k++, ptr0++) {
    printf("%d %lu\n",k,ptr0->diagonal);
  }
  printf("total_npositions = %d, nchromosomes = %d\n",total_npositions,nchromosomes);
#endif

  FREE(heap);
  FREE(batchpool);

  /* Note: segments is in descending diagonal order.  Will need to
     reverse before solving middle deletions */

  *nsegments = ptr - segments;
  *nspliceable = ptr_spliceable - *spliceable;
  debug(printf("nsegments = %d (total_npositions = %d, nchromosomes = %d)\n",
	       *nsegments,total_npositions,nchromosomes));
  debug1(printf("nsegments = %d (total_npositions = %d, nchromosomes = %d)\n",
		*nsegments,total_npositions,nchromosomes));

  assert(*nsegments <= total_npositions + nchromosomes);

  if (splicesites == NULL) {
    FREE(splicesites_local);
  }

  return segments;
}


/* Specialized version of identify_all_segments that stores only floor_left and floor_right */
static struct Segment_T *
identify_all_segments_for_terminals (int *nsegments, Univcoord_T **positions, int *npositions,
				     bool *omitted, int querylength, int query_lastpos,
				     Floors_T floors, int max_mismatches_allowed, bool plusp) {
  struct Segment_T *segments = NULL;
  Batch_T batch, sentinel;
  struct Batch_T sentinel_struct, *batchpool;
  Batch_T *heap;
  int heapsize = 0;
  int parenti, smallesti, righti, i;
  int querypos, first_querypos, last_querypos;
  int floor_left, floor_right, floor_incr;
  int *floors_from_neg3, *floors_from_xfirst, *floors_to_xlast, *floors_to_pos3;
  /* int exclude_xfirst, exclude_xlast; */
  Univcoord_T diagonal, last_diagonal, chroffset = 0U, chrhigh = 0U;
  Chrpos_T chrlength;
  Chrnum_T chrnum = 1;
#ifdef OLD_FLOOR_ENDS
  int halfquerylength, halfquery_lastpos;
#endif
#ifdef DIAGONAL_ADD_QUERYPOS
  UINT8 diagonal_add_querypos;
#endif
  int total_npositions = 0;
  Segment_T ptr, ptr_chrstart;
#ifndef SLOW_CHR_UPDATE
  Univcoord_T goal;
  int j, nchromosomes_local = nchromosomes;
  Univcoord_T *chrhighs_local = chrhighs;
#endif

  debug(printf("*** Starting identify_all_segments ***\n"));

#ifdef OLD_FLOOR_ENDS
  halfquerylength = querylength / 2;
  halfquery_lastpos = halfquerylength - index1part;
#endif

  /* Create sentinel */
#ifdef DIAGONAL_ADD_QUERYPOS
  sentinel_struct.diagonal_add_querypos = (UINT8) -1; /* infinity */
  sentinel_struct.diagonal_add_querypos <<= 32;
#else
  sentinel_struct.querypos = querylength; /* essentially infinity */
  sentinel_struct.diagonal = (Univcoord_T) -1; /* infinity */
#endif
  sentinel = &sentinel_struct;

  /* Set up batches */
  batchpool = (struct Batch_T *) CALLOC(query_lastpos+1,sizeof(struct Batch_T));
  heap = (Batch_T *) CALLOC(2*(query_lastpos+1)+1+1,sizeof(Batch_T));

  /* Don't add entries for compoundpos positions (skip querypos -2, -1, lastpos+1, lastpos+2) */
  if (plusp) {
    for (querypos = 0, i = 0; querypos <= query_lastpos; querypos++) {
      if (omitted[querypos] == true) {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      } else if (npositions[querypos] > 0) {
	debug1(printf("Adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
	batch = &(batchpool[i]);
	Batch_init(batch,querypos,/*diagterm*/querylength - querypos,positions[querypos],npositions[querypos],querylength);
	total_npositions += npositions[querypos];
	if (batch->npositions > 0) {
	  min_heap_insert(heap,&heapsize,batch);
	  i++;
	}
      } else {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      }
    }
  } else {
    for (querypos = 0, i = 0; querypos <= query_lastpos; querypos++) {
      if (omitted[querypos] == true) {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      } else if (npositions[querypos] > 0) {
	debug1(printf("Adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
	batch = &(batchpool[i]);
	Batch_init(batch,querypos,/*diagterm*/querypos + index1part,positions[querypos],npositions[querypos],querylength);
	total_npositions += npositions[querypos];
	if (batch->npositions > 0) {
	  min_heap_insert(heap,&heapsize,batch);
	  i++;
	}
      } else {
	debug1(printf("Not adding batch for querypos %d with %d positions, omitted %d\n",
		      querypos,npositions[querypos],omitted[querypos]));
      }
    }
  }


  if (i == 0) {
    FREE(heap);
    FREE(batchpool);
    *nsegments = 0;
    return (struct Segment_T *) NULL;
  }

  /* Set up rest of heap */
  for (i = heapsize+1; i <= 2*heapsize+1; i++) {
    heap[i] = sentinel;
  }

  /* Putting chr marker "segments" after each chromosome */
  segments = (struct Segment_T *) CALLOC(total_npositions + nchromosomes,sizeof(struct Segment_T));
  ptr_chrstart = ptr = &(segments[0]);

  /*
  if ((exclude_xfirst = firstbound-2-index1part-max_end_insertions) < index1interval) {
    exclude_xfirst = index1interval;
  }
  if ((exclude_xlast = lastbound+1+max_end_insertions) > query_lastpos-index1interval) {
    exclude_xlast = query_lastpos-index1interval;
  }
  */

#if 0
  /* Should account for firstbound and lastbound */
  floors_from_xfirst = floors->scorefrom[/* xfirst_from = */ firstbound-index1interval+max_end_insertions];
  floors_to_xlast = floors->scoreto[/* xlast_to = */ lastbound+1+index1interval-index1part-max_end_insertions];
#else
  if (spansize /* +max_end_insertions */ > query_lastpos + index1interval) {
    floors_from_xfirst = floors->scorefrom[query_lastpos+index1interval];
  } else {
    floors_from_xfirst = floors->scorefrom[spansize /* +max_end_insertions */];
  }
  if (query_lastpos-spansize /* -max_end_insertions */ < -index1interval) {
    floors_to_xlast = floors->scoreto[-index1interval];
  } else {
    floors_to_xlast = floors->scoreto[query_lastpos-spansize /* -max_end_insertions */];
  }
#endif
  floors_from_neg3 = floors->scorefrom[-index1interval];
  floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];


  /* Initialize loop */
  batch = heap[1];
  first_querypos = last_querypos = querypos = batch->querypos;
  last_diagonal = diagonal = batch->diagonal;

  floor_incr = floors_from_neg3[first_querypos];
#if 0
  floor = floor_incr;
  floor_xlast = floor_incr;
  floor_xfirst = floors_from_xfirst[first_querypos] /* floors->scorefrom[xfirst_from][first_querypos] */;
#endif

#ifdef OLD_FLOOR_ENDS
  if (querypos < halfquery_lastpos) {
    floor_left = floor_incr;
  } else {
    floor_left = floors->scorefrom[-index1interval][halfquery_lastpos];
  }
  if (querypos < halfquerylength) {
    floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos];
  } else {
    floor_right = floors->scorefrom[halfquerylength-index1interval][first_querypos];
  }
#else
  floor_left = floor_incr;
#ifdef DEBUG1
  floor_right = -99;
#endif
#endif


  debug1(printf("multiple_mm_%s, diagonal %lu, querypos %d\n",plusp ? "plus" : "minus",diagonal,querypos));
  debug1(printf("first_querypos = %d => initial values: floor_left %d, floor_right %d\n",
	        first_querypos,floor_left,floor_right));

  if (--batch->npositions <= 0) {
    /* Use last entry in heap for insertion */
    batch = heap[heapsize];
    querypos = batch->querypos;
    heap[heapsize--] = sentinel;

  } else {
    /* Use this batch for insertion (same querypos) */
#ifdef WORDS_BIGENDIAN
    batch->diagonal = Bigendian_convert_univcoord(*(++batch->positions)) + batch->diagterm;
#else
    batch->diagonal = *(++batch->positions) + batch->diagterm;
#endif
#ifdef DIAGONAL_ADD_QUERYPOS
    batch->diagonal_add_querypos = (UINT8) batch->diagonal;
    batch->diagonal_add_querypos <<= 32;
    batch->diagonal_add_querypos |= querypos /* Previously added 2 because querypos was -2: + 2*/;
#endif
  }

  /* heapify */
  parenti = 1;
#ifdef DIAGONAL_ADD_QUERYPOS
  diagonal_add_querypos = batch->diagonal_add_querypos;
  smallesti = (heap[3]->diagonal_add_querypos < heap[2]->diagonal_add_querypos) ? 3 : 2;
  while (diagonal_add_querypos > heap[smallesti]->diagonal_add_querypos) {
    heap[parenti] = heap[smallesti];
    parenti = smallesti;
    smallesti = LEFT(parenti);
    righti = smallesti+1;
    if (heap[righti]->diagonal_add_querypos < heap[smallesti]->diagonal_add_querypos) {
      smallesti = righti;
    }
  }
#else
  diagonal = batch->diagonal;
  smallesti = ((heap[3]->diagonal < heap[2]->diagonal) ||
	       ((heap[3]->diagonal == heap[2]->diagonal) &&
		(heap[3]->querypos < heap[2]->querypos))) ? 3 : 2;
  /* Note that diagonal/querypos will never exceed a sentinel diagonal/querypos */
  while (diagonal > heap[smallesti]->diagonal ||
	 (diagonal == heap[smallesti]->diagonal &&
	  querypos > heap[smallesti]->querypos)) {
    heap[parenti] = heap[smallesti];
    parenti = smallesti;
    smallesti = LEFT(parenti);
    righti = smallesti+1;
    if ((heap[righti]->diagonal < heap[smallesti]->diagonal) ||
		  ((heap[righti]->diagonal == heap[smallesti]->diagonal) &&
		   (heap[righti]->querypos < heap[smallesti]->querypos))) {
      smallesti = righti;
    }
  }
#endif
  heap[parenti] = batch;


  /* Continue after initialization */
  while (heapsize > 0) {
    batch = heap[1];
    querypos = batch->querypos;
    diagonal = batch->diagonal;

    if (diagonal == last_diagonal) {
      /* Continuing exact match or substitution */
      floor_incr = floors->scorefrom[last_querypos][querypos];
#if 0
      floor += floor_incr;
      floor_xfirst += floor_incr;
      floor_xlast += floor_incr;
#endif

#ifdef OLD_FLOOR_ENDS
      /* Why is this here?  Just set floor_left at start and floor_right at end. */
      if (querypos < halfquery_lastpos) {
	floor_left += floor_incr;
      } else if (last_querypos < halfquery_lastpos) {
	/* Finish floor_left */
	floor_left += floors->scorefrom[last_querypos][halfquery_lastpos+index1interval];
      }
      if (querypos >= halfquerylength) {
	if (last_querypos < halfquerylength) {
	  /* Start floor_right */
	  floor_right = floors->scorefrom[halfquerylength-index1interval][querypos];
	} else {
	  floor_right += floor_incr;
	}
      }
#endif

      debug1(printf("diagonal %lu unchanged: last_querypos = %d, querypos = %d => floor increments by %d\n",
		    diagonal,last_querypos,querypos,floor_incr));
      debug1(printf("*multiple_mm_%s, diagonal %lu, querypos %d, floor_left %d, floor_right %d\n",
		    plusp ? "plus" : "minus",diagonal,querypos,floor_left,floor_right));
    } else {
      /* End of diagonal */
      floor_incr = floors_to_pos3[last_querypos]  /* floors->score[last_querypos][query_lastpos+index1interval] */;
#if 0
      floor += floor_incr;
      floor_xfirst += floor_incr;
      floor_xlast += floors_to_xlast[last_querypos];  /* floors->score[last_querypos][xlast_to]; */
#endif

#ifdef OLD_FLOOR_ENDS
      if (last_querypos < halfquery_lastpos) {
	floor_left += floors->scorefrom[last_querypos][halfquery_lastpos+index1interval];
	floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos+index1interval];
      }
      if (last_querypos >= halfquerylength) {
	floor_right += floor_incr;
      }
#else
      floor_right = floor_incr;
#endif

      debug1(printf("new diagonal %lu > last diagonal %lu: last_querypos = %d => final values: floor_left %d, floor_right %d\n",
		    diagonal,last_diagonal,last_querypos,floor_left,floor_right));

      if (last_diagonal > chrhigh) {
	if (ptr > ptr_chrstart) {
	  /* Add chr marker segment */
	  debug1(printf("=== ptr %p > ptr_chrstart %p, so adding chr marker segment\n",ptr,ptr_chrstart));
	  ptr->diagonal = (Univcoord_T) -1;
	  ptr_chrstart = ++ptr;
	}

	/* update chromosome bounds, based on low end */
#ifdef SLOW_CHR_UPDATE
	chrnum = Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength);
	Univ_IIT_interval_bounds(&chroffset,&chrhigh,&chrlength,chromosome_iit,chrnum,circular_typeint);
	/* chrhigh += 1; */
#else
	j = 1;
#ifdef NO_EXTENSIONS_BEFORE_ZERO
	goal = last_diagonal - querylength + 1;
#else
	goal = last_diagonal + 1;
#endif
	while (j < nchromosomes_local && chrhighs_local[j] < goal) {
	  j <<= 1;			/* gallop by 2 */
	}
	if (j >= nchromosomes_local) {
	  j = binary_search(j >> 1,nchromosomes_local,chrhighs_local,goal);
	} else {
	  j = binary_search(j >> 1,j,chrhighs_local,goal);
	}
	chrnum += j;
#ifdef DEBUG15
	if (chrnum != Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength)) {
	  fprintf(stderr,"Got chrnum %d, but wanted %d\n",
		  chrnum,Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength));
	  abort();
	}
#endif
	chroffset = chroffsets[chrnum-1];
	chrhigh = chrhighs[chrnum-1];
	chrlength = chrlengths[chrnum-1];
	chrhighs_local += j;
	nchromosomes_local -= j;
#endif
      }
      if (last_diagonal <= chrhigh) { /* FORMULA for high position */
	/* position of high end is within current chromosome */
	debug1(printf("  => multiple_mm, diagonal %lu, query %d..%d, chrbounds %lu..%lu, floor_left %d, floor_right %d\n",
		      last_diagonal,first_querypos,last_querypos,chroffset,chrhigh,floor_left,floor_right));
	if (floor_left <= max_mismatches_allowed || floor_right <= max_mismatches_allowed) {
	  /* Save segment */
	  ptr->diagonal = last_diagonal;
	  ptr->chrnum = chrnum;
	  ptr->chroffset = chroffset;
	  ptr->chrhigh = chrhigh;
	  ptr->chrlength = chrlength;
	  ptr->querypos5 = first_querypos;
	  ptr->querypos3 = last_querypos;
#if 0
	  ptr->floor = floor;
	  ptr->floor_xfirst = floor_xfirst;
	  ptr->floor_xlast = floor_xlast;
#endif
	  ptr->floor_left = floor_left;
	  ptr->floor_right = floor_right;
#if 0
	  ptr->leftmost = ptr->rightmost = -1;
	  ptr->left_splice_p = ptr->right_splice_p = false;
	  ptr->leftspan = ptr->rightspan = -1;
#endif
	  ptr->usedp = false;
	  ptr++;
	}
      }

      /* Prepare next diagonal */
      first_querypos = querypos;
      last_diagonal = diagonal;
      floor_incr = floors_from_neg3[first_querypos] /* floors->score[-index1interval][first_querypos] */;
#if 0
      floor = floor_incr;
      floor_xlast = floor_incr;
      floor_xfirst = floors_from_xfirst[first_querypos];  /* floors->score[xfirst_from][first_querypos]; */
#endif

#ifdef OLD_FLOOR_ENDS
      if (querypos < halfquery_lastpos) {
	floor_left = floor_incr;
      } else {
	floor_left = floors->scorefrom[-index1interval][halfquery_lastpos];
      }
      if (querypos < halfquerylength) {
	floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos];
      } else {
	floor_right = floors->scorefrom[halfquerylength-index1interval][first_querypos];
      }
#else
      floor_left = floor_incr;
#ifdef DEBUG1
      floor_right = -99;
#endif
#endif

      debug1(printf("*multiple_mm_%s, diagonal %lu, querypos %d\n",plusp ? "plus" : "minus",diagonal,querypos));
      debug1(printf("start of diagonal %lu, first_querypos = %d => initial values: floor_left %d, floor_right %d\n",
		    diagonal,first_querypos,floor_left,floor_right));
    }
    last_querypos = querypos;


    if (--batch->npositions <= 0) {
      /* Use last entry in heap for insertion */
      batch = heap[heapsize];
      querypos = batch->querypos;
      heap[heapsize--] = sentinel;

    } else {
      /* Use this batch for insertion (same querypos) */
#ifdef WORDS_BIGENDIAN
      batch->diagonal = Bigendian_convert_univcoord(*(++batch->positions)) + batch->diagterm;
#else
      batch->diagonal = *(++batch->positions) + batch->diagterm;
#endif
#ifdef DIAGONAL_ADD_QUERYPOS
      batch->diagonal_add_querypos = (UINT8) batch->diagonal;
      batch->diagonal_add_querypos <<= 32;
      batch->diagonal_add_querypos |= querypos /* Previously added 2 because querypos was -2: + 2*/;
#endif
    }

    /* heapify */
    parenti = 1;
#ifdef DIAGONAL_ADD_QUERYPOS
    diagonal_add_querypos = batch->diagonal_add_querypos;
    smallesti = (heap[3]->diagonal_add_querypos < heap[2]->diagonal_add_querypos) ? 3 : 2;
    while (diagonal_add_querypos > heap[smallesti]->diagonal_add_querypos) {
      heap[parenti] = heap[smallesti];
      parenti = smallesti;
      smallesti = LEFT(parenti);
      righti = smallesti+1;
      if (heap[righti]->diagonal_add_querypos < heap[smallesti]->diagonal_add_querypos) {
	smallesti = righti;
      }
    }
#else
    diagonal = batch->diagonal;
    smallesti = ((heap[3]->diagonal < heap[2]->diagonal) ||
		 ((heap[3]->diagonal == heap[2]->diagonal) &&
		  (heap[3]->querypos < heap[2]->querypos))) ? 3 : 2;
    /* Note that diagonal/querypos will never exceed a sentinel diagonal/querypos */
    while (diagonal > heap[smallesti]->diagonal ||
	   (diagonal == heap[smallesti]->diagonal &&
	    querypos > heap[smallesti]->querypos)) {
      heap[parenti] = heap[smallesti];
      parenti = smallesti;
      smallesti = LEFT(parenti);
      righti = smallesti+1;
      if ((heap[righti]->diagonal < heap[smallesti]->diagonal) ||
	  ((heap[righti]->diagonal == heap[smallesti]->diagonal) &&
	   (heap[righti]->querypos < heap[smallesti]->querypos))) {
	smallesti = righti;
      }
    }
#endif
    heap[parenti] = batch;
  }

  /* Terminate loop. */
  floor_incr = floors_to_pos3[last_querypos];   /* floors->score[last_querypos][query_lastpos+index1interval]; */
#if 0
  floor += floor_incr;
  floor_xfirst += floor_incr;
  floor_xlast += floors_to_xlast[last_querypos];  /* floors->score[last_querypos][xlast_to]; */
#endif

#ifdef OLD_FLOOR_ENDS
  if (last_querypos < halfquery_lastpos) {
    floor_left += floors->scorefrom[last_querypos][halfquery_lastpos+index1interval];
    floor_right = floors->scorefrom[halfquerylength-index1interval][query_lastpos+index1interval];
  }
  if (last_querypos >= halfquerylength) {
    floor_right += floor_incr;
  }
#else
  floor_right = floor_incr;
#endif

  
  debug1(printf("no more diagonals: last_querypos = %d => terminal values: floor_left %d, floor_right %d\n",
		last_querypos,floor_left,floor_right));

  if (last_diagonal > chrhigh) {
    if (ptr > ptr_chrstart) {
      /* Add chr marker segment */
      debug1(printf("=== ptr %p > ptr_chrstart %p, so adding chr marker segment\n",ptr,ptr_chrstart));
      ptr->diagonal = (Univcoord_T) -1;
      ptr_chrstart = ++ptr;
    }

    /* update chromosome bounds, based on low end */
#ifdef SLOW_CHR_UPDATE
    chrnum = Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength);
    Univ_IIT_interval_bounds(&chroffset,&chrhigh,&chrlength,chromosome_iit,chrnum,circular_typeint);
    /* chrhigh += 1; */
#else
    j = 1;
#ifdef NO_EXTENSIONS_BEFORE_ZERO
    goal = last_diagonal - querylength + 1;
#else
    goal = last_diagonal + 1;
#endif
    while (j < nchromosomes_local && chrhighs_local[j] < goal) {
      j <<= 1;			/* gallop by 2 */
    }
    if (j >= nchromosomes_local) {
      j = binary_search(j >> 1,nchromosomes_local,chrhighs_local,goal);
    } else {
      j = binary_search(j >> 1,j,chrhighs_local,goal);
    }
    chrnum += j;
#ifdef DEBUG15
    if (chrnum != Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength)) {
      fprintf(stderr,"Got chrnum %d, but wanted %d\n",
	      chrnum,Univ_IIT_get_one(chromosome_iit,last_diagonal-querylength,last_diagonal-querylength));
      abort();
    }
#endif
    chroffset = chroffsets[chrnum-1];
    chrhigh = chrhighs[chrnum-1];
    chrlength = chrlengths[chrnum-1];
    chrhighs_local += j;
    nchromosomes_local -= j;
#endif
  }
  if (last_diagonal <= chrhigh) { /* FORMULA for high position */
    /* position of high end is within current chromosome */
    debug1(printf("  => multiple_mm, diagonal %lu, query %d..%d, chrbounds %lu..%lu, floor_left %d, floor_right %d\n",
		  last_diagonal,first_querypos,last_querypos,chroffset,chrhigh,floor_left,floor_right));
    if (floor_left <= max_mismatches_allowed || floor_right <= max_mismatches_allowed) {
      /* Save segment */
      ptr->diagonal = last_diagonal;
      ptr->chrnum = chrnum;
      ptr->chroffset = chroffset;
      ptr->chrhigh = chrhigh;
      ptr->chrlength = chrlength;
      ptr->querypos5 = first_querypos;
      ptr->querypos3 = last_querypos;
#if 0
      ptr->floor = floor;
      ptr->floor_xfirst = floor_xfirst;
      ptr->floor_xlast = floor_xlast;
#endif
      ptr->floor_left = floor_left;
      ptr->floor_right = floor_right;
#if 0
      ptr->leftmost = ptr->rightmost = -1;
      ptr->left_splice_p = ptr->right_splice_p = false;
      ptr->leftspan = ptr->rightspan = -1;
#endif
      ptr->usedp = false;
      ptr++;
    }
  }

  if (ptr > ptr_chrstart) {
    /* Final chr marker segment */
    debug1(printf("=== ptr %p > ptr_chrstart %p, so adding final chr marker segment\n",ptr,ptr_chrstart));
    ptr->diagonal = (Univcoord_T) -1;
    /* ptr_chrstart = */ ++ptr;
  }


  FREE(heap);
  FREE(batchpool);

  /* Note: segments is in descending diagonal order.  Will need to
     reverse before solving middle deletions */

  *nsegments = ptr - segments;
  debug1(printf("nsegments = %d\n",*nsegments));
  debug(printf("nsegments = %d (total_npositions = %d)\n",*nsegments,total_npositions));

  assert(*nsegments <= total_npositions + nchromosomes);

  return segments;
}



/*

The pattern below is a middle insertion on plus strand, or middle deletion on minus strand:

diagonal 2354, querypos 18
diagonal 2354, querypos 19
diagonal 2354, querypos 20
diagonal 2354, querypos 21
diagonal 2354, querypos 22
diagonal 2354, querypos 23
diagonal 2354, querypos 24
diagonal 2356, querypos 0
diagonal 2356, querypos 1
diagonal 2356, querypos 2
diagonal 2356, querypos 3
diagonal 2356, querypos 4
diagonal 2356, querypos 5


The pattern below is a middle deletion on plus strand, or middle insertion on minus strand:

diagonal 2354, querypos 0
diagonal 2354, querypos 1
diagonal 2354, querypos 2
diagonal 2354, querypos 3
diagonal 2354, querypos 4
diagonal 2354, querypos 5
diagonal 2354, querypos 6
diagonal 2354, querypos 7
diagonal 2356, querypos 18
diagonal 2356, querypos 19
diagonal 2356, querypos 20
diagonal 2356, querypos 21
diagonal 2356, querypos 22
diagonal 2356, querypos 23
diagonal 2356, querypos 24

*/


static List_T
find_middle_indels (int *found_score, int *nhits, List_T hits,
		    Segment_T *plus_spliceable, int plus_nspliceable,
		    Segment_T *minus_spliceable, int minus_nspliceable,
#ifdef DEBUG2
		    char *queryuc_ptr, char *queryrc, 
#endif
		    Floors_T floors, int querylength, int query_lastpos,
		    Compress_T query_compress_fwd, Compress_T query_compress_rev,
		    int min_indel_end_matches,
		    int indel_penalty_middle, int max_mismatches_allowed, int genestrand) {
  Segment_T segmenti, segmentj, segmentj_end, *ptr;
  int indels, floor, pos, prev, middle;
  int *floors_from_neg3, *floors_to_pos3;
  bool foundp;


  debug(printf("*** find_middle_indels with querylength %d and max_mismatches_allowed %d ***\n",
	       querylength,max_mismatches_allowed));

  /* Plus segments */
  if (floors != NULL) {
    floors_from_neg3 = floors->scorefrom[-index1interval];
    floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

    for (ptr = plus_spliceable; ptr < &(plus_spliceable[plus_nspliceable]); ptr++) {
      segmenti = *ptr;
      debug2(printf("\nplus segmenti:  diagonal %lu, querypos %d..%d\n",
		    segmenti->diagonal,segmenti->querypos5,segmenti->querypos3));
      if (1 || segmenti->diagonal < (Univcoord_T) -1) { /* No markers were stored in spliceable */
	/* Identify potential segmentj for segmenti */
	segmentj_end = segmenti+1;
	while (
#ifdef NO_MARKER_SEGMENTS
	       segmentj_end < &(plus_segments[plus_nsegments]) && segmentj_end->chrnum == segmenti->chrnum &&
#endif
	       segmentj_end->diagonal <= segmenti->diagonal + max_middle_insertions) {
	  segmentj_end++;
	}
	  
	for (segmentj = segmenti+1; segmentj < segmentj_end; segmentj++) {
	  debug2(printf("plus insertion?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d => ",
			segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
			segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	  /* j5 j3 i5 i3 */
	  if (segmentj->querypos3 < segmenti->querypos5) {
	    indels = segmentj->diagonal - segmenti->diagonal; /* positive */
	    floor = floors_from_neg3[segmentj->querypos5] + floors_to_pos3[segmenti->querypos3]
	      /* floors->score[-index1interval][segmentj->querypos5] + floors->score[segmenti->querypos3][query_lastpos+index1interval] */ ;
	    if (floors->prev_omitted == NULL) {
	      if ((middle = FLOOR_MIDDLE(segmenti->querypos5 - segmentj->querypos3 - indels)) > 0) {
		middle--;	/* for insertion, which looks like a mismatch */
	      }
	      debug2(printf("\nmiddle (no omission): %d\n",middle));
	      floor += middle;
	    } else {
	      pos = segmenti->querypos5;
	      debug2(printf("\nmiddle (omission):"));
	      while (pos > segmentj->querypos3) {
		if ((prev = floors->prev_omitted[pos]) < segmentj->querypos3) {
		  prev = segmentj->querypos3;
		}
		if ((middle = FLOOR_MIDDLE(pos - prev - indels)) > 0) {
		  middle--;	/* for insertion, which looks like a mismatch */
		}
		floor += middle;
		debug2(printf("(%d..%d)+%d,",prev,pos,middle));
		pos = prev;
	      }
	      debug2(printf("\n"));
	    }
	    if (floor <= max_mismatches_allowed) {
	      debug2(printf("successful insertion, floor = %d+middle+%d=%d, indels = %d\n",
			    floors->scorefrom[-index1interval][segmentj->querypos5],
			    floors->scorefrom[segmenti->querypos3][query_lastpos+index1interval],
			    floor,indels));
	      hits = Indel_solve_middle_insertion(&foundp,&(*found_score),&(*nhits),hits,
						  /*left*/segmenti->diagonal - querylength,
						  segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
						  indels,/*query_compress*/query_compress_fwd,
#ifdef DEBUG2
						  /*queryptr*/queryuc_ptr,
#endif
						  querylength,max_mismatches_allowed,
						  /*plusp*/true,genestrand,/*sarrayp*/false);
	      if (foundp == true) {
		segmenti->usedp = segmentj->usedp = true;
	      }
	    } else {
	      debug2(printf("too many mismatches, because floor %d+middle+%d=%d > %d\n",
			    floors->scorefrom[-index1interval][segmentj->querypos5],
			    floors->scorefrom[segmenti->querypos3][query_lastpos+index1interval],
			    floor,max_mismatches_allowed));
	    }
	  } else {
	    debug2(printf("garbage, because querypos3 %d >= querypos5 %d\n",
			  segmentj->querypos3,segmenti->querypos5));
	  }
	}

	/* Identify potential segmentj for segmenti */
	segmentj_end = segmenti+1;
	while (
#ifdef NO_MARKER_SEGMENTS
	       segmentj_end < &(plus_segments[plus_nsegments]) && segmentj_end->chrnum == segmenti->chrnum &&
#endif
	       segmentj_end->diagonal <= segmenti->diagonal + max_middle_deletions) {
	  segmentj_end++;
	}

	for (segmentj = segmenti+1; segmentj < segmentj_end; segmentj++) {
	  debug2(printf("plus deletion?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d => ",
			segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
			segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	  /* i5 i3 j5 j3 */
	  if (segmenti->querypos3 < segmentj->querypos5) {
	    indels = segmenti->diagonal - segmentj->diagonal; /* negative */
	    floor = floors_from_neg3[segmenti->querypos5] + floors_to_pos3[segmentj->querypos3]
	      /* floors->score[-index1interval][segmenti->querypos5] + floors->score[segmentj->querypos3][query_lastpos+index1interval] */;
	    if (floors->prev_omitted == NULL) {
	      if ((middle = FLOOR_MIDDLE(segmentj->querypos5 - segmenti->querypos3 /*- indels*/)) > 0) {
		middle--;	/* for deletion, which looks like a mismatch */
	      }
	      debug2(printf("\nmiddle (no omission): %d\n",middle));
	      floor += middle;
	    } else {
	      pos = segmentj->querypos5;
	      debug2(printf("\nmiddle (omission):"));
	      while (pos > segmenti->querypos3) {
		if ((prev = floors->prev_omitted[pos]) < segmenti->querypos3) {
		  prev = segmenti->querypos3;
		}
		if ((middle = FLOOR_MIDDLE(pos - prev /*- indels*/)) > 0) {
		  middle--;	/* for deletion, which looks like a mismatch */
		}
		floor += middle;
		debug2(printf("(%d..%d)+%d,",prev,pos,middle));
		pos = prev;
	      }
	      debug2(printf("\n"));
	    }
	    if (floor <= max_mismatches_allowed) {
	      debug2(printf("successful deletion, floor = %d+middle+%d=%d, indels = %d\n",
			    floors->scorefrom[-index1interval][segmenti->querypos5],
			    floors->scorefrom[segmentj->querypos3][query_lastpos+index1interval],
			    floor,indels));
	      hits = Indel_solve_middle_deletion(&foundp,&(*found_score),&(*nhits),hits,
						 /*left*/segmenti->diagonal - querylength,
						 segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
						 indels,/*query_compress*/query_compress_fwd,
#ifdef DEBUG2
						 /*queryptr*/queryuc_ptr,
#endif
						 querylength,max_mismatches_allowed,/*plusp*/true,genestrand,/*sarrayp*/false);
	      if (foundp == true) {
		segmenti->usedp = segmentj->usedp = true;
	      }
	    } else {
	      debug2(printf("too many mismatches, because floor = %d+middle+%d=%d > %d\n",
			    floors->scorefrom[-index1interval][segmenti->querypos5],
			    floors->scorefrom[segmentj->querypos3][query_lastpos+index1interval],
			    floor,max_mismatches_allowed));
	    }
	  } else {
	    debug2(printf("garbage, because querypos3 %d >= querypos5 %d\n",
			  segmenti->querypos3,segmentj->querypos5));
	  }
	}
      }
    }

    /* Minus segments */
    floors_from_neg3 = floors->scorefrom[-index1interval];
    floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

    for (ptr = minus_spliceable; ptr < &(minus_spliceable[minus_nspliceable]); ptr++) {
      segmenti = *ptr;
      debug2(printf("\nminus segmenti:  diagonal %lu, querypos %d..%d\n",
		    segmenti->diagonal,segmenti->querypos5,segmenti->querypos3));
      if (1 || segmenti->diagonal < (Univcoord_T) -1) { /* No markers were stored in spliceable */
	/* Identify potential segmentj for segmenti */
	segmentj_end = segmenti+1;
	while (
#ifdef NO_MARKER_SEGMENTS
	       segmentj_end < &(minus_segments[minus_nsegments]) && segmentj_end->chrnum == segmenti->chrnum &&
#endif
	       segmentj_end->diagonal <= segmenti->diagonal + max_middle_deletions) {
	  segmentj_end++;
	}

	for (segmentj = segmenti+1; segmentj < segmentj_end; segmentj++) {
	  debug2(printf("minus deletion?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d => ",
			segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
			segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	  /* j5 j3 i5 i3 */
	  if (segmentj->querypos3 < segmenti->querypos5) {
	    indels = segmenti->diagonal - segmentj->diagonal; /* negative */
	    floor = floors_from_neg3[segmentj->querypos5] + floors_to_pos3[segmenti->querypos3]
	      /* floors->score[-index1interval][segmentj->querypos5] + floors->score[segmenti->querypos3][query_lastpos+index1interval] */;
	    if (floors->prev_omitted == NULL) {
	      if ((middle = FLOOR_MIDDLE(segmenti->querypos5 - segmentj->querypos3 /*- indels*/)) > 0) {
		middle--;	/* for deletion, which looks like a mismatch */
	      }
	      debug2(printf("\nmiddle (no omission): %d\n",middle));
	      floor += middle;
	    } else {
	      pos = segmenti->querypos5;
	      debug2(printf("\nmiddle (omission):"));
	      while (pos > segmentj->querypos3) {
		if ((prev = floors->prev_omitted[pos]) < segmentj->querypos3) {
		  prev = segmentj->querypos3;
		}
		if ((middle = FLOOR_MIDDLE(pos - prev /*- indels*/)) > 0) {
		  middle--; /* for deletion, which looks like a mismatch */
		}
		floor += middle;
		debug2(printf("(%d..%d)+%d,",prev,pos,middle));
		pos = prev;
	      }
	      debug2(printf("\n"));
	    }
	    if (floor <= max_mismatches_allowed) {
	      debug2(printf("successful deletion, floor = %d+middle+%d=%d, indels = %d\n",
			    floors->scorefrom[-index1interval][segmentj->querypos5],
			    floors->scorefrom[segmenti->querypos3][query_lastpos+index1interval],
			    floor,indels));
	      hits = Indel_solve_middle_deletion(&foundp,&(*found_score),&(*nhits),hits,
						 /*left*/segmenti->diagonal - querylength,
						 segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
						 indels,/*query_compress*/query_compress_rev,
#ifdef DEBUG2
						 /*queryptr*/queryrc,
#endif
						 querylength,max_mismatches_allowed,/*plusp*/false,genestrand,/*sarrayp*/false);
	      if (foundp == true) {
		segmenti->usedp = segmentj->usedp = true;
	      }
	    } else {
	      debug2(printf("too many mismatches, because floor = %d+middle+%d=%d > %d\n",
			    floors->scorefrom[-index1interval][segmentj->querypos5],
			    floors->scorefrom[segmenti->querypos3][query_lastpos+index1interval],
			    floor,max_mismatches_allowed));
	      debug2(printf("too many mismatches, because floor %d > %d\n",floor,max_mismatches_allowed));
	    }
	  } else {
	    debug2(printf("garbage, because querypos3 %d >= querypos5 %d\n",
			  segmentj->querypos3,segmenti->querypos5));
	  }
	}

	/* Identify potential segmentj for segmenti */
	segmentj_end = segmenti+1;
	while (
#ifdef NO_MARKER_SEGMENTS
	       segmentj_end < &(minus_segments[minus_nsegments]) && segmentj_end->chrnum == segmenti->chrnum &&
#endif
	       segmentj_end->diagonal <= segmenti->diagonal + max_middle_insertions) {
	  segmentj_end++;
	}

	for (segmentj = segmenti+1; segmentj < segmentj_end; segmentj++) {
	  debug2(printf("minus insertion?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d => ",
			segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
			segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	  /* i5 i3 j5 j3 */
	  if (segmenti->querypos3 < segmentj->querypos5) {
	    indels = segmentj->diagonal - segmenti->diagonal; /* positive */
	    floor = floors_from_neg3[segmenti->querypos5] + floors_to_pos3[segmentj->querypos3]
	      /* floors->score[-index1interval][segmenti->querypos5] + floors->score[segmentj->querypos3][query_lastpos+index1interval] */;
	    if (floors->prev_omitted == NULL) {
	      if ((middle = FLOOR_MIDDLE(segmentj->querypos5 - segmenti->querypos3 - indels)) > 0) {
		middle--;	/* for insertion, which looks like a mismatch */
	      }
	      debug2(printf("\nmiddle (no omission): %d\n",middle));
	      floor += middle;
	    } else {
	      pos = segmentj->querypos5;
	      debug2(printf("\nmiddle (omission):"));
	      while (pos > segmenti->querypos3) {
		if ((prev = floors->prev_omitted[pos]) < segmenti->querypos3) {
		  prev = segmenti->querypos3;
		}
		if ((middle = FLOOR_MIDDLE(pos - prev - indels)) > 0) {
		  middle--;	/* for insertion, which looks like a mismatch */
		}
		floor += middle;
		debug2(printf("(%d..%d)+%d,",prev,pos,middle));
		pos = prev;
	      }
	      debug2(printf("\n"));
	    }
	    if (floor <= max_mismatches_allowed) {
	      debug2(printf("successful insertion, floor = %d+middle+%d=%d, indels = %d\n",
			    floors->scorefrom[-index1interval][segmenti->querypos5],
			    floors->scorefrom[segmentj->querypos3][query_lastpos+index1interval],
			    floor,indels));
	      hits = Indel_solve_middle_insertion(&foundp,&(*found_score),&(*nhits),hits,
						  /*left*/segmenti->diagonal - querylength,
						  segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
						  indels,/*query_compress*/query_compress_rev,
#ifdef DEBUG2
						  /*queryptr*/queryrc,
#endif
						  querylength,max_mismatches_allowed,/*plusp*/false,genestrand,/*sarrayp*/false);
	      if (foundp == true) {
		segmenti->usedp = segmentj->usedp = true;
	      }
	    } else {
	      debug2(printf("too many mismatches, because floor %d+middle+%d=%d > %d\n",
			    floors->scorefrom[-index1interval][segmenti->querypos5],
			    floors->scorefrom[segmentj->querypos3][query_lastpos+index1interval],
			    floor,max_mismatches_allowed));
	    }
	  } else {
	    debug2(printf("garbage, because querypos3 %d >= querypos5 %d\n",
			  segmenti->querypos3,segmentj->querypos5));
	  }
	}
      }
    }
  }

  return hits;
}


/************************************************************************/

/************************************************************************
 *   right deletion: use <  / indel_pos = [conti]
 *   right insertion: use - sep <  / indel_pos = [conti]
 *   left deletion: use >  / indel_pos = [conti] + 1
 *   left insertion: use + sep >   / indel_pos = [conti]-sep+1
 ************************************************************************/

static int
compute_end_indels_right (int *indels, int *nmismatches_longcont, int *nmismatches_shift,
			  int *mismatch_positions_long, int nmismatches_avail_long,
			  int breakpoint, int querylength, Univcoord_T left, Compress_T query_compress,
			  int min_indel_end_matches, int max_end_insertions, int max_end_deletions,
			  int max_mismatches_short, bool plusp, int genestrand) {
#ifdef DEBUG2E
  int i;
#endif
  int length1;
  int sep, end;
  int nmismatches_avail_shift, nmatches;
  int mismatch_positions_shift[MAX_READLENGTH+1];
  int sum, best_sum = MAX_READLENGTH;
  int conti, shifti;
  int best_indel_pos = -1, endlength;
#ifdef OLD_END_INDELS
  int indel_pos;
#else
  int indel_pos_cont, indel_pos_shift;
#endif

  debug2e(printf("Entered compute_end_indels_right with breakpoint = %d, max_mismatches_short %d\n",
		 breakpoint,max_mismatches_short));
  length1 = querylength - breakpoint;
#if 0
  /* Should not need to reset max_end_deletions */
  if (max_end_deletions > length1 - min_indel_end_matches) {
    max_end_deletions = length1 - min_indel_end_matches;
  }
#endif
  if (max_end_insertions > length1 - min_indel_end_matches) {
    max_end_insertions = length1 - min_indel_end_matches;
  }

  if (max_end_deletions > 0) {
    for (sep = 1; sep <= max_end_deletions; sep++) {
      /* *indels = -sep; */
      nmismatches_avail_shift = Genome_mismatches_right(mismatch_positions_shift,
							max_mismatches_short,query_compress,
							left-(-sep),/*pos5*/0,/*pos3*/querylength,
							plusp,genestrand);
      debug2e(
	      printf("A. Trying deletion of %d.  ",-sep);
	      printf("%d mismatches on right at:",nmismatches_avail_shift);
	      for (i = 0; i <= nmismatches_avail_shift; i++) {
		printf(" %d",mismatch_positions_shift[i]);
	      }
	      printf(".  ");
	      );

      if (nmismatches_avail_shift == 0) {
	/* Skip, because we have an exact match on the shift */
      } else {
#ifdef OLD_END_INDELS
	/* Compute over mismatch_positions_shift[n-1] to querylength */
	/* A. Right deletion: Primary loop along Genome_mismatches_right (shifti) to get lowest coordinate */
	shifti = 0;
	conti = nmismatches_avail_long - 1;
	while (shifti < nmismatches_avail_shift) {
	  while (conti >= 0 && mismatch_positions_long[conti] > mismatch_positions_shift[shifti]) {
	    conti--;
	  }
	  sum = shifti + conti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti+1,shifti,mismatch_positions_shift[shifti]+1));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_shift[shifti] + 1;
	    if ((endlength = querylength - indel_pos) >= min_indel_end_matches && endlength >= sep) {
	      /* Don't want to delete more than the amount matched */
	      nmatches = endlength - shifti;
	      if (nmatches - 3*shifti - 4 >= 0) {
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		/* Want more matches than mismatches */
		best_indel_pos = indel_pos;
		*indels = -sep;
		*nmismatches_longcont = conti + 1;
		*nmismatches_shift = shifti;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  shifti++;
	}
	debug2e(printf("\n"));

	/* A. Right deletion: Primary loop along Genome_mismatches_left (conti) to see if we missed anything */
	shifti = nmismatches_avail_shift - 1;
	conti = 0;

	/* Start in region */
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] < mismatch_positions_shift[shifti]) {
	  conti++;
	}

	while (conti < nmismatches_avail_long) {
	  while (shifti >= 0 && mismatch_positions_shift[shifti] < mismatch_positions_long[conti]) {
	    shifti--;
	  }
	  sum = conti + shifti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_long[conti];
	    if ((endlength = querylength - indel_pos) >= min_indel_end_matches && endlength >= sep) {
	      /* Don't want to delete more than the amount matched */
	      nmatches = endlength - (shifti + 1);
	      if (nmatches - 3*(shifti+1) - 4 >= 0) {
		/* Want more matches than mismatches */
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		best_indel_pos = indel_pos;
		*indels = -sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = shifti + 1;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  conti++;
	}
	debug2e(printf("\n"));
#else
	shifti = nmismatches_avail_shift - 1;
	conti = 0;
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] < mismatch_positions_shift[shifti]) {
	  conti++;
	}
	indel_pos_cont = mismatch_positions_long[conti];
	indel_pos_shift = mismatch_positions_shift[shifti] + 1;

	while (conti < nmismatches_avail_long && shifti >= 0) {
	  if (indel_pos_cont < indel_pos_shift) {
	    sum = shifti + conti + 1;
	    debug2e(printf("cont %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]));
	    if (sum < best_sum) {
	      if ((endlength = querylength - indel_pos_cont) >= min_indel_end_matches && endlength >= sep) {
		/* Don't want to delete more than the amount matched */
		nmatches = endlength - (shifti + 1);
		if (nmatches - 3*(shifti+1) - 4 >= 0) {
		  /* Want more matches than mismatches */
		  /* Values -3 and -4 correspond to defaults for trim_mismatch_score and trim_indel_score */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		  best_indel_pos = indel_pos_cont;
		  *indels = -sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti + 1;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    indel_pos_cont = mismatch_positions_long[conti];

	  } else if (indel_pos_shift < indel_pos_cont) {
	    sum = shifti + conti;
	    debug2e(printf("shift %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,mismatch_positions_shift[shifti]+1));
	    if (sum < best_sum) {
	      if ((endlength = querylength - indel_pos_shift) >= min_indel_end_matches && endlength >= sep) {
		/* Don't want to delete more than the amount matched */
		nmatches = endlength - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = -sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    shifti--;
	    indel_pos_shift = mismatch_positions_shift[shifti] + 1;

	  } else {
	    sum = shifti + conti;
	    debug2e(printf("both %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,indel_pos_cont));
	    if (sum < best_sum) {
	      if ((endlength = querylength - indel_pos_shift) >= min_indel_end_matches && endlength >= sep) {
		/* Don't want to delete more than the amount matched */
		nmatches = endlength - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = -sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    shifti--;
	    indel_pos_cont = mismatch_positions_long[conti];
	    indel_pos_shift = mismatch_positions_shift[shifti] + 1;
	  }
	}
	
	if (shifti < 0) {
	  sum = conti /*+ shifti + 1*/;
	  debug2e(printf("last %d=%d at indel_pos %d.  ",sum,conti,indel_pos_cont));
	  if (sum < best_sum) {
	    if ((endlength = querylength - indel_pos_cont) >= min_indel_end_matches && endlength >= sep) {
	      /* Don't want to delete more than the amount matched */
	      nmatches = endlength /*- (shifti + 1)*/;
	      if (nmatches >= /*shifti + 1*/ + 4) {
		/* Want more matches than mismatches */
		best_indel_pos = indel_pos_cont;
		*indels = -sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = 0 /*shifti + 1*/;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	}

	debug2e(printf("\n"));
#endif

      }
    }
  }
  
  if (max_end_insertions > 0) {
    if (left < (unsigned int) max_end_insertions) {
      debug2e(printf("left %lu < max_end_insertions %d, so end = left\n",left,max_end_insertions));
      end = left;
    } else {
      end = max_end_insertions;
    }

    for (sep = 1; sep <= end; sep++) {
      /* *indels = +sep; */
      nmismatches_avail_shift = Genome_mismatches_right(mismatch_positions_shift,
							max_mismatches_short,query_compress,
							left-(+sep),/*pos5*/0,/*pos3*/querylength,
							plusp,genestrand);

      debug2e(
	      printf("B. Trying insertion of %d.  ",+sep);
	      printf("%d mismatches on right at:",nmismatches_avail_shift);
	      for (i = 0; i <= nmismatches_avail_shift; i++) {
		printf(" %d",mismatch_positions_shift[i]);
	      }
	      printf(".  ");
	      );

      if (nmismatches_avail_shift == 0) {
	/* Skip, because we have an exact match on the shift */
      } else {
#ifdef OLD_END_INDELS
	/* Compute over mismatch_positions_shift[n-1] to querylength */
	/* B. Right insertion: First, try primary loop along Genome_mismatches_right (shifti) to get lowest coordinate */
	shifti = 0;
	conti = nmismatches_avail_long - 1;
	while (shifti < nmismatches_avail_shift) {
	  while (conti >= 0 && mismatch_positions_long[conti] > mismatch_positions_shift[shifti] - sep) {
	    conti--;
	  }
	  sum = shifti + conti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti+1,shifti,mismatch_positions_shift[shifti]-sep+1));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_shift[shifti] - sep + 1;
	    endlength = querylength - (indel_pos + sep);
	    if (endlength >= min_indel_end_matches) {
	      nmatches = endlength - shifti;
	      if (nmatches - 3*shifti - 4 >= 0) {
		/* Want more matches than mismatches */
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		best_indel_pos = indel_pos;
		*indels = +sep;
		*nmismatches_longcont = conti + 1;
		*nmismatches_shift = shifti;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  shifti++;
	}
	debug2e(printf("\n"));


	/* B. Right insertion: Try primary loop along Genome_mismatches_left (conti) to see if we missed anything */
	shifti = nmismatches_avail_shift - 1;
	conti = 0;

	/* Start in region */
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] < mismatch_positions_shift[shifti]) {
	  conti++;
	}

	while (conti < nmismatches_avail_long) {
	  while (shifti >= 0 && mismatch_positions_shift[shifti] < mismatch_positions_long[conti] + sep) {
	    shifti--;
	  }
	  sum = conti + shifti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_long[conti];
	    endlength = querylength - indel_pos - sep;
	    if (endlength >= min_indel_end_matches) {
	      nmatches = endlength - (shifti + 1);
	      if (nmatches - 3*(shifti+1) - 4 >= 0) {
		/* Want more matches than mismatches */
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		best_indel_pos = indel_pos;
		*indels = +sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = shifti + 1;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  conti++;
	}
	debug2e(printf("\n"));

#else
	shifti = nmismatches_avail_shift - 1;
	conti = 0;
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] < mismatch_positions_shift[shifti]) {
	  conti++;
	}
	indel_pos_cont = mismatch_positions_long[conti];
	indel_pos_shift = mismatch_positions_shift[shifti] - sep + 1;

	while (conti < nmismatches_avail_long && shifti >= 0) {
	  if (indel_pos_cont < indel_pos_shift) {
	    sum = conti + shifti + 1;
	    debug2e(printf("cont %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]));
	    if (sum < best_sum) {
	      endlength = querylength - indel_pos_cont - sep;
	      if (endlength >= min_indel_end_matches) {
		nmatches = endlength - (shifti + 1);
		if (nmatches - 3*(shifti+1) - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		  best_indel_pos = indel_pos_cont;
		  *indels = +sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti + 1;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    indel_pos_cont = mismatch_positions_long[conti];

	  } else if (indel_pos_shift < indel_pos_cont) {
	    sum = shifti + conti;
	    debug2e(printf("shift %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,mismatch_positions_shift[shifti]-sep+1));
	    if (sum < best_sum) {
	      endlength = querylength - (indel_pos_shift + sep);
	      if (endlength >= min_indel_end_matches) {
		nmatches = endlength - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = +sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    shifti--;
	    indel_pos_shift = mismatch_positions_shift[shifti] - sep + 1;

	  } else {
	    sum = shifti + conti;
	    debug2e(printf("both %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,mismatch_positions_shift[shifti]-sep+1));
	    if (sum < best_sum) {
	      endlength = querylength - (indel_pos_shift + sep);
	      if (endlength >= min_indel_end_matches) {
		nmatches = endlength - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = +sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    shifti--;
	    indel_pos_cont = mismatch_positions_long[conti];
	    indel_pos_shift = mismatch_positions_shift[shifti] - sep + 1;
	  }
	}

	if (shifti < 0) {
	  sum = conti /*+ shifti + 1*/;
	  debug2e(printf("last %d=%d at indel_pos %d.  ",sum,conti,mismatch_positions_long[conti]));
	  if (sum < best_sum) {
	    endlength = querylength - indel_pos_cont - sep;
	    if (endlength >= min_indel_end_matches) {
	      nmatches = endlength /*- (shifti + 1)*/;
	      if (nmatches >= /*shifti + 1*/ + 4) {
		/* Want more matches than mismatches */
		best_indel_pos = indel_pos_cont;
		*indels = +sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = 0 /*shifti + 1*/;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	}

	debug2e(printf("\n"));
#endif
      }
    }
  }

  debug2e(printf("compute_end_indels_right returning with nmismatches_longcont %d + nmismatches_shift %d for %d indels at indel_pos %d\n",
		 *nmismatches_longcont,*nmismatches_shift,*indels,best_indel_pos));

  return best_indel_pos;
}


/* Want genomic low position for indel, so check deletions and insertions in reverse order of preference
   and check for sum <= best_sum */
static int
compute_end_indels_left (int *indels, int *nmismatches_longcont, int *nmismatches_shift,
			 int *mismatch_positions_long, int nmismatches_avail_long,
			 int breakpoint, int querylength, Univcoord_T left, Compress_T query_compress,
			 int min_indel_end_matches, int max_end_insertions, int max_end_deletions,
			 int max_mismatches_short, bool plusp, int genestrand) {
#ifdef DEBUG2E
  int i;
#endif
  int length1;
  int sep, start;
  int nmismatches_avail_shift, nmatches;
  int mismatch_positions_shift[MAX_READLENGTH+1];
  int sum, best_sum = MAX_READLENGTH;
  int conti, shifti;
  int best_indel_pos = -1;
#ifdef OLD_END_INDELS
  int indel_pos;
#else
  int indel_pos_cont, indel_pos_shift;
#endif


  debug2e(printf("Entered compute_end_indels_left with breakpoint = %d, max_mismatches_short %d\n",
		 breakpoint,max_mismatches_short));
  length1 = breakpoint;
#if 0
  /* Should not need to reset max_end_deletions */
  if (max_end_deletions > length1 - min_indel_end_matches) {
    max_end_deletions = length1 - min_indel_end_matches;
    debug2e(printf("Resetting max_end_deletions to be %d - %d = %d\n",length1,min_indel_end_matches,max_end_deletions));
  }
#endif
  if (max_end_insertions > length1 - min_indel_end_matches) {
    max_end_insertions = length1 - min_indel_end_matches;
  }

  if (max_end_insertions > 0) {
    for (sep = max_end_insertions; sep >= 1; sep--) {
      /* *indels = +sep; */
      nmismatches_avail_shift = Genome_mismatches_left(mismatch_positions_shift,
						       max_mismatches_short,query_compress,
						       left+(+sep),/*pos5*/0,/*pos3*/querylength,
						       plusp,genestrand);
      debug2e(
	      printf("D. Trying insertion of %d.  ",+sep);
	      printf("%d mismatches on left at:",nmismatches_avail_shift);
	      for (i = 0; i <= nmismatches_avail_shift; i++) {
		printf(" %d",mismatch_positions_shift[i]);
	      }
	      printf(".  ");
	      );

      if (nmismatches_avail_shift == 0) {
	/* Skip, because we have an exact match on the shift */
      } else {
#ifdef OLD_END_INDELS
	/* Compute over 0 to mismatch_positions_shift[n-1] */
	/* D. Left insertion.  First, try primary loop is on Genome_mismatches_right (conti), to get lowest coordinate */
	shifti = nmismatches_avail_shift - 1;
	conti = 0;

	/* Start in region */
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] > mismatch_positions_shift[shifti]) {
	  conti++;
	}

	while (conti < nmismatches_avail_long) {
	  while (shifti >= 0 && mismatch_positions_shift[shifti] > mismatch_positions_long[conti] - sep) {
	    shifti--;
	  }
	  sum = conti + shifti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]-sep+1));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_long[conti] - sep + 1;
	    if (indel_pos >= min_indel_end_matches) {
	      nmatches = indel_pos - (shifti + 1);
	      if (nmatches - 3*(shifti+1)- 4 >= 0) {
		/* Want more matches than mismatches */
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		best_indel_pos = indel_pos;
		*indels = +sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = shifti + 1;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  conti++;
	}
	debug2e(printf("\n"));


	/* D. Left insertion.  Then, try primary loop is on Genome_mismatches_left (shifti), to see if we missed anything */
	shifti = 0;
	conti = nmismatches_avail_long - 1;
	while (shifti < nmismatches_avail_shift) {
	  while (conti >= 0 && mismatch_positions_long[conti] < mismatch_positions_shift[shifti] + sep) {
	    conti--;
	  }
	  sum = shifti + conti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti+1,shifti,mismatch_positions_shift[shifti]));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_shift[shifti];
	    if (indel_pos >= min_indel_end_matches) {
	      nmatches = indel_pos - shifti;
	      if (nmatches - 3*shifti - 4 >= 0) {
		/* Want more matches than mismatches */
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		best_indel_pos = indel_pos;
		*indels = +sep;
		*nmismatches_longcont = conti + 1;
		*nmismatches_shift = shifti;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  shifti++;
	}
	debug2e(printf("\n"));

#else
	shifti = nmismatches_avail_shift - 1;
	conti = 0;
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] > mismatch_positions_shift[shifti]) {
	  conti++;
	}
	indel_pos_cont = mismatch_positions_long[conti] - sep + 1;
	indel_pos_shift = mismatch_positions_shift[shifti];

	while (conti < nmismatches_avail_long && shifti >= 0) {
	  if (indel_pos_cont > indel_pos_shift) {
	    sum = conti + shifti + 1;
	    debug2e(printf("cont %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]-sep+1));
	    if (sum <= best_sum) {
	      if (indel_pos_cont >= min_indel_end_matches) {
		nmatches = indel_pos_cont - (shifti + 1);
		if (nmatches - 3*(shifti+1) - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		  best_indel_pos = indel_pos_cont;
		  *indels = +sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti + 1;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    indel_pos_cont = mismatch_positions_long[conti] - sep + 1;

	  } else if (indel_pos_shift > indel_pos_cont) {
	    sum = shifti + conti;
	    debug2e(printf("shift %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,mismatch_positions_shift[shifti]));
	    if (sum <= best_sum) {
	      if (indel_pos_shift >= min_indel_end_matches) {
		nmatches = indel_pos_shift - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = +sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    shifti--;
	    indel_pos_shift = mismatch_positions_shift[shifti];

	  } else {
	    sum = shifti + conti;
	    debug2e(printf("both %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,mismatch_positions_shift[shifti]));
	    if (sum <= best_sum) {
	      if (indel_pos_shift >= min_indel_end_matches) {
		nmatches = indel_pos_shift - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = +sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    shifti--;
	    indel_pos_cont = mismatch_positions_long[conti] - sep + 1;
	    indel_pos_shift = mismatch_positions_shift[shifti];

	  }
	}

	if (shifti < 0) {
	  sum = conti /*+ shifti + 1*/;
	  debug2e(printf("last %d=%d at indel_pos %d.  ",sum,conti,mismatch_positions_long[conti]-sep+1));
	  if (sum <= best_sum) {
	    if (indel_pos_cont >= min_indel_end_matches) {
	      nmatches = indel_pos_cont /*- (shifti + 1)*/;
	      if (nmatches >= /*shifti + 1*/ + 4) {
		/* Want more matches than mismatches */
		best_indel_pos = indel_pos_cont;
		*indels = +sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = 0 /*shifti + 1*/;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	}

	debug2e(printf("\n"));
#endif
      }
    }
  }


  if (max_end_deletions > 0) {
    if (left < (unsigned int) max_end_deletions) {
      debug2e(printf("left %lu < max_end_deletions %d, so start = left\n",left,max_end_deletions));
      start = left;
    } else {
      start = 1;
    }

    for (sep = max_end_deletions; sep >= 1; sep--) {
      /* *indels = -sep; */
      nmismatches_avail_shift = Genome_mismatches_left(mismatch_positions_shift,
						       max_mismatches_short,query_compress,
						       left+(-sep),/*pos5*/0,/*pos3*/querylength,
						       plusp,genestrand);
      debug2e(
	      printf("C. Trying deletion of %d.  ",-sep);
	      printf("%d mismatches on left at:",nmismatches_avail_shift);
	      for (i = 0; i <= nmismatches_avail_shift; i++) {
		printf(" %d",mismatch_positions_shift[i]);
	      }
	      printf(".  ");
	      );

      if (nmismatches_avail_shift == 0) {
	/* Skip, because we have an exact match on the shift */
      } else {
#ifdef OLD_END_INDELS
	/* Compute over 0 to mismatch_positions_shift[n-1] */
	/* C. Left deletion.  First, try primary loop (cont) on Genome_mismatches_right, to get lowest coordinate */
	shifti = nmismatches_avail_shift - 1;
	conti = 0;

	/* Start in region */
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] > mismatch_positions_shift[shifti]) {
	  conti++;
	}

	while (conti < nmismatches_avail_long) {
	  while (shifti >= 0 && mismatch_positions_shift[shifti] > mismatch_positions_long[conti]) {
	    shifti--;
	  }
	  sum = conti + shifti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]+1));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_long[conti] + 1;
	    if (indel_pos >= min_indel_end_matches && indel_pos >= sep) {
	      nmatches = indel_pos - (shifti + 1);
	      if (nmatches - 3*(shifti+1) - 4 >= 0) {
		/* Want more matches than mismatches */
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		best_indel_pos = indel_pos;
		*indels = -sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = shifti + 1;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  conti++;
	}
	debug2e(printf("\n"));


	/* C. Left deletion.  Then, try primary loop on Genome_mismatches_left (shifti) to see if we missed anything */
	shifti = 0;
	conti = nmismatches_avail_long - 1;
	while (shifti < nmismatches_avail_shift) {
	  while (conti >= 0 && mismatch_positions_long[conti] < mismatch_positions_shift[shifti]) {
	    conti--;
	  }
	  sum = shifti + conti + 1;
	  debug2e(printf("sum %d=%d+%d at indel_pos %d.  ",sum,conti+1,shifti,mismatch_positions_shift[shifti]));
	  if (sum < best_sum) {
	    indel_pos = mismatch_positions_shift[shifti];
	    if (indel_pos >= min_indel_end_matches && indel_pos >= sep) {
	      nmatches = indel_pos - shifti;
	      if (nmatches - 3*shifti - 4 >= 0) {
		/* Want more matches than mismatches */
		debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		best_indel_pos = indel_pos;
		*indels = -sep;
		*nmismatches_longcont = conti + 1;
		*nmismatches_shift = shifti;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	  shifti++;
	}
	debug2e(printf("\n"));

#else
	shifti = nmismatches_avail_shift - 1;
	conti = 0;
	while (conti < nmismatches_avail_long && mismatch_positions_long[conti] > mismatch_positions_shift[shifti]) {
	  conti++;
	}
	indel_pos_cont = mismatch_positions_long[conti] + 1;
	indel_pos_shift = mismatch_positions_shift[shifti];

	while (conti < nmismatches_avail_long && shifti >= 0) {
	  if (indel_pos_cont > indel_pos_shift) {
	    sum = conti + shifti + 1;
	    debug2e(printf("cont %d=%d+%d at indel_pos %d.  ",sum,conti,shifti+1,mismatch_positions_long[conti]+1));
	    if (sum <= best_sum) {
	      if (indel_pos_cont >= min_indel_end_matches && indel_pos_cont >= sep) {
		nmatches = indel_pos_cont - (shifti + 1);
		if (nmatches - 3*(shifti+1) - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti+1,nmatches-3*(shifti+1)-4));
		  best_indel_pos = indel_pos_cont;
		  *indels = -sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti + 1;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    indel_pos_cont = mismatch_positions_long[conti] + 1;

	  } else if (indel_pos_shift > indel_pos_cont) {
	    sum = shifti + conti;
	    debug2e(printf("shift %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,mismatch_positions_shift[shifti]));
	    if (sum <= best_sum) {
	      if (indel_pos_shift >= min_indel_end_matches && indel_pos_shift >= sep) {
		nmatches = indel_pos_shift - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = -sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    shifti--;
	    indel_pos_shift = mismatch_positions_shift[shifti];

	  } else {
	    sum = shifti + conti;
	    debug2e(printf("both %d=%d+%d at indel_pos %d.  ",sum,conti,shifti,mismatch_positions_shift[shifti]));
	    if (sum <= best_sum) {
	      if (indel_pos_shift >= min_indel_end_matches && indel_pos_shift >= sep) {
		nmatches = indel_pos_shift - shifti;
		if (nmatches - 3*shifti - 4 >= 0) {
		  /* Want more matches than mismatches */
		  debug2e(printf("nmatches %d - 3*%d - 4 = %d.  ",nmatches,shifti,nmatches-3*shifti-4));
		  best_indel_pos = indel_pos_shift;
		  *indels = -sep;
		  *nmismatches_longcont = conti;
		  *nmismatches_shift = shifti;
		  debug2e(printf("**"));
		  best_sum = sum;
		}
	      }
	    }
	    conti++;
	    shifti--;
	    indel_pos_cont = mismatch_positions_long[conti] + 1;
	    indel_pos_shift = mismatch_positions_shift[shifti];
	  }
	}

	if (shifti < 0) {
	  sum = conti /*+ shifti + 1*/;
	  debug2e(printf("last %d=%d at indel_pos %d.  ",sum,conti,mismatch_positions_long[conti]+1));
	  if (sum <= best_sum) {
	    if (indel_pos_cont >= min_indel_end_matches && indel_pos_cont >= sep) {
	      nmatches = indel_pos_cont /*- (shifti + 1)*/;
	      if (nmatches >= /*shifti + 1*/ + 4) {
		/* Want more matches than mismatches */
		best_indel_pos = indel_pos_cont;
		*indels = -sep;
		*nmismatches_longcont = conti;
		*nmismatches_shift = 0 /*shifti + 1*/;
		debug2e(printf("**"));
		best_sum = sum;
	      }
	    }
	  }
	}

	debug2e(printf("\n"));
#endif
      }
    }
  }


  debug2e(printf("compute_end_indels_left returning with nmismatches_cont %d + nmismatches_shift %d for %d indels at indel_pos %d\n",
		 *nmismatches_longcont,*nmismatches_shift,*indels,best_indel_pos));

  return best_indel_pos;
}


/************************************************************************/

/* Was solve_first_indel_plus and solve_last_indel_minus */
static List_T
solve_end_indel_low (int *found_score, int *nhits, List_T hits, Segment_T ptr,
		     Univcoord_T diagonal, int firstbound,
		     Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh, Chrpos_T chrlength,
#ifdef DEBUG2E
		     char *queryptr,
#endif
		     int querylength, Compress_T query_compress,
		     int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
		     int indel_penalty_end, int max_mismatches, bool plusp, int genestrand) {
#ifdef DEBUG2E
  char *gbuffer;
#endif
  int i;
  Stage3end_T hit;
  Univcoord_T left;
  int indels, query_indel_pos, indel_pos, breakpoint;
  int nmismatches, nmismatches_long, nmismatches_longcont, nmismatches_shift;
  int mismatch_positions[MAX_READLENGTH];
  int nmismatches1, nmismatches2;


  left = diagonal - querylength;
  if ((unsigned int) max_end_deletions > left - chroffset) {
    max_end_deletions = left - chroffset;
    /* diagonal - querylength guaranteed to be >= chroffset, so max_end_deletions >= 0 */
  }

  debug2e(
	  if (plusp == true) {
	    printf("\nsolve_end_indel_low: Getting genome at diagonal %lu - querylength %d - max_end_deletions %d = %lu.\n",
		   diagonal,querylength,max_end_deletions,left-max_end_deletions);
	  } else {
	    printf("\nsolve_end_indel_low: Getting genome at diagonal %lu + 12 - querylength %d = %lu, max_end_deletions = %d.\n",
		   diagonal,querylength,left,max_end_deletions);
	  });

  debug2e(gbuffer = (char *) CALLOC(querylength+max_end_deletions+1,sizeof(char)));
  debug2e(Genome_fill_buffer_blocks(left-max_end_deletions,querylength+max_end_deletions,gbuffer));
  debug2e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
  debug2e(FREE(gbuffer));

  /* No need to check chromosome bounds */
  nmismatches = Genome_mismatches_right(mismatch_positions,max_mismatches,
					query_compress,left,/*pos5*/0,/*pos3*/querylength,plusp,genestrand);

  debug2e(
	  printf("full read: %d (max %d) mismatches from right:",nmismatches,max_mismatches);
	  for (i = 0; i <= nmismatches; i++) {
	    printf(" %d",mismatch_positions[i]);
	  }
	  printf("\n");
	  );

  /* Find first mismatch past firstbound */
  i = 0;
  while (i <= nmismatches && mismatch_positions[i] > firstbound) {
    i++;
  }
  nmismatches_long = i;

#if 0
  nmismatches_short = nmismatches - i;
  /* Previously checked if nmismatches_short <= 0 */
#endif
  
  /* Should be >= */
  if (i >= nmismatches) {
    debug2e(printf("i %d >= nmismatches %d => no indel\n",i,nmismatches));
  } else {
#if 0
    mismatch_positions_short = &(mismatch_positions[i]);
    debug2e(
	    printf("nmismatches_long = %d, short = %d\n",nmismatches_long,nmismatches_short);
	    printf("short end from firstbound %d:",firstbound);
	    for (i = 0; i <= nmismatches_short; i++) {
	      printf(" %d",mismatch_positions_short[i]);
	    }
	    printf("\n");
	    );
    breakpoint = mismatch_positions_short[0] + 1;
#else
    breakpoint = mismatch_positions[i] + 1;
#endif

    if ((indel_pos = compute_end_indels_left(&indels,&nmismatches_longcont,&nmismatches_shift,
					     mismatch_positions,nmismatches,
					     breakpoint,querylength,left,query_compress,
					     min_indel_end_matches,max_end_insertions,max_end_deletions,
					     /*max_mismatches_allowed*/max_mismatches-nmismatches_long+1,
					     plusp,genestrand)) >= 0) {
      debug2e(printf("Got indel_pos %d.\n",indel_pos));

      /* formulas for query_indel_pos have been checked on examples */
      if (indels > 0) {
	if (plusp == true) {
	  query_indel_pos = indel_pos /* + indels */;
	  debug2e(printf("case 1: query_indel_pos = %d = %d\n",indel_pos,query_indel_pos));
	  nmismatches1 = nmismatches_shift;
	  nmismatches2 = nmismatches_longcont;
	  /* end1_indel_p = true; */
	  /* end2_indel_p = false; */
	} else {
	  query_indel_pos = querylength - indel_pos - indels;
	  debug2e(printf("case 2: query_indel_pos = %d - %d - %d = %d\n",querylength,indel_pos,indels,query_indel_pos));
	  nmismatches1 = nmismatches_longcont;
	  nmismatches2 = nmismatches_shift;
	  /* end1_indel_p = false; */
	  /* end2_indel_p = true; */
	}

	if ((hit = Stage3end_new_insertion(&(*found_score),indels,query_indel_pos,
					   nmismatches1,nmismatches2,
					   left+indels,/*genomiclength*/querylength-indels,
					   query_compress,querylength,plusp,genestrand,
					   chrnum,chroffset,chrhigh,chrlength,indel_penalty_end,
					   /*sarrayp*/false)) != NULL) {
	  ptr->usedp = true;
	  *nhits += 1;
	  hits = List_push(hits,(void *) hit);
	  debug2e(printf("successful insertion at %d with %d long/cont + %d shift mismatches\n",
			 indel_pos,nmismatches_longcont,nmismatches_shift));
	}

      } else {
	if (plusp == true) {
	  query_indel_pos = indel_pos;
	  nmismatches1 = nmismatches_shift;
	  nmismatches2 = nmismatches_longcont;
	  /* end1_indel_p = true; */
	  /* end2_indel_p = false; */
	} else {
	  query_indel_pos = querylength - indel_pos;
	  nmismatches1 = nmismatches_longcont;
	  nmismatches2 = nmismatches_shift;
	  /* end1_indel_p = false; */
	  /* end2_indel_p = true; */
	}

	if ((hit = Stage3end_new_deletion(&(*found_score),-indels,query_indel_pos,
					  nmismatches1,nmismatches2,
					  left+indels,/*genomiclength*/querylength-indels,
					  query_compress,querylength,plusp,genestrand,
					  chrnum,chroffset,chrhigh,chrlength,indel_penalty_end,
					  /*sarrayp*/false)) != NULL) {
	  ptr->usedp = true;
	  *nhits += 1;
	  hits = List_push(hits,(void *) hit);
	  debug2e(printf("successful end deletion at %d with %d long/cont + %d shift mismatches and nindels %d\n",
			 indel_pos,nmismatches_longcont,nmismatches_shift,-indels));
	}
      }
    }
  }

  return hits;
}


/* Was solve_first_indel_minus and solve_last_indel_plus */
static List_T
solve_end_indel_high (int *found_score, int *nhits, List_T hits, Segment_T ptr,
		      Univcoord_T diagonal, int lastbound,
		      Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh, Chrpos_T chrlength,
#ifdef DEBUG2E
		      char *queryptr,
#endif
		      int querylength, Compress_T query_compress,
		      int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
		      int indel_penalty_end, int max_mismatches, bool plusp, int genestrand) {
#ifdef DEBUG2E
  char *gbuffer;
#endif
  int i;
  Stage3end_T hit;
  Univcoord_T left;
  int indels, query_indel_pos, indel_pos, breakpoint;
  int nmismatches, nmismatches_long, nmismatches_longcont, nmismatches_shift;
  int mismatch_positions[MAX_READLENGTH];
  int nmismatches1, nmismatches2;


  left = diagonal - querylength;
  if ((unsigned int) max_end_deletions > chrhigh - diagonal) {
    max_end_deletions = chrhigh - diagonal;
    /* diagonal guaranteed to be <= chrhigh, so max_end_deletions >= 0 */
  }

  debug2e(
	  if (plusp == true) {
	    printf("\nsolve_end_indel_high: Getting genome at diagonal %lu - querylength %d + max_end_deletions %d = %lu.\n",
		   diagonal,querylength,max_end_deletions,left+max_end_deletions);
	  } else {
	    printf("\nsolve_end_indel_high: Getting genome at diagonal %lu + 12 - querylength %d = %lu, max_end_deletions = %d.\n",
		   diagonal,querylength,left,max_end_deletions);
	  });

  debug2e(gbuffer = (char *) CALLOC(querylength+max_end_deletions+1,sizeof(char)));
  debug2e(Genome_fill_buffer_blocks(left,querylength+max_end_deletions,gbuffer));
  debug2e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
  debug2e(FREE(gbuffer));

  /* No need to check chromosome bounds */
  /* Previously checked from 0 to lastbound */
  nmismatches = Genome_mismatches_left(mismatch_positions,max_mismatches,
				       query_compress,left,/*pos5*/0,/*pos3*/querylength,plusp,genestrand);

  debug2e(
	  printf("full read: %d (max %d) mismatches from left:",nmismatches,max_mismatches);
	  for (i = 0; i <= nmismatches; i++) {
	    printf(" %d",mismatch_positions[i]);
	  }
	  printf("\n");
	  );

  /* Find first mismatch past lastbound */
  i = 0;
  while (i <= nmismatches && mismatch_positions[i] < lastbound) {
    i++;
  }
  nmismatches_long = i;
#if 0
  /* Previously checked if nmismatches_short <= 0 */
  nmismatches_short = nmismatches - i;
#endif
  
  /* Should be >= */
  if (i >= nmismatches) {
    debug2e(printf("i %d >= nmismatches %d => no indel\n",i,nmismatches));
  } else {
#if 0
    mismatch_positions_short = &(mismatch_positions[i]);
    debug2e(
	    printf("nmismatches_long = %d, short = %d\n",nmismatches_long,nmismatches_short);
	    printf("short end from lastbound %d:",lastbound);
	    for (i = 0; i <= nmismatches_short; i++) {
	      printf(" %d",mismatch_positions_short[i]);
	    }
	    printf("\n");
	    );
    breakpoint = mismatch_positions_short[0] - 1;
#else
    breakpoint = mismatch_positions[i] - 1;
#endif

    if ((indel_pos = compute_end_indels_right(&indels,&nmismatches_longcont,&nmismatches_shift,
					      mismatch_positions,nmismatches,
					      breakpoint,querylength,left,query_compress,
					      min_indel_end_matches,max_end_insertions,max_end_deletions,
					      /*max_mismatches_allowed*/max_mismatches-nmismatches_long+1,
					      plusp,genestrand)) >= 0) {
      debug2e(printf("Got indel_pos %d\n",indel_pos));

      /* formulas for query_indel_pos have been checked on examples */
      if (indels > 0) {
	if (plusp == true) {
	  query_indel_pos = indel_pos /* + indels */;
	  debug2e(printf("case 3: query_indel_pos = %d = %d\n",indel_pos,query_indel_pos));
	  nmismatches1 = nmismatches_longcont;
	  nmismatches2 = nmismatches_shift;
	  /* end1_indel_p = false; */
	  /* end2_indel_p = true; */
	} else {
	  query_indel_pos = querylength - indel_pos - indels;
	  debug2e(printf("case 4: query_indel_pos = %d - %d - %d = %d\n",querylength,indel_pos,indels,query_indel_pos));
	  nmismatches1 = nmismatches_shift;
	  nmismatches2 = nmismatches_longcont;
	  /* end1_indel_p = true; */
	  /* end2_indel_p = false; */
	}

	if ((hit = Stage3end_new_insertion(&(*found_score),indels,query_indel_pos,
					   nmismatches1,nmismatches2,
					   left,/*genomiclength*/querylength-indels,
					   query_compress,querylength,plusp,genestrand,
					   chrnum,chroffset,chrhigh,chrlength,indel_penalty_end,
					   /*sarrayp*/false)) != NULL) {
	  ptr->usedp = true;
	  *nhits += 1;
	  hits = List_push(hits,(void *) hit);
	  debug2e(printf("successful end insertion at %d with %d long/cont + %d shift mismatches\n",
			 indel_pos,nmismatches_longcont,nmismatches_shift));
	}

      } else {
	if (plusp == true) {
	  query_indel_pos = indel_pos;
	  nmismatches1 = nmismatches_longcont;
	  nmismatches2 = nmismatches_shift;
	  /* end1_indel_p = false; */
	  /* end2_indel_p = true; */
	} else {
	  query_indel_pos = querylength - indel_pos;
	  nmismatches1 = nmismatches_shift;
	  nmismatches2 = nmismatches_longcont;
	  /* end1_indel_p = true; */
	  /* end2_indel_p = false; */
	}

	if ((hit = Stage3end_new_deletion(&(*found_score),-indels,query_indel_pos,
					  nmismatches1,nmismatches2,
					  left,/*genomiclength*/querylength-indels,
					  query_compress,querylength,plusp,genestrand,
					  chrnum,chroffset,chrhigh,chrlength,indel_penalty_end,
					  /*sarrayp*/false)) != NULL) {
	  ptr->usedp = true;
	  *nhits += 1;
	  hits = List_push(hits,(void *) hit);
	  debug2e(printf("successful end deletion at %d with %d long/cont + %d shift mismatches and nindels %d\n",
			 indel_pos,nmismatches_longcont,nmismatches_shift,-indels));
	}
      }
    }
  }

  return hits;
}


static List_T
find_end_indels (int *found_score, int *nhits, List_T hits,
		 struct Segment_T *plus_segments, struct Segment_T *minus_segments,
		 int plus_nsegments, int minus_nsegments,
#ifdef DEBUG2E
		 char *queryuc_ptr, char *queryrc,
#endif
		 int querylength, int firstbound, int lastbound,
		 Compress_T query_compress_fwd, Compress_T query_compress_rev,
		 int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
		 int indel_penalty_end, int max_mismatches_allowed, int genestrand) {
  Segment_T ptr;

  debug(printf("*** find_end_indels with max_mismatches_allowed %d ***\n",
	       max_mismatches_allowed));

  for (ptr = plus_segments; ptr < &(plus_segments[plus_nsegments]); ptr++) {
    if (ptr->diagonal < (Univcoord_T) -1) {

      if (ptr->floor_xfirst <= max_mismatches_allowed) {
	/* First indel, plus */
	debug2e(printf("floor_xfirst %d <= mismatches allowed %d\n",ptr->floor_xfirst,max_mismatches_allowed));
	hits = solve_end_indel_low(&(*found_score),&(*nhits),hits,ptr,ptr->diagonal,firstbound,
				   ptr->chrnum,ptr->chroffset,ptr->chrhigh,ptr->chrlength,
#ifdef DEBUG2E
				   /*queryptr*/queryuc_ptr,
#endif
				   querylength,/*query_compress*/query_compress_fwd,
				   max_end_insertions,max_end_deletions,min_indel_end_matches,
				   indel_penalty_end,max_mismatches_allowed,/*plusp*/true,genestrand);
      }

      if (ptr->floor_xlast <= max_mismatches_allowed) {
	/* Last indel, plus */
	debug2e(printf("floor_xlast %d <= mismatches allowed %d\n",ptr->floor_xlast,max_mismatches_allowed));
	hits = solve_end_indel_high(&(*found_score),&(*nhits),hits,ptr,ptr->diagonal,lastbound,
				    ptr->chrnum,ptr->chroffset,ptr->chrhigh,ptr->chrlength,
#ifdef DEBUG2E
				    /*queryptr*/queryuc_ptr,
#endif
				    querylength,/*query_compress*/query_compress_fwd,
				    max_end_insertions,max_end_deletions,min_indel_end_matches,
				    indel_penalty_end,max_mismatches_allowed,/*plusp*/true,genestrand);
      }
    }
  }

  for (ptr = minus_segments; ptr < &(minus_segments[minus_nsegments]); ptr++) {
    if (ptr->diagonal < (Univcoord_T) -1) {

      if (ptr->floor_xfirst <= max_mismatches_allowed) {
	/* First indel, minus */
	debug2e(printf("floor_xfirst %d <= mismatches allowed %d\n",ptr->floor_xfirst,max_mismatches_allowed));
	hits = solve_end_indel_high(&(*found_score),&(*nhits),hits,ptr,ptr->diagonal,lastbound,
				    ptr->chrnum,ptr->chroffset,ptr->chrhigh,ptr->chrlength,
#ifdef DEBUG2E
				    /*queryptr*/queryrc,
#endif
				    querylength,/*query_compress*/query_compress_rev,
				    max_end_insertions,max_end_deletions,min_indel_end_matches,
				    indel_penalty_end,max_mismatches_allowed,/*plusp*/false,genestrand);
      }

      if (ptr->floor_xlast <= max_mismatches_allowed) {
	/* Last indel, minus */
	debug2e(printf("floor_xlast %d <= mismatches allowed %d\n",ptr->floor_xlast,max_mismatches_allowed));
	hits = solve_end_indel_low(&(*found_score),&(*nhits),hits,ptr,ptr->diagonal,firstbound,
				   ptr->chrnum,ptr->chroffset,ptr->chrhigh,ptr->chrlength,
#ifdef DEBUG2E
				   /*queryptr*/queryrc,
#endif
				   querylength,/*query_compress*/query_compress_rev,
				   max_end_insertions,max_end_deletions,min_indel_end_matches,
				   indel_penalty_end,max_mismatches_allowed,/*plusp*/false,genestrand);
      }
    }
  }

  return hits;
}



/************************************************************************
 *   Splicing
 ************************************************************************/


/* Do not compare against true or false */
/* Moderate criterion */
static int
sufficient_splice_prob_distant (int support, int nmismatches, double spliceprob) {
  support -= 3*nmismatches;
  if (support < min_distantsplicing_end_matches) {
    return 0;
  } else if (support < 30) {
    return (spliceprob > 0.95);
  } else if (support < 35) {
    return (spliceprob > 0.90);
  } else if (support < 40) {
    return (spliceprob > 0.85);
  } else {
    return (spliceprob > 0.70);
  }
}

/* Do not compare against true or false */

#ifdef HALFINTRON
/* Strictest criterion */
static int
sufficient_splice_prob_halfintron (int support, int nmismatches, double spliceprob) {
  support -= 3*nmismatches;
  if (support < 20) {
    return 0;
  } else if (support < 26) {
    return (spliceprob > 0.95);
  } else if (support < 32) {
    return (spliceprob > 0.90);
  } else if (support < 38) {
    return (spliceprob > 0.85);
  } else if (support < 44) {
    return (spliceprob > 0.80);
  } else if (support < 50) {
    return (spliceprob > 0.50);
  } else {
    return 1;
  }
}
#endif



#if 0
static void
find_segmentm_span (Segment_T segmentm, int max_mismatches_allowed,
		    int querylength, Compress_T query_compress,
		    Univcoord_T left, bool plusp, int genestrand) {
  int mismatch_positions[MAX_READLENGTH];
  int nmismatches, i;
  int leftspan, rightspan, bestspan;

  /* Find all mismatches */
  nmismatches = Genome_mismatches_left(mismatch_positions,/*max_mismatches*/querylength,
				       query_compress,left,/*pos5*/0,/*pos3*/querylength,plusp,genestrand);

  if (nmismatches < max_mismatches_allowed) {
    segmentm->leftspan = 0;
    segmentm->rightspan = querylength;
  } else {
    segmentm->leftspan = 0;
    bestspan = segmentm->rightspan = mismatch_positions[max_mismatches_allowed] + /*slop*/ 1;
    for (i = 0; i < nmismatches - max_mismatches_allowed; i++) {
      leftspan = mismatch_positions[i];
      rightspan = mismatch_positions[i + max_mismatches_allowed + 1] + /*slop*/ 1;
      if (rightspan - leftspan > bestspan) {
	segmentm->leftspan = leftspan;
	segmentm->rightspan = rightspan;
	bestspan = rightspan - leftspan;
      } else if (rightspan - leftspan == bestspan) {
	segmentm->rightspan = rightspan;
      }
    }
  }
  return;
}
#endif


static List_T
find_singlesplices_plus (int *found_score, List_T hits, List_T *lowprob,
			 Segment_T *plus_spliceable, int plus_nspliceable,
			 Floors_T floors, int querylength, int query_lastpos,
			 Compress_T query_compress /* expecting fwd */,
			 int splicing_penalty, int max_mismatches_allowed, bool first_read_p, int genestrand,
			 bool subs_or_indels_p) {
#ifdef DEBUG4S
  int i;
#endif
  int j;
  Segment_T segmenti, segmentj, segmentj_end, *ptr;
  Univcoord_T segmenti_left, segmentj_left;
  int mismatch_positions_left[MAX_READLENGTH], mismatch_positions_right[MAX_READLENGTH];
  int nmismatches_left, nmismatches_right;
  int segmenti_donor_knownpos[MAX_READLENGTH+1], segmentj_acceptor_knownpos[MAX_READLENGTH+1],
    segmentj_antidonor_knownpos[MAX_READLENGTH+1], segmenti_antiacceptor_knownpos[MAX_READLENGTH+1];
  int segmenti_donor_knowni[MAX_READLENGTH+1], segmentj_acceptor_knowni[MAX_READLENGTH+1],
    segmentj_antidonor_knowni[MAX_READLENGTH+1], segmenti_antiacceptor_knowni[MAX_READLENGTH+1];
  int segmenti_donor_nknown, segmentj_acceptor_nknown,
    segmentj_antidonor_nknown, segmenti_antiacceptor_nknown;
  
  Chrpos_T max_distance;

  int floor_outer_i;
  int *floors_from_neg3, *floors_to_pos3;
  int nhits_local = 0;


  debug4s(printf("*** Starting find_singlesplices_plus on %d spliceable segments ***\n",plus_nspliceable));
  /* debug(printf("Initially have %d hits\n",List_length(hits))); */

  floors_from_neg3 = floors->scorefrom[-index1interval];
  floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

  for (ptr = plus_spliceable; ptr < &(plus_spliceable[plus_nspliceable]); ptr++) {
    segmenti = *ptr;
    debug4s(printf("plus_spliceable segmenti at diagonal %u\n",segmenti->diagonal));
    if (1 || segmenti->diagonal < (Univcoord_T) -1) { /* No markers were stored in spliceable */
      segmenti_left = segmenti->diagonal - querylength;
      floor_outer_i = floors_from_neg3[segmenti->querypos5];

      segmenti_donor_nknown = 0;
      segmenti_antiacceptor_nknown = 0;
      max_distance = shortsplicedist;

      if ((j = segmenti->splicesites_i) >= 0) {
	/* Ends 1 (donor, plus) and 8 (antiacceptor, plus): mark known splice sites in segmenti */
	while (j < nsplicesites && splicesites[j] < segmenti->diagonal) {
	  if (splicetypes[j] == DONOR) {
	    debug4s(printf("Setting known donor %d for segmenti at %lu\n",j,splicesites[j]));
	    segmenti_donor_knownpos[segmenti_donor_nknown] = splicesites[j] - segmenti_left;
	    segmenti_donor_knowni[segmenti_donor_nknown++] = j;
	  } else if (splicetypes[j] == ANTIACCEPTOR) {
	    debug4s(printf("Setting known antiacceptor %d for segmenti at %lu\n",j,splicesites[j]));
	    segmenti_antiacceptor_knownpos[segmenti_antiacceptor_nknown] = splicesites[j] - segmenti_left;
	    segmenti_antiacceptor_knowni[segmenti_antiacceptor_nknown++] = j;
	  }

	  /* This computation was already made in identify_all_segments */
	  if (splicedists[j] > max_distance) {
	    debug4s(printf("Setting max_distance for known i %d to be %u\n",j,splicedists[j]));
	    max_distance = splicedists[j];
	  }

	  j++;
	}
      }
      segmenti_donor_knownpos[segmenti_donor_nknown] = MAX_READLENGTH;
      segmenti_antiacceptor_knownpos[segmenti_antiacceptor_nknown] = MAX_READLENGTH;


      /* Identify potential segmentj for segmenti */
      segmentj_end = segmenti+1;
      while (
#ifdef NO_MARKER_SEGMENTS
	     segmentj_end < &(plus_segments[plus_nsegments]) && segmentj_end->chrnum == segmenti->chrnum &&
#endif
	     segmentj_end->diagonal <= segmenti->diagonal + max_distance) {
	segmentj_end++;
      }
      if (segmentj_end - segmenti > MAX_LOCALSPLICING_POTENTIAL + 1) {    /* segmentj_end - segmenti - 1 > MAX_LOCALSPLICING_POTENTIAL */
	/* Too many to check */
	segmentj_end = segmenti+1 + MAX_LOCALSPLICING_POTENTIAL;
      }

      for (segmentj = segmenti+1; segmentj < segmentj_end; segmentj++) {
	debug4s(printf("plus local?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d => ",
		       segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
		       segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	/* i5 i3 j5 j3 */
	assert(segmenti->diagonal < segmentj->diagonal);
	if (segmenti->querypos3 >= segmentj->querypos5) {
	  /* Fail querypos test */
	  debug4s(printf("Bad querypos\n"));

	} else if (segmenti->diagonal + min_intronlength > segmentj->diagonal) {
	  /* Too short to be an intron */
	  debug4s(printf("Too short\n"));

	} else {
	  segmenti->right_splice_p = true;
	  segmentj->left_splice_p = true;
	  if (floor_outer_i + floors_to_pos3[segmentj->querypos3] > max_mismatches_allowed) {
	    /* Fail outer floor test */
	    /* floors->score[-index1interval][segmenti->querypos5] +floors->score[segmentj->querypos3][query_lastpos+index1interval] */

	    debug4s(printf("too many mismatches, outer floor = %d+%d=%d > %d\n",
			   floors->scorefrom[-index1interval][segmenti->querypos5],
			   floors->scorefrom[segmentj->querypos3][query_lastpos+index1interval],
			   floors->scorefrom[-index1interval][segmenti->querypos5] +
			   floors->scorefrom[segmentj->querypos3][query_lastpos+index1interval],
			   max_mismatches_allowed));

	  } else {
	    /* Apply leftmost/rightmost test */
	    if (segmenti->leftmost < 0) {
	      nmismatches_left = Genome_mismatches_left(mismatch_positions_left,max_mismatches_allowed,
							query_compress,/*left*/segmenti_left,/*pos5*/0,/*pos3*/querylength,
							/*plusp*/true,genestrand);
	      segmenti->leftmost = (nmismatches_left == 0) ? 0 : mismatch_positions_left[nmismatches_left-1];
	      debug4s(printf("%d mismatches on left at:",nmismatches_left);
		      for (i = 0; i <= nmismatches_left; i++) {
			printf(" %d",mismatch_positions_left[i]);
		      }
		      printf("\n"));
	    }
	  
	    segmentj_left = segmentj->diagonal - querylength;
	    if (segmentj->rightmost < 0) {
	      nmismatches_right = Genome_mismatches_right(mismatch_positions_right,max_mismatches_allowed,
							  query_compress,/*left*/segmentj_left,/*pos5*/0,/*pos3*/querylength,
							  /*plusp*/true,genestrand);
	      segmentj->rightmost = (nmismatches_right == 0) ? 0 : mismatch_positions_right[nmismatches_right-1];
	      debug4s(printf("%d mismatches on right at:",nmismatches_right);
		      for (i = 0; i <= nmismatches_right; i++) {
			printf(" %d",mismatch_positions_right[i]);
		      }
		      printf("\n"));
	    }
	  
	    debug4s(printf("For a single splice, want leftmost %d > rightmost %d\n",segmenti->leftmost,segmentj->rightmost));
	    
	    if (segmenti->leftmost > segmentj->rightmost) {
	      /* Single splice is possible */

	      segmentj_acceptor_nknown = 0;
	      segmentj_antidonor_nknown = 0;
	      if ((j = segmentj->splicesites_i) >= 0) {
		/* Ends 2 (acceptor, plus) and 7 (antidonor, plus): mark known splice sites in segmentj */
		while (j < nsplicesites && splicesites[j] < segmentj->diagonal) {
		  if (splicetypes[j] == ACCEPTOR) {
		    debug4s(printf("Setting known acceptor %d for segmentj at %lu\n",j,splicesites[j]));
		    segmentj_acceptor_knownpos[segmentj_acceptor_nknown] = splicesites[j] - segmentj_left;
		    segmentj_acceptor_knowni[segmentj_acceptor_nknown++] = j;
		  } else if (splicetypes[j] == ANTIDONOR) {
		    debug4s(printf("Setting known antidonor %d for segmentj at %lu\n",j,splicesites[j]));
		    segmentj_antidonor_knownpos[segmentj_antidonor_nknown] = splicesites[j] - segmentj_left;
		    segmentj_antidonor_knowni[segmentj_antidonor_nknown++] = j;
		  }
		  j++;
		}
	      }
	      segmentj_acceptor_knownpos[segmentj_acceptor_nknown] = MAX_READLENGTH;
	      segmentj_antidonor_knownpos[segmentj_antidonor_nknown] = MAX_READLENGTH;


	      debug4s(printf("  => checking for single splice: Splice_solve_single_plus\n"));
	      hits = Splice_solve_single(&(*found_score),&nhits_local,hits,&(*lowprob),
					 &segmenti->usedp,&segmentj->usedp,
					 /*segmenti_left*/segmenti->diagonal - querylength,
					 /*segmentj_left*/segmentj->diagonal - querylength,
					 segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
					 segmentj->chrnum,segmentj->chroffset,segmentj->chrhigh,segmentj->chrlength,
					 querylength,query_compress,
					 segmenti_donor_knownpos,segmentj_acceptor_knownpos,
					 segmentj_antidonor_knownpos,segmenti_antiacceptor_knownpos,
					 segmenti_donor_knowni,segmentj_acceptor_knowni,
					 segmentj_antidonor_knowni,segmenti_antiacceptor_knowni,
					 segmenti_donor_nknown,segmentj_acceptor_nknown,
					 segmentj_antidonor_nknown,segmenti_antiacceptor_nknown,
					 splicing_penalty,max_mismatches_allowed,
					 first_read_p,/*plusp*/true,genestrand,subs_or_indels_p,
					 /*sarrayp*/false);
	    }
	  }
	}
      }
    }
  }

  debug(printf("Finished find_singlesplices_plus with %d hits and %d lowprob\n",
	       List_length(hits),List_length(*lowprob)));

  return hits;
}


static List_T
find_singlesplices_minus (int *found_score, List_T hits, List_T *lowprob,
			  Segment_T *minus_spliceable, int minus_nspliceable,
			  Floors_T floors, int querylength, int query_lastpos, Compress_T query_compress /* expecting rev */,
			  int splicing_penalty, int max_mismatches_allowed, bool first_read_p, int genestrand,
			  bool subs_or_indels_p) {
#ifdef DEBUG4S
  int i;
#endif
  int j;
  Segment_T segmenti, segmentj, segmentj_end, *ptr;
  Univcoord_T segmenti_left, segmentj_left;
  int mismatch_positions_left[MAX_READLENGTH], mismatch_positions_right[MAX_READLENGTH];
  int nmismatches_left, nmismatches_right;
  int segmenti_donor_knownpos[MAX_READLENGTH+1], segmentj_acceptor_knownpos[MAX_READLENGTH+1],
    segmentj_antidonor_knownpos[MAX_READLENGTH+1], segmenti_antiacceptor_knownpos[MAX_READLENGTH+1];
  int segmenti_donor_knowni[MAX_READLENGTH+1], segmentj_acceptor_knowni[MAX_READLENGTH+1],
    segmentj_antidonor_knowni[MAX_READLENGTH+1], segmenti_antiacceptor_knowni[MAX_READLENGTH+1];
  int segmenti_donor_nknown, segmentj_acceptor_nknown,
    segmentj_antidonor_nknown, segmenti_antiacceptor_nknown;

  Chrpos_T max_distance;

  int floor_outer_i;
  int *floors_from_neg3, *floors_to_pos3;
  int nhits_local = 0;


  debug4s(printf("*** Starting find_singlesplices_minus on %d spliceable segments ***\n",minus_nspliceable));
  /* debug(printf("Initially have %d hits\n",List_length(hits))); */

  floors_from_neg3 = floors->scorefrom[-index1interval];
  floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

  for (ptr = minus_spliceable; ptr < &(minus_spliceable[minus_nspliceable]); ptr++) {
    segmenti = *ptr;
    debug4s(printf("minus_spliceable segmenti at diagonal %u\n",segmenti->diagonal));
    if (1 || segmenti->diagonal < (Univcoord_T) -1) { /* No markers were stored in spliceable */
      segmenti_left = segmenti->diagonal - querylength;
      floor_outer_i = floors_to_pos3[segmenti->querypos3];

      segmenti_antiacceptor_nknown = 0;
      segmenti_donor_nknown = 0;
      max_distance = shortsplicedist;

      if ((j = segmenti->splicesites_i) >= 0) {
	/* Ends 4 and 5: mark known splice sites in segmenti */
	while (j < nsplicesites && splicesites[j] < segmenti->diagonal) {
	  if (splicetypes[j] == ANTIACCEPTOR) {
	    debug4s(printf("Setting known antiacceptor %d for segmenti at %lu\n",j,splicesites[j]));
	    segmenti_antiacceptor_knownpos[segmenti_antiacceptor_nknown] = splicesites[j] - segmenti_left;
	    segmenti_antiacceptor_knowni[segmenti_antiacceptor_nknown++] = j;
	  } else if (splicetypes[j] == DONOR) {
	    debug4s(printf("Setting known donor %d for segmenti at %lu\n",j,splicesites[j]));
	    segmenti_donor_knownpos[segmenti_donor_nknown] = splicesites[j] - segmenti_left;
	    segmenti_donor_knowni[segmenti_donor_nknown++] = j;
	  }

	  /* This computation was already made in identify_all_segments */
	  if (splicedists[j] > max_distance) {
	    debug4s(printf("Setting max_distance for known %d to be %u\n",j,splicedists[j]));
	    max_distance = splicedists[j];
	  }

	  j++;
	}
      }
      segmenti_antiacceptor_knownpos[segmenti_antiacceptor_nknown] = MAX_READLENGTH;
      segmenti_donor_knownpos[segmenti_donor_nknown] = MAX_READLENGTH;

      /* Identify potential segmentj for segmenti */
      segmentj_end = segmenti+1;
      while (
#ifdef NO_MARKER_SEGMENTS
	     segmentj_end < &(minus_segments[minus_nsegments]) && segmentj_end->chrnum == segmenti->chrnum &&
#endif
	     segmentj_end->diagonal <= segmenti->diagonal + max_distance) {
	segmentj_end++;
      }
      if (segmentj_end - segmenti > MAX_LOCALSPLICING_POTENTIAL + 1) {    /* segmentj_end - segmenti - 1 > MAX_LOCALSPLICING_POTENTIAL */
	/* Too many to check */
	segmentj_end = segmenti+1 + MAX_LOCALSPLICING_POTENTIAL;
      }

      for (segmentj = segmenti+1; segmentj < segmentj_end; segmentj++) {
	debug4s(printf("minus local?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d => ",
		       segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
		       segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	/* j5 j3 i5 i3 */
	assert(segmenti->diagonal < segmentj->diagonal);
	if (segmentj->querypos3 >= segmenti->querypos5) {
	  /* Fail querypos test */
	  debug4s(printf("Bad querypos\n"));

	} else if (segmenti->diagonal + min_intronlength > segmentj->diagonal) {
	  /* Too short to be an intron */
	  debug4s(printf("Too short\n"));

	} else {
	  segmenti->right_splice_p = true;
	  segmentj->left_splice_p = true;
	  if (floors_from_neg3[segmentj->querypos5] + floor_outer_i > max_mismatches_allowed) {
	    /* Fail outer floor test */
	    /* floors->score[-index1interval][segmentj->querypos5] + floors->score[segmenti->querypos3][query_lastpos+index1interval] */;
	  
	    debug4s(printf("too many mismatches, outer floor = %d+%d=%d > %d\n",
			   floors->scorefrom[-index1interval][segmentj->querypos5],
			   floors->scorefrom[segmenti->querypos3][query_lastpos+index1interval],
			   floors->scorefrom[-index1interval][segmentj->querypos5] +
			   floors->scorefrom[segmenti->querypos3][query_lastpos+index1interval],
			   max_mismatches_allowed));

	  } else {
	    /* Apply leftmost/rightmost test */
	    if (segmenti->leftmost < 0) {
	      nmismatches_left = Genome_mismatches_left(mismatch_positions_left,max_mismatches_allowed,
							query_compress,/*left*/segmenti_left,/*pos5*/0,/*pos3*/querylength,
							/*plusp*/false,genestrand);
	      segmenti->leftmost = (nmismatches_left == 0) ? 0 : mismatch_positions_left[nmismatches_left-1];
	      debug4s(printf("%d mismatches on left at:",nmismatches_left);
		      for (i = 0; i <= nmismatches_left; i++) {
			printf(" %d",mismatch_positions_left[i]);
		      }
		      printf("\n"));
	    }

	    segmentj_left = segmentj->diagonal - querylength;
	    if (segmentj->rightmost < 0) {
	      nmismatches_right = Genome_mismatches_right(mismatch_positions_right,max_mismatches_allowed,
							  query_compress,/*left*/segmentj_left,/*pos5*/0,/*pos3*/querylength,
							  /*plusp*/false,genestrand);
	      segmentj->rightmost = (nmismatches_right == 0) ? 0 : mismatch_positions_right[nmismatches_right-1];
	      debug4s(printf("%d mismatches on right at:",nmismatches_right);
		      for (i = 0; i <= nmismatches_right; i++) {
			printf(" %d",mismatch_positions_right[i]);
		      }
		      printf("\n"));
	    }

	    debug4s(printf("For a single splice, want leftmost %d > rightmost %d\n",segmenti->leftmost,segmentj->rightmost));

	    if (segmenti->leftmost > segmentj->rightmost) {
	      /* Single splice is possible */

	      segmentj_antidonor_nknown = 0;
	      segmentj_acceptor_nknown = 0;
	      if ((j = segmentj->splicesites_i) >= 0) {
		/* Ends 3 and 6: mark known splice sites in segmentj */
		while (j < nsplicesites && splicesites[j] < segmentj->diagonal) {
		  if (splicetypes[j] == ANTIDONOR) {
		    debug4s(printf("Setting known antidonor %d for segmentj at %lu\n",j,splicesites[j]));
		    segmentj_antidonor_knownpos[segmentj_antidonor_nknown] = splicesites[j] - segmentj_left;
		    segmentj_antidonor_knowni[segmentj_antidonor_nknown++] = j;
		  } else if (splicetypes[j] == ACCEPTOR) {
		    debug4s(printf("Setting known acceptor %d for segmentj at %lu\n",j,splicesites[j]));
		    segmentj_acceptor_knownpos[segmentj_acceptor_nknown] = splicesites[j] - segmentj_left;
		    segmentj_acceptor_knowni[segmentj_acceptor_nknown++] = j;
		  }
		  j++;
		}
	      }
	      segmentj_antidonor_knownpos[segmentj_antidonor_nknown] = MAX_READLENGTH;
	      segmentj_acceptor_knownpos[segmentj_acceptor_nknown] = MAX_READLENGTH;

	      debug4s(printf("  => checking for single splice: Splice_solve_single_minus\n"));
	      hits = Splice_solve_single(&(*found_score),&nhits_local,hits,&(*lowprob),
					 &segmenti->usedp,&segmentj->usedp,
					 /*segmenti_left*/segmenti->diagonal - querylength,
					 /*segmentj_left*/segmentj->diagonal - querylength,
					 segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
					 segmentj->chrnum,segmentj->chroffset,segmentj->chrhigh,segmentj->chrlength,
					 querylength,query_compress,					 
					 segmenti_donor_knownpos,segmentj_acceptor_knownpos,
					 segmentj_antidonor_knownpos,segmenti_antiacceptor_knownpos,
					 segmenti_donor_knowni,segmentj_acceptor_knowni,
					 segmentj_antidonor_knowni,segmenti_antiacceptor_knowni,
					 segmenti_donor_nknown,segmentj_acceptor_nknown,
					 segmentj_antidonor_nknown,segmenti_antiacceptor_nknown,
					 splicing_penalty,max_mismatches_allowed,
					 first_read_p,/*plusp*/false,genestrand,subs_or_indels_p,
					 /*sarrayp*/false);
	    }
	  }
	}
      }
    }
  }

  debug(printf("Finished find_singlesplices_minus with %d hits and %d lowprob\n",
	       List_length(hits),List_length(*lowprob)));

  return hits;
}


static List_T
find_doublesplices (int *found_score, List_T hits, List_T *lowprob,
		    Segment_T *spliceable, int nspliceable, struct Segment_T *segments, 
		    char *queryptr, Floors_T floors,
		    int querylength, int query_lastpos, Compress_T query_compress,
		    Chrpos_T max_distance, int splicing_penalty, int min_shortend,
		    int max_mismatches_allowed, bool pairedp, bool first_read_p,
		    bool plusp, int genestrand, bool subs_or_indels_p) {
  int j, j1, j2, joffset, k, l, jj;
  
  Segment_T segmenti, segmentj, segmentm, segmenti_start, segmentj_end,
    potentiali[MAX_LOCALSPLICING_POTENTIAL], potentialj[MAX_LOCALSPLICING_POTENTIAL], *ptr;
  Univcoord_T segmenti_left, segmentj_left, segmentm_left;
  int segmenti_donor_knownpos[MAX_READLENGTH+1], segmentj_acceptor_knownpos[MAX_READLENGTH+1],
    segmentj_antidonor_knownpos[MAX_READLENGTH+1], segmenti_antiacceptor_knownpos[MAX_READLENGTH+1],
    segmentm_donor_knownpos[MAX_READLENGTH+1], segmentm_acceptor_knownpos[MAX_READLENGTH+1],
    segmentm_antidonor_knownpos[MAX_READLENGTH+1], segmentm_antiacceptor_knownpos[MAX_READLENGTH+1];
  int segmenti_donor_knowni[MAX_READLENGTH+1], segmentj_acceptor_knowni[MAX_READLENGTH+1],
    segmentj_antidonor_knowni[MAX_READLENGTH+1], segmenti_antiacceptor_knowni[MAX_READLENGTH+1],
    segmentm_donor_knowni[MAX_READLENGTH+1], segmentm_acceptor_knowni[MAX_READLENGTH+1],
    segmentm_antidonor_knowni[MAX_READLENGTH+1], segmentm_antiacceptor_knowni[MAX_READLENGTH+1];
  int segmenti_donor_nknown, segmentj_acceptor_nknown,
    segmentj_antidonor_nknown, segmenti_antiacceptor_nknown,
    segmentm_donor_nknown, segmentm_acceptor_nknown,
    segmentm_antidonor_nknown, segmentm_antiacceptor_nknown;

  Intlist_T splicesites_i_left, splicesites_i_right;
  Intlist_T nmismatches_list_left, nmismatches_list_right;
  bool ambp_left, ambp_right;
  bool sensep;
  int sensedir;
  int *floors_from_neg3, *floors_to_pos3;

  int nmismatches_shortexon_left, nmismatches_shortexon_middle, nmismatches_shortexon_right;
  int amb_nmatches_donor, amb_nmatches_acceptor;
  int best_left_j, best_right_j;
  bool shortexon_orig_plusp, shortexon_orig_minusp, saw_antidonor_p, saw_acceptor_p;
  int leftpos, rightpos;
  Substring_T donor, acceptor, shortexon;

  int nhits_local = 0, npotential_left, npotential_right;

  
  debug(printf("*** Starting find_known_doublesplices on %d segments ***\n",nspliceable));
  debug(printf("Initially have %d hits\n",List_length(hits)));

  floors_from_neg3 = floors->scorefrom[-index1interval];
  floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

  for (ptr = spliceable; ptr < &(spliceable[nspliceable]); ptr++) {
    segmentm = *ptr;
    if (1 || segmentm->diagonal < (Univcoord_T) -1) { /* No markers were stored in spliceable */
      segmentm_left = segmentm->diagonal - querylength;
	
      shortexon_orig_plusp = shortexon_orig_minusp = false;
      saw_acceptor_p = saw_antidonor_p = false;

      segmentm_donor_nknown = 0;
      segmentm_acceptor_nknown = 0;
      segmentm_antidonor_nknown = 0;
      segmentm_antiacceptor_nknown = 0;

      if ((joffset = segmentm->splicesites_i) >= 0) {
	j = joffset;
	while (j < nsplicesites && splicesites[j] < segmentm->diagonal) {
	  if (splicetypes[j] == DONOR) {
	    debug4k(printf("Setting known donor %d for segmentm at %lu\n",j,splicesites[j]));
	    segmentm_donor_knownpos[segmentm_donor_nknown] = splicesites[j] - segmentm_left;
	    segmentm_donor_knowni[segmentm_donor_nknown++] = j;
	    if (saw_acceptor_p == true) {
	      /* acceptor...donor */
	      shortexon_orig_plusp = true;
	    }
	  } else if (splicetypes[j] == ANTIACCEPTOR) {
	    debug4k(printf("Setting known antiacceptor %d for segmentm at %lu\n",j,splicesites[j]));
	    segmentm_antiacceptor_knownpos[segmentm_antiacceptor_nknown] = splicesites[j] - segmentm_left;
	    segmentm_antiacceptor_knowni[segmentm_antiacceptor_nknown++] = j;
	    if (saw_antidonor_p == true) {
	      /* antidonor...antiacceptor */
	      shortexon_orig_minusp = true;
	    }
	  } else if (splicetypes[j] == ACCEPTOR) {
	    debug4k(printf("Saw known acceptor at %lu\n",splicesites[j]));
	    segmentm_acceptor_knownpos[segmentm_acceptor_nknown] = splicesites[j] - segmentm_left;
	    segmentm_acceptor_knowni[segmentm_acceptor_nknown++] = j;
	    saw_acceptor_p = true;
	  } else if (splicetypes[j] == ANTIDONOR) {
	    debug4k(printf("Saw known antidonor at %lu\n",splicesites[j]));
	    segmentm_antidonor_knownpos[segmentm_antidonor_nknown] = splicesites[j] - segmentm_left;
	    segmentm_antidonor_knowni[segmentm_antidonor_nknown++] = j;
	    saw_antidonor_p = true;
	  }
	  j++;
	}
      }


      /* Novel splicing.  Do not alter j. */
      /* Still necessary to check segmentm querypos to achieve speed */
      if (novelsplicingp &&
	  segmentm->querypos3 >= index1part && segmentm->querypos5 <= query_lastpos - index1part &&
	  segmentm->left_splice_p == true && segmentm->right_splice_p == true) {
	debug4d(printf("segment diagonal %lu, querypos %d..%d\n",
		       segmentm->diagonal,segmentm->querypos5,segmentm->querypos3));

	/* Identify potential segmenti for segmentm */
	segmenti_start = segmentm-1;
	while (
	       /* Cannot use marker segments going leftward */
	       segmenti_start >= &(segments[0]) &&
	       segmenti_start->diagonal < (Univcoord_T) -1 && /* Needs to be next criterion, since we initialize only segments[0]->diagonal */
	       segmenti_start->chrnum == segmentm->chrnum &&
	       segmentm->diagonal <= segmenti_start->diagonal + max_distance) {
	  segmenti_start--;
	}
	if (segmentm - segmenti_start > MAX_LOCALSPLICING_POTENTIAL + 1) { /* segmentm - segmenti_start - 1 > MAX_LOCALSPLICING_POTENTIAL */
	  /* Too many to check */
	  segmenti_start = segmentm-1 - MAX_LOCALSPLICING_POTENTIAL;
	}

	npotential_left = 0;
	for (segmenti = segmentm-1; segmenti > segmenti_start; segmenti--) {
	  debug4d(printf("local left?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d\n",
			 segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
			 segmentm->diagonal,segmentm->querypos5,segmentm->querypos3));
	  /* i5 i3 m5 m3 */
	  assert(segmenti->diagonal < segmentm->diagonal);
	  if (segmenti->leftmost < 0) {
	    /* Failed outer floor test in find_singlesplices */
	  } else if (plusp == true && segmenti->querypos3 >= segmentm->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (plusp == false && segmentm->querypos3 >= segmenti->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (segmenti->diagonal + min_intronlength > segmentm->diagonal) {
	    debug4d(printf("Too short\n"));
	  } else {
	    potentiali[npotential_left++] = segmenti;
	    debug4d(printf("Potential left #%d: %lu\n",npotential_left,segmenti->diagonal));
	  }
	}

	/* Identify potential segmentj for segmentm */
	segmentj_end = segmentm+1;
	while (
#ifdef NO_MARKER_SEGMENTS
	       segmentj_end < &(segments[nsegments]) && segmentj_end->chrnum == segmentm->chrnum &&
#endif
	       segmentj_end->diagonal <= segmentm->diagonal + max_distance) {
	  segmentj_end++;
	}
	if (segmentj_end - segmentm > MAX_LOCALSPLICING_POTENTIAL + 1) {    /* segmentj_end - segmentm - 1 > MAX_LOCALSPLICING_POTENTIAL */
	  /* Too many to check */
	  segmentj_end = segmentm+1 + MAX_LOCALSPLICING_POTENTIAL;
	}

	npotential_right = 0;
	for (segmentj = segmentm+1; segmentj < segmentj_end; segmentj++) {
	  debug4d(printf("local right?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d\n",
			 segmentm->diagonal,segmentm->querypos5,segmentm->querypos3,
			 segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	  /* m5 m3 j5 j3 */
	  assert(segmentm->diagonal < segmentj->diagonal);
	  if (segmentj->rightmost < 0) {
	    /* Failed outer floor test in find_singlesplices */
	  } else if (plusp == true && segmentm->querypos3 >= segmentj->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (plusp == false && segmentj->querypos3 >= segmentm->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (segmentm->diagonal + min_intronlength > segmentj->diagonal) {
	    debug4d(printf("Too short\n"));
	  } else {
	    potentialj[npotential_right++] = segmentj;
	    debug4d(printf("Potential right #%d: %lu\n",npotential_right,segmentj->diagonal));
	  }
	}

	if (npotential_left > 0 && npotential_right > 0) {
	  segmentm_donor_knownpos[segmentm_donor_nknown] = MAX_READLENGTH;
	  segmentm_acceptor_knownpos[segmentm_acceptor_nknown] = MAX_READLENGTH;
	  segmentm_antidonor_knownpos[segmentm_antidonor_nknown] = MAX_READLENGTH;
	  segmentm_antiacceptor_knownpos[segmentm_antiacceptor_nknown] = MAX_READLENGTH;

	  for (k = 0; k < npotential_left; k++) {
	    segmenti = potentiali[k];
	    segmenti_left = segmenti->diagonal - querylength;

	    /* Set known sites for segmenti */
	    segmenti_donor_nknown = 0;
	    segmenti_antiacceptor_nknown = 0;
	    if ((jj = segmenti->splicesites_i) >= 0) {
	      while (jj < nsplicesites && splicesites[jj] < segmenti->diagonal) {
		if (splicetypes[jj] == DONOR) {
		  debug4d(printf("Setting known donor %d for segmenti at %lu\n",jj,splicesites[jj]));
		  segmenti_donor_knownpos[segmenti_donor_nknown] = splicesites[jj] - segmenti_left;
		  segmenti_donor_knowni[segmenti_donor_nknown++] = jj;
		} else if (splicetypes[jj] == ANTIACCEPTOR) {
		  debug4d(printf("Setting known antiacceptor %d for segmenti at %lu\n",jj,splicesites[jj]));
		  segmenti_antiacceptor_knownpos[segmenti_antiacceptor_nknown] = splicesites[jj] - segmenti_left;
		  segmenti_antiacceptor_knowni[segmenti_antiacceptor_nknown++] = jj;
		}
		jj++;
	      }
	    }
	    segmenti_donor_knownpos[segmenti_donor_nknown] = MAX_READLENGTH;
	    segmenti_antiacceptor_knownpos[segmenti_antiacceptor_nknown] = MAX_READLENGTH;
	      
	    for (l = 0; l < npotential_right; l++) {
	      segmentj = potentialj[l];

	      debug4d(printf("Doublesplice span test (%d mismatches allowed): %d mismatches found from leftmost %d to j.rightmost %d\n",
			     max_mismatches_allowed,
			     Genome_count_mismatches_substring(query_compress,segmentm_left,
							       /*pos5*/segmenti->leftmost,/*pos3*/segmentj->rightmost,
							       plusp,genestrand),
			     segmenti->leftmost,segmentj->rightmost));
	    
	      if (segmenti->leftmost >= segmentj->rightmost) {
		debug4d(printf("Double splice is not possible with pos5 %d > pos3 %d\n",
			       segmenti->leftmost,segmentj->rightmost));
	      } else if (Genome_count_mismatches_limit(query_compress,segmentm_left,
						       /*pos5*/segmenti->leftmost,/*pos3*/segmentj->rightmost,
						       max_mismatches_allowed,plusp,genestrand) <= max_mismatches_allowed) {
		debug4d(printf("Double splice is possible\n"));
		segmentj_left = segmentj->diagonal - querylength;

		/* Set known sites for segmentj */
		segmentj_acceptor_nknown = 0;
		segmentj_antidonor_nknown = 0;
		if ((jj = segmentj->splicesites_i) >= 0) {
		  while (jj < nsplicesites && splicesites[jj] < segmentj->diagonal) {
		    if (splicetypes[jj] == ACCEPTOR) {
		      debug4d(printf("Setting known acceptor %d for segmentj at %lu\n",jj,splicesites[jj]));
		      segmentj_acceptor_knownpos[segmentj_acceptor_nknown] = splicesites[jj] - segmentj_left;
		      segmentj_acceptor_knowni[segmentj_acceptor_nknown++] = jj;
		    } else if (splicetypes[jj] == ANTIDONOR) {
		      debug4d(printf("Setting known antidonor %d for segmentj at %lu\n",jj,splicesites[jj]));
		      segmentj_antidonor_knownpos[segmentj_antidonor_nknown] = splicesites[jj] - segmentj_left;
		      segmentj_antidonor_knowni[segmentj_antidonor_nknown++] = jj;
		    }
		    jj++;
		  }
		}
		segmentj_acceptor_knownpos[segmentj_acceptor_nknown] = MAX_READLENGTH;
		segmentj_antidonor_knownpos[segmentj_antidonor_nknown] = MAX_READLENGTH;

		debug4d(printf("  => checking for double splice: Splice_solve_double\n"));
		hits = Splice_solve_double(&(*found_score),&nhits_local,hits,&(*lowprob),
					   &segmenti->usedp,&segmentm->usedp,&segmentj->usedp,
					   /*segmenti_left*/segmenti->diagonal - querylength,
					   /*segmentm_left*/segmentm->diagonal - querylength,
					   /*segmentj_left*/segmentj->diagonal - querylength,
					   segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
					   segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength,
					   segmentj->chrnum,segmentj->chroffset,segmentj->chrhigh,segmentj->chrlength,
					   querylength,query_compress,
					   segmenti_donor_knownpos,segmentm_acceptor_knownpos,segmentm_donor_knownpos,segmentj_acceptor_knownpos,
					   segmentj_antidonor_knownpos,segmentm_antiacceptor_knownpos,segmentm_antidonor_knownpos,segmenti_antiacceptor_knownpos,
					   segmenti_donor_knowni,segmentm_acceptor_knowni,segmentm_donor_knowni,segmentj_acceptor_knowni,
					   segmentj_antidonor_knowni,segmentm_antiacceptor_knowni,segmentm_antidonor_knowni,segmenti_antiacceptor_knowni,
					   segmenti_donor_nknown,segmentm_acceptor_nknown,segmentm_donor_nknown,segmentj_acceptor_nknown,
					   segmentj_antidonor_nknown,segmentm_antiacceptor_nknown,segmentm_antidonor_nknown,segmenti_antiacceptor_nknown,
					   splicing_penalty,max_mismatches_allowed,plusp,genestrand,subs_or_indels_p,/*sarrayp*/false);
	      }
	    }
	  }
	}
      }

      /* Short exon using known splicing, originally on plus strand */
      if (shortexon_orig_plusp == true) {
	debug4k(printf("Short exon candidate, orig_plusp.  Saw short exon acceptor...donor on segment i\n"));
	sensep = (plusp == true) ? true : false;
	sensedir = (plusp == true) ? SENSE_FORWARD : SENSE_ANTI;

	for (j1 = joffset; j1 < j; j1++) {
	  if (splicetypes[j1] == ACCEPTOR) {
	    leftpos = splicesites[j1] - segmentm_left;
	    debug4k(printf("  Doing Splicetrie_find_left from leftpos %d (plus)\n",leftpos));
	    if ((splicesites_i_left =
		 Splicetrie_find_left(&nmismatches_shortexon_left,&nmismatches_list_left,j1,
				      /*origleft*/segmentm_left,/*pos5*/0,/*pos3*/leftpos,segmentm->chroffset,
				      query_compress,queryptr,querylength,max_mismatches_allowed,plusp,genestrand,
				      /*collect_all_p*/pairedp == true && first_read_p != plusp)) != NULL) {
	      ambp_left = (leftpos < min_shortend || Intlist_length(splicesites_i_left) > 1) ? true : false;

	      for (j2 = j1 + 1; j2 < j; j2++) {
		if (splicetypes[j2] == DONOR && splicesites[j2] > splicesites[j1]) {
		  rightpos = splicesites[j2] - segmentm_left;
		  debug4k(printf("  Doing Splicetrie_find_right from rightpos %d (plus)\n",rightpos));
		  if ((nmismatches_shortexon_middle =
		       Genome_count_mismatches_substring(query_compress,segmentm_left,/*pos5*/leftpos,/*pos3*/rightpos,
							 plusp,genestrand)) <= max_mismatches_allowed - nmismatches_shortexon_left &&
		      (splicesites_i_right =
		       Splicetrie_find_right(&nmismatches_shortexon_right,&nmismatches_list_right,j2,
					     /*origleft*/segmentm_left,/*pos5*/rightpos,/*pos3*/querylength,segmentm->chrhigh,
					     query_compress,queryptr,
					     max_mismatches_allowed - nmismatches_shortexon_left - nmismatches_shortexon_middle,
					     plusp,genestrand,/*collect_all_p*/pairedp == true && first_read_p == plusp)) != NULL) {
		    ambp_right = (querylength - rightpos < min_shortend || Intlist_length(splicesites_i_right) > 1) ? true : false;

		    debug4k(printf("  donor %s ... acceptor %d (%lu) ... donor %d (%lu) ... acceptor %s: %d + %d + %d mismatches\n",
				   Intlist_to_string(splicesites_i_left),j1,splicesites[j1],j2,splicesites[j2],Intlist_to_string(splicesites_i_right),
				   nmismatches_shortexon_left,nmismatches_shortexon_middle,nmismatches_shortexon_right));

		    if (ambp_left == true && ambp_right == true) {
		      shortexon = Substring_new_shortexon(j1,j2,/*joffset*/0,/*acceptor_pos*/leftpos,/*donor_pos*/rightpos,
							  nmismatches_shortexon_middle,
							  /*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/true,/*donor_ambp*/true,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);
		      if (shortexon != NULL) {
			debug4k(printf("New one-third shortexon at left %lu\n",segmentm_left));
			amb_nmatches_donor = leftpos - nmismatches_shortexon_left;
			amb_nmatches_acceptor = querylength - rightpos - nmismatches_shortexon_right;
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),/*donor*/NULL,/*acceptor*/NULL,shortexon,
									       /*acceptor_distance*/0U,/*donor_distance*/0U,
									       amb_nmatches_donor,amb_nmatches_acceptor,
									       /*ambi_left*/splicesites_i_left,/*ambi_right*/splicesites_i_right,
									       nmismatches_list_left,nmismatches_list_right,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }

		    } else if (ambp_left == true && ambp_right == false) {
		      debug4k(printf("ambp_left true, ambp_right false\n"));
		      best_right_j = Intlist_head(splicesites_i_right);

		      debug4k(printf("shortexon with amb_acceptor at %d (%lu) ... donor at %d (%lu)\n",
				     j1,splicesites[j1],j2,splicesites[j2]));
		      shortexon = Substring_new_shortexon(j1,j2,/*joffset*/0,/*acceptor_pos*/leftpos,/*donor_pos*/rightpos,
							  nmismatches_shortexon_middle,
							  /*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/true,/*donor_ambp*/false,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      debug4k(printf("acceptor at %d (%lu)\n",best_right_j,splicesites[best_right_j]));
		      acceptor = Substring_new_acceptor(best_right_j,/*joffset*/0,/*splice_pos*/rightpos,nmismatches_shortexon_right,
							/*prob*/2.0,/*left*/splicesites[best_right_j]-rightpos,
							query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
							segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      if (shortexon == NULL || acceptor == NULL) {
			if (shortexon != NULL) Substring_free(&shortexon);
			if (acceptor != NULL) Substring_free(&acceptor);
		      } else {
			debug4k(printf("ambp_left true, ambp_right false: New two-thirds shortexon at left %lu\n",segmentm_left));
			amb_nmatches_donor = leftpos - nmismatches_shortexon_left;
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),/*donor*/NULL,acceptor,shortexon,
									       /*acceptor_distance*/0U,
									       /*donor_distance*/splicesites[best_right_j]-splicesites[j2],
									       amb_nmatches_donor,/*amb_nmatches_acceptor*/0,
									       /*ambi_left*/splicesites_i_left,/*ambi_right*/NULL,
									       nmismatches_list_left,/*amb_nmismatches_right*/NULL,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }

		    } else if (ambp_left == false && ambp_right == true) {
		      debug4k(printf("ambp_left false, ambp_right true\n"));
		      best_left_j = Intlist_head(splicesites_i_left);

		      debug4k(printf("donor at %d (%lu)\n",best_left_j,splicesites[best_left_j]));
		      donor = Substring_new_donor(best_left_j,/*joffset*/0,/*splice_pos*/leftpos,nmismatches_shortexon_left,
						  /*prob*/2.0,/*left*/splicesites[best_left_j]-leftpos,
						  query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
						  segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      debug4k(printf("shortexon with acceptor at %d (%lu) ... amb_donor %d (%lu)\n",
				     j1,splicesites[j1],j2,splicesites[j2]));
		      shortexon = Substring_new_shortexon(j1,j2,/*joffset*/0,/*acceptor_pos*/leftpos,/*donor_pos*/rightpos,
							  nmismatches_shortexon_middle,
							  /*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/false,/*donor_ambp*/true,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      if (donor == NULL || shortexon == NULL) {
			if (donor != NULL) Substring_free(&donor);
			if (shortexon != NULL) Substring_free(&shortexon);
		      } else {
			amb_nmatches_acceptor = querylength - rightpos - nmismatches_shortexon_right;
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),donor,/*acceptor*/NULL,shortexon,
									       /*acceptor_distance*/splicesites[j1]-splicesites[best_left_j],
									       /*donor_distance*/0U,
									       /*amb_nmatches_donor*/0,amb_nmatches_acceptor,
									       /*ambi_left*/NULL,/*ambi_right*/splicesites_i_right,
									       /*amb_nmismatches_left*/NULL,nmismatches_list_right,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }


		    } else { /* ambp_left == false && ambp_right == false */
		      debug4k(printf("ambp_left false, ambp_right false\n"));
		      best_left_j = Intlist_head(splicesites_i_left);
		      best_right_j = Intlist_head(splicesites_i_right);
		      donor = Substring_new_donor(best_left_j,/*joffset*/0,/*splice_pos*/leftpos,nmismatches_shortexon_left,
						  /*prob*/2.0,/*left*/splicesites[best_left_j]-leftpos,
						  query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
						  segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      shortexon = Substring_new_shortexon(j1,j2,/*joffset*/0,/*acceptor_pos*/leftpos,/*donor_pos*/rightpos,
							  nmismatches_shortexon_middle,/*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/false,/*donor_ambp*/false,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);
		      
		      acceptor = Substring_new_acceptor(best_right_j,/*joffset*/0,/*splice_pos*/rightpos,nmismatches_shortexon_right,
							/*prob*/2.0,/*left*/splicesites[best_right_j]-rightpos,
							query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
							segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      if (donor == NULL || shortexon == NULL || acceptor == NULL) {
			if (donor != NULL) Substring_free(&donor);
			if (shortexon != NULL) Substring_free(&shortexon);
			if (acceptor != NULL) Substring_free(&acceptor);
		      } else {
			debug4k(printf("New shortexon at left %lu\n",segmentm_left));
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),donor,acceptor,shortexon,
									       /*acceptor_distance*/splicesites[j1]-splicesites[best_left_j],
									       /*donor_distance*/splicesites[best_right_j]-splicesites[j2],
									       /*amb_nmatches_donor*/0,/*amb_nmatches_acceptor*/0,
									       /*ambi_left*/NULL,/*ambi_right*/NULL,
									       /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }
		    }
		    Intlist_free(&nmismatches_list_right);
		    Intlist_free(&splicesites_i_right);
		  }
		}
	      }
	      Intlist_free(&nmismatches_list_left);
	      Intlist_free(&splicesites_i_left);
	    }
	  }
	}
	debug4k(printf("End of case 1\n"));
      }

      /* Short exon using known splicing, originally on minus strand */
      if (shortexon_orig_minusp == true) {
	debug4k(printf("Short exon candidate, orig_minusp.  Saw short exon antidonor...antiacceptor on segment i\n"));
	sensep = (plusp == true) ? false : true;
	sensedir = (plusp == true) ? SENSE_ANTI : SENSE_FORWARD;

	for (j1 = joffset; j1 < j; j1++) {
	  if (splicetypes[j1] == ANTIDONOR) {
	    leftpos = splicesites[j1] - segmentm_left;
	    debug4k(printf("  Doing Splicetrie_find_left from leftpos %d (minus)\n",leftpos));
	    if ((splicesites_i_left =
		 Splicetrie_find_left(&nmismatches_shortexon_left,&nmismatches_list_left,j1,
				      /*origleft*/segmentm_left,/*pos5*/0,/*pos3*/leftpos,segmentm->chroffset,
				      query_compress,queryptr,querylength,max_mismatches_allowed,plusp,genestrand,
				      /*collect_all_p*/pairedp == true && first_read_p != plusp)) != NULL) {
	      ambp_left = (leftpos < min_shortend || Intlist_length(splicesites_i_left) > 1) ? true : false;
	      
	      for (j2 = j1 + 1; j2 < j; j2++) {
		if (splicetypes[j2] == ANTIACCEPTOR && splicesites[j2] > splicesites[j1]) {
		  rightpos = splicesites[j2] - segmentm_left;
		  debug4k(printf("  Doing Splicetrie_find_right from rightpos %d (minus)\n",rightpos));
		  if ((nmismatches_shortexon_middle =
		       Genome_count_mismatches_substring(query_compress,segmentm_left,/*pos5*/leftpos,/*pos3*/rightpos,
							 plusp,genestrand)) <= max_mismatches_allowed - nmismatches_shortexon_left &&
		      (splicesites_i_right =
		       Splicetrie_find_right(&nmismatches_shortexon_right,&nmismatches_list_right,j2,
					     /*origleft*/segmentm_left,/*pos5*/rightpos,/*pos3*/querylength,segmentm->chrhigh,
					     query_compress,queryptr,
					     max_mismatches_allowed - nmismatches_shortexon_left - nmismatches_shortexon_middle,
					     plusp,genestrand,/*collect_all_p*/pairedp == true && first_read_p == plusp)) != NULL) {
		    ambp_right = (querylength - rightpos < min_shortend || Intlist_length(splicesites_i_right) > 1) ? true : false;

		    debug4k(printf("  antiacceptor %s ... antidonor %d (%lu) ... antiacceptor %d (%lu) ... antidonor %s: %d + %d + %d mismatches\n",
				   Intlist_to_string(splicesites_i_left),j1,splicesites[j1],j2,splicesites[j2],Intlist_to_string(splicesites_i_right),
				   nmismatches_shortexon_left,nmismatches_shortexon_middle,nmismatches_shortexon_right));

		    if (ambp_left == true && ambp_right == true) {
		      shortexon = Substring_new_shortexon(j2,j1,/*joffset*/0,/*acceptor_pos*/rightpos,/*donor_pos*/leftpos,nmismatches_shortexon_middle,
							  /*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/true,/*donor_ambp*/true,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);
		      if (shortexon != NULL) {
			debug4k(printf("New one-third shortexon at left %lu\n",segmentm_left));
			amb_nmatches_donor = querylength - rightpos - nmismatches_shortexon_right;
			amb_nmatches_acceptor = leftpos - nmismatches_shortexon_left;
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),/*donor*/NULL,/*acceptor*/NULL,shortexon,
									       /*acceptor_distance*/0U,/*donor_distance*/0U,
									       amb_nmatches_donor,amb_nmatches_acceptor,
									       /*ambi_left*/splicesites_i_left,/*ambi_right*/splicesites_i_right,
									       nmismatches_list_left,nmismatches_list_right,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }

		    } else if (ambp_left == true && ambp_right == false) {
		      debug4k(printf("ambp_left true, ambp_right false\n"));
		      best_right_j = Intlist_head(splicesites_i_right);

		      debug4k(printf("shortexon with amb_donor at %d (%lu) ... acceptor at %d (%lu)\n",
				     j1,splicesites[j1],j2,splicesites[j2]));
		      shortexon = Substring_new_shortexon(j2,j1,/*joffset*/0,/*acceptor_pos*/rightpos,/*donor_pos*/leftpos,nmismatches_shortexon_middle,
							  /*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/false,/*donor_ambp*/true,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      debug4k(printf("donor at %d (%lu)\n",best_right_j,splicesites[best_right_j]));
		      donor = Substring_new_donor(best_right_j,/*joffset*/0,/*splice_pos*/rightpos,nmismatches_shortexon_right,
						  /*prob*/2.0,/*left*/splicesites[best_right_j]-rightpos,
						  query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
						  segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      if (donor == NULL || shortexon == NULL) {
			if (donor != NULL) Substring_free(&donor);
			if (shortexon != NULL) Substring_free(&shortexon);
		      } else {
			amb_nmatches_acceptor = leftpos - nmismatches_shortexon_left;
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),donor,/*acceptor*/NULL,shortexon,
									       /*acceptor_distance*/splicesites[best_right_j]-splicesites[j2],
									       /*donor_distance*/0U,
									       /*amb_nmatches_donor*/0,amb_nmatches_acceptor,
									       /*ambi_left*/splicesites_i_left,/*ambi_right*/NULL,
									       nmismatches_list_left,/*amb_nmismatches_right*/NULL,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }

		    } else if (ambp_left == false && ambp_right == true) {
		      debug4k(printf("ambp_left false, ambp_right true\n"));
		      best_left_j = Intlist_head(splicesites_i_left);

		      debug4k(printf("acceptor at %d (%lu)\n",best_left_j,splicesites[best_left_j]));
		      acceptor = Substring_new_acceptor(best_left_j,/*joffset*/0,/*splice_pos*/leftpos,nmismatches_shortexon_left,
							/*prob*/2.0,/*left*/splicesites[best_left_j]-leftpos,
							query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
							segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      debug4k(printf("shortexon with donor at %d (%lu) ... amb_acceptor at %d (%lu)\n",
				     j2,splicesites[j2],j1,splicesites[j1]));
		      shortexon = Substring_new_shortexon(j2,j1,/*joffset*/0,/*acceptor_pos*/rightpos,/*donor_pos*/leftpos,nmismatches_shortexon_middle,
							  /*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/true,/*donor_ambp*/false,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      if (shortexon == NULL || acceptor == NULL) {
			if (shortexon != NULL) Substring_free(&shortexon);
			if (acceptor != NULL) Substring_free(&acceptor);
		      } else {
			debug4k(printf("ambp_left false, ambp_right true: New splice at left %lu\n",segmentm_left));
			amb_nmatches_donor = querylength - rightpos - nmismatches_shortexon_right;
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),/*donor*/NULL,acceptor,shortexon,
									       /*acceptor_distance*/0U,
									       /*donor_distance*/splicesites[j1]-splicesites[best_left_j],
									       amb_nmatches_donor,/*amb_nmatches_acceptor*/0,
									       /*ambi_left*/NULL,/*ambi_right*/splicesites_i_right,
									       /*amb_nmismatches_left*/NULL,nmismatches_list_right,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }

		    } else {  /* ambp_left == false && ambp_right == false */
		      best_left_j = Intlist_head(splicesites_i_left);
		      best_right_j = Intlist_head(splicesites_i_right);
		      acceptor = Substring_new_acceptor(best_left_j,/*joffset*/0,/*splice_pos*/leftpos,nmismatches_shortexon_left,
							/*prob*/2.0,/*left*/splicesites[best_left_j]-leftpos,
							query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
							segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      shortexon = Substring_new_shortexon(j2,j1,/*joffset*/0,/*acceptor_pos*/rightpos,/*donor_pos*/leftpos,
							  nmismatches_shortexon_middle,/*acceptor_prob*/2.0,/*donor_prob*/2.0,
							  /*left*/segmentm_left,query_compress,
							  querylength,plusp,genestrand,sensep,/*acceptor_ambp*/false,/*donor_ambp*/false,
							  segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      donor = Substring_new_donor(best_right_j,/*joffset*/0,/*splice_pos*/rightpos,nmismatches_shortexon_right,
						  /*prob*/2.0,/*left*/splicesites[best_right_j]-rightpos,
						  query_compress,querylength,plusp,genestrand,sensep,segmentm->chrnum,
						  segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength);

		      if (acceptor == NULL || shortexon == NULL || donor == NULL) {
			if (acceptor != NULL) Substring_free(&acceptor);
			if (shortexon != NULL) Substring_free(&shortexon);
			if (donor != NULL) Substring_free(&donor);
		      } else {
			debug4k(printf("New shortexon at left %lu\n",segmentm_left));
			segmentm->usedp = true;
			hits = List_push(hits,(void *) Stage3end_new_shortexon(&(*found_score),donor,acceptor,shortexon,
									       /*acceptor_distance*/splicesites[best_right_j]-splicesites[j2],
									       /*donor_distance*/splicesites[j1]-splicesites[best_left_j],
									       /*amb_nmatches_donor*/0,/*amb_nmatches_acceptor*/0,
									       /*ambi_left*/NULL,/*ambi_right*/NULL,
									       /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									       /*copy_donor_p*/false,/*copy_acceptor_p*/false,/*copy_shortexon_p*/false,
									       splicing_penalty,querylength,sensedir,/*sarrayp*/false));
#if 0
			if (++nhits_local >= MAX_LOCALSPLICING_HITS) {
			  Intlist_free(&nmismatches_list_right);
			  Intlist_free(&splicesites_i_right);
			  Intlist_free(&nmismatches_list_left);
			  Intlist_free(&splicesites_i_left);
			  return hits;
			}
#endif
		      }
		    }
		    Intlist_free(&nmismatches_list_right);
		    Intlist_free(&splicesites_i_right);
		  }
		}
	      }
	      Intlist_free(&nmismatches_list_left);
	      Intlist_free(&splicesites_i_left);
	    }
	  }
	}
	debug4k(printf("End of case 2\n"));
      }
      /* End of known splicesites, segment i */
    }
  }

  debug4k(printf("Finished find_known_doublesplices with %d hits\n",List_length(hits)));
  return hits;
}



#if 0
/* For known and novel doublesplices */
static List_T
find_doublesplices_old (int *found_score, List_T hits, struct Segment_T *segments, int nsegments,
			Floors_T floors, int querylength, int query_lastpos, Compress_T query_compress,
			Chrpos_T max_distance, int splicing_penalty,
			int max_mismatches_allowed, bool plusp, int genestrand) {
  int k, l;
  Segment_T segmenti, segmentj, segmentm, potentiali[MAX_LOCALSPLICING_POTENTIAL], potentialj[MAX_LOCALSPLICING_POTENTIAL];
  Univcoord_T segmenti_left, segmentj_left, segmentm_left;
  int segmenti_donor_knownpos[MAX_READLENGTH+1], segmentj_acceptor_knownpos[MAX_READLENGTH+1],
    segmentj_antidonor_knownpos[MAX_READLENGTH+1], segmenti_antiacceptor_knownpos[MAX_READLENGTH+1],
    segmentm_donor_knownpos[MAX_READLENGTH+1], segmentm_acceptor_knownpos[MAX_READLENGTH+1],
    segmentm_antidonor_knownpos[MAX_READLENGTH+1], segmentm_antiacceptor_knownpos[MAX_READLENGTH+1];
  int segmenti_donor_knowni[MAX_READLENGTH+1], segmentj_acceptor_knowni[MAX_READLENGTH+1],
    segmentj_antidonor_knowni[MAX_READLENGTH+1], segmenti_antiacceptor_knowni[MAX_READLENGTH+1],
    segmentm_donor_knowni[MAX_READLENGTH+1], segmentm_acceptor_knowni[MAX_READLENGTH+1],
    segmentm_antidonor_knowni[MAX_READLENGTH+1], segmentm_antiacceptor_knowni[MAX_READLENGTH+1];
  int segmenti_donor_nknown, segmentj_acceptor_nknown,
    segmentj_antidonor_nknown, segmenti_antiacceptor_nknown,
    segmentm_donor_nknown, segmentm_acceptor_nknown,
    segmentm_antidonor_nknown, segmentm_antiacceptor_nknown;
  
  int *floors_from_neg3, *floors_to_pos3;
  int nhits_local = 0, npotential_left, npotential_right;


  debug(printf("*** Starting find_doublesplices_old on %d segments with max_distance %u ***\n",
	       nsegments,max_distance));
  debug(printf("Initially have %d hits\n",List_length(hits)));

  assert(*nhits == List_length(hits));

  if (nsegments > 2) {
    floors_from_neg3 = floors->scorefrom[-index1interval];
    floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

    for (segmentm = &(segments[1]); segmentm < &(segments[nsegments]); segmentm++) {
      if (segmentm->diagonal < (Univcoord_T) -1 &&
	  segmentm->querypos3 >= index1part &&
	  segmentm->querypos5 <= query_lastpos - index1part) {
	debug4d(printf("segment diagonal %lu, querypos %d..%d\n",
		       segmentm->diagonal,segmentm->querypos5,segmentm->querypos3));

	npotential_left = 0;
	for (segmenti = segmentm-1;
	     /* Cannot use marker segments going leftward */
	     segmenti >= &(segments[0]) &&
	       segmenti->diagonal < (Univcoord_T) -1 && /* Needs to be next criterion, since we initialize only segments[0]->diagonal  */
	       segmenti->chrnum == segmentm->chrnum &&
	       segmentm->diagonal <= segmenti->diagonal + max_distance &&
	       npotential_left < MAX_LOCALSPLICING_POTENTIAL; segmenti--) {
	  debug4d(printf("local left?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d\n",
			 segmenti->diagonal,segmenti->querypos5,segmenti->querypos3,
			 segmentm->diagonal,segmentm->querypos5,segmentm->querypos3));
	  /* i5 i3 m5 m3 */
	  assert(segmenti->diagonal < segmentm->diagonal);
	  if (segmenti->leftmost < 0) {
	    /* Failed outer floor test in find_singlesplices */
	  } else if (plusp == true && segmenti->querypos3 >= segmentm->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (plusp == false && segmentm->querypos3 >= segmenti->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (segmenti->diagonal + min_intronlength > segmentm->diagonal) {
	    debug4d(printf("Too short\n"));
	  } else {
	    potentiali[npotential_left++] = segmenti;
	    debug4d(printf("Potential left #%d: %lu\n",npotential_left,segmenti->diagonal));
	  }
	}

	npotential_right = 0;
	for (segmentj = segmentm+1;
#ifdef NO_MARKER_SEGMENTS
	     segmentj < &(segments[nsegments]) && segmentj->chrnum == segmentm->chrnum &&
#endif
	       segmentj->diagonal <= segmentm->diagonal + max_distance &&
	       npotential_right < MAX_LOCALSPLICING_POTENTIAL; segmentj++) {
	  debug4d(printf("local right?  diagonal %lu, querypos %d..%d => diagonal %lu, querypos %d..%d\n",
			 segmentm->diagonal,segmentm->querypos5,segmentm->querypos3,
			 segmentj->diagonal,segmentj->querypos5,segmentj->querypos3));
	  /* m5 m3 j5 j3 */
	  assert(segmentm->diagonal < segmentj->diagonal);
	  if (segmentj->rightmost < 0) {
	    /* Failed outer floor test in find_singlesplices */
	  } else if (plusp == true && segmentm->querypos3 >= segmentj->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (plusp == false && segmentj->querypos3 >= segmentm->querypos5) {
	    debug4d(printf("Bad querypos\n"));
	  } else if (segmentm->diagonal + min_intronlength > segmentj->diagonal) {
	    debug4d(printf("Too short\n"));
	  } else {
	    potentialj[npotential_right++] = segmentj;
	    debug4d(printf("Potential right #%d: %lu\n",npotential_right,segmentj->diagonal));
	  }
	}

	if (npotential_left > 0 && npotential_right > 0) {
	  segmentm_left = segmentm->diagonal - querylength;

	  for (k = 0; k < npotential_left; k++) {
	    segmenti = potentiali[k];
	    segmenti_left = segmenti->diagonal - querylength;

	    for (l = 0; l < npotential_right; l++) {
	      segmentj = potentialj[l];

	      debug4d(printf("Doublesplice span test (%d mismatches allowed): %d mismatches found from leftmost %d to j.rightmost %d\n",
			     max_mismatches_allowed,
			     Genome_count_mismatches_substring(query_compress,segmentm_left,
							       /*pos5*/segmenti->leftmost,/*pos3*/segmentj->rightmost,
							       plusp,genestrand),
			     segmenti->leftmost,segmentj->rightmost));
	      
	      if (segmenti->leftmost >= segmentj->rightmost) {
		debug4d(printf("Double splice is not possible with pos5 %d > pos3 %d\n",
			       segmenti->leftmost,segmentj->rightmost));
	      } else if (Genome_count_mismatches_limit(query_compress,segmentm_left,
						       /*pos5*/segmenti->leftmost,/*pos3*/segmentj->rightmost,
						       max_mismatches_allowed,plusp,genestrand) <= max_mismatches_allowed) {
		debug4d(printf("Double splice is possible\n"));
		segmentj_left = segmentj->diagonal - querylength;

		debug4d(printf("  => checking for double splice: Splice_solve_double\n"));
		hits = Splice_solve_double(&(*found_score),&nhits_local,hits,&(*lowprob),
					   &segmenti->usedp,&segmentm->usedp,&segmentj->usedp,
					   /*segmenti_left*/segmenti->diagonal - querylength,
					   /*segmentm_left*/segmentm->diagonal - querylength,
					   /*segmentj_left*/segmentj->diagonal - querylength,
					   segmenti->chrnum,segmenti->chroffset,segmenti->chrhigh,segmenti->chrlength,
					   segmentm->chrnum,segmentm->chroffset,segmentm->chrhigh,segmentm->chrlength,
					   segmentj->chrnum,segmentj->chroffset,segmentj->chrhigh,segmentj->chrlength,
					   querylength,query_compress,
					   segmenti_donor_knownpos,segmentm_acceptor_knownpos,segmentm_donor_knownpos,segmentj_acceptor_knownpos,
					   segmentj_antidonor_knownpos,segmentm_antiacceptor_knownpos,segmentm_antidonor_knownpos,segmenti_antiacceptor_knownpos,
					   segmenti_donor_knowni,segmentm_acceptor_knowni,segmentm_donor_knowni,segmentj_acceptor_knowni,
					   segmentj_antidonor_knowni,segmentm_antiacceptor_knowni,segmentm_antidonor_knowni,segmenti_antiacceptor_knowni,
					   segmenti_donor_nknown,segmentm_acceptor_nknown,segmentm_donor_nknown,segmentj_acceptor_nknown,
					   segmentj_antidonor_nknown,segmentm_antiacceptor_nknown,segmentm_antidonor_nknown,segmenti_antiacceptor_nknown,
					   splicing_penalty,max_mismatches_allowed,plusp,genestrand,subs_or_indels_p,/*sarrayp*/false);
	      }
	    }
	  }
	}
      }
    }
  }

  return hits;
}
#endif


static void
find_spliceends_shortend (List_T **shortend_donors, List_T **shortend_antidonors,
			  List_T **shortend_acceptors, List_T **shortend_antiacceptors,
			  struct Segment_T *segments, int nsegments,
#ifdef DEBUG4E
			  char *queryptr,
#endif
			  Floors_T floors, int querylength, int query_lastpos, Compress_T query_compress,
			  int max_mismatches_allowed, bool plusp, int genestrand) {
#ifdef DEBUG4E
  char *gbuffer;
#endif

  Segment_T segment;
  Substring_T hit;
  Univcoord_T segment_left;
  int nmismatches, jstart, jend, j;
  int splice_pos;

  int mismatch_positions[MAX_READLENGTH+1];
  int nmismatches_left, nmismatches_right;
  int *floors_from_neg3, *floors_to_pos3;
  bool sensep;

  int splice_pos_start, splice_pos_end;
#ifdef DEBUG4E
  int i;
#endif

  debug4e(printf("Entering find_spliceends_shortend with %d segments\n",nsegments));

  if (nsegments > 0) {
    floors_from_neg3 = floors->scorefrom[-index1interval];
    floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

    for (segment = segments; segment < &(segments[nsegments]); segment++) {
      if (segment->diagonal == (Univcoord_T) -1) {
	/* Skip chr marker segment */

      } else if (segment->splicesites_i >= 0) {
	segment_left = segment->diagonal - querylength; /* FORMULA: Corresponds to querypos 0 */
	debug4e(printf("find_spliceends_shortend: Checking up to %d mismatches at diagonal %lu (querypos %d..%d) - querylength %d = %lu, floors %d and %d\n",
		       max_mismatches_allowed,segment->diagonal,segment->querypos5,segment->querypos3,querylength,segment_left,
		       floors_from_neg3[segment->querypos5],floors_to_pos3[segment->querypos3]));

	debug4e(
		gbuffer = (char *) CALLOC(querylength+1,sizeof(char));
		Genome_fill_buffer_blocks(segment_left,querylength,gbuffer);
		printf("genome 0..: %s\n",gbuffer);
		printf("query  0..: %s\n",queryptr);
		FREE(gbuffer);
		);

	/* Splice ends from left to splice site */
	if ((plusp == true && floors_from_neg3[segment->querypos5] <= max_mismatches_allowed) ||
	    (plusp == false && floors_to_pos3[segment->querypos3] <= max_mismatches_allowed)) {

	  /* pos3 was trimpos */
	  nmismatches_left = Genome_mismatches_left(mismatch_positions,max_mismatches_allowed,
						    query_compress,/*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
						    plusp,genestrand);

	  debug4e(
		  printf("%d mismatches on left (%d allowed) at:",
			 nmismatches_left,max_mismatches_allowed);
		  for (i = 0; i <= nmismatches_left; i++) {
		    printf(" %d",mismatch_positions[i]);
		  }
		  printf("\n");
		  );

	  splice_pos_start = 1;  /* not index1part */
	  if (nmismatches_left <= max_mismatches_allowed) {
	    splice_pos_end = querylength - 1;
	  } else if ((splice_pos_end = mismatch_positions[nmismatches_left-1]) > querylength - 1) {
	    splice_pos_end = querylength - 1;
	  }
	  splice_pos_end = querylength - 1;

	  debug4e(printf("Search for splice sites from %d up (%lu) to %d (%lu)\n",
			 splice_pos_start,segment_left+splice_pos_start,splice_pos_end,segment_left+splice_pos_end));

	  jstart = segment->splicesites_i;
	  while (jstart < nsplicesites && splicesites[jstart] < segment_left + splice_pos_start) {
	    jstart++;
	  }
	  jend = jstart;
	  while (jend < nsplicesites && splicesites[jend] <= segment_left + splice_pos_end) { /* Needs to be <= */
	    jend++;
	  }

	  nmismatches = 0;
	  for (j = jstart; j < jend; j++) {
	    debug4e(printf("splicesites_i #%d is at %lu\n",j,splicesites[j]));
	    splice_pos = splicesites[j] - segment_left;
	    while (nmismatches < nmismatches_left && mismatch_positions[nmismatches] < splice_pos) { /* Changed from <= to < */
	      debug4e(printf("  mismatch at %d\n",mismatch_positions[nmismatches]));
	      nmismatches++;
	    }
#if 0
	    assert(nmismatches == Genome_count_mismatches_substring(query_compress,segment_left,/*pos5*/0,/*pos3*/splice_pos,plusp,genestrand));
#endif
	    if (nmismatches > max_mismatches_allowed) {
	      debug4e(printf("nmismatches %d > max_mismatches_allowed %d\n",nmismatches,max_mismatches_allowed));
	    } else if (splicetypes[j] == DONOR) {
	      debug4e(printf("Known donor #%d at querypos %d\n",j,splicesites[j] - segment_left));
	      debug4e(printf("Known donor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
			     segment_left,splice_pos,nmismatches,splice_pos_end));
	      sensep = (plusp == true) ? true : false;
	      if ((hit = Substring_new_donor(j,/*joffset*/0,splice_pos,nmismatches,
					     /*prob*/2.0,/*left*/segment_left,query_compress,
					     querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
					     segment->chrhigh,segment->chrlength)) != NULL) {
		debug4e(printf("=> %s donor: known at %d (%d mismatches)\n",
			       plusp == true ? "plus" : "minus",Substring_chimera_pos(hit),nmismatches));
		(*shortend_donors)[nmismatches] = List_push((*shortend_donors)[nmismatches],(void *) hit);
	      }

	    } else if (splicetypes[j] == ANTIACCEPTOR) {
	      debug4e(printf("Known antiacceptor #%d at querypos %d\n",j,splicesites[j] - segment_left));
	      debug4e(printf("Known antiacceptor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
			     segment_left,splice_pos,nmismatches,splice_pos_end));
	      sensep = (plusp == true) ? false : true;
	      if ((hit = Substring_new_acceptor(j,/*joffset*/0,splice_pos,nmismatches,
						/*prob*/2.0,/*left*/segment_left,query_compress,
						querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength)) != NULL) {
		debug4e(printf("=> %s antiacceptor : known at %d (%d mismatches)\n",
			       plusp == true ? "plus" : "minus",Substring_chimera_pos(hit),nmismatches));
		(*shortend_antiacceptors)[nmismatches] = List_push((*shortend_antiacceptors)[nmismatches],(void *) hit);
	      }
	    }
	  }
	}

	/* Splice ends from splice site to right end */
	if ((plusp == true && floors_to_pos3[segment->querypos3] <= max_mismatches_allowed) ||
	    (plusp == false && floors_from_neg3[segment->querypos5] <= max_mismatches_allowed)) {

	  /* pos5 was trimpos+1 */
	  nmismatches_right = Genome_mismatches_right(mismatch_positions,max_mismatches_allowed,
						      query_compress,/*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
						      plusp,genestrand);

	  debug4e(
		  printf("%d mismatches on right (%d allowed) at:",nmismatches_right,max_mismatches_allowed);
		  for (i = 0; i <= nmismatches_right; i++) {
		    printf(" %d",mismatch_positions[i]);
		  }
		  printf("\n");
		  );

	  splice_pos_end = querylength - 1;  /* not query_lastpos */
	  if (nmismatches_right <= max_mismatches_allowed) {
	    splice_pos_start = 1;
	  } else if ((splice_pos_start = mismatch_positions[nmismatches_right-1]) < 1) {
	    splice_pos_start = 1;
	  }

	  debug4e(printf("Search for splice sites from %d (%lu) down to %d (%lu)\n",
			 splice_pos_end,segment_left+splice_pos_end,splice_pos_start,segment_left+splice_pos_start));

	  jstart = segment->splicesites_i;
	  while (jstart < nsplicesites && splicesites[jstart] < segment_left + splice_pos_start) {
	    jstart++;
	  }
	  jend = jstart;
	  while (jend < nsplicesites && splicesites[jend] <= segment_left + splice_pos_end) { /* Needs to be <= */
	    jend++;
	  }

	  nmismatches = 0;
	  for (j = jend - 1; j >= jstart; j--) {
	    debug4e(printf("splicesites_i #%d is at %lu\n",j,splicesites[j]));
	    splice_pos = splicesites[j] - segment_left;
	    while (nmismatches < nmismatches_right && mismatch_positions[nmismatches] >= splice_pos) { /* Must be >= */
	      debug4e(printf("  mismatch at %d\n",mismatch_positions[nmismatches]));
	      nmismatches++;
	    }
#if 0
	    assert(nmismatches == Genome_count_mismatches_substring(query_compress,segment_left,/*pos5*/splice_pos,/*pos3*/querylength,plusp,genestrand));
#endif
	    if (nmismatches > max_mismatches_allowed) {
	      debug4e(printf("nmismatches %d > max_mismatches_allowed %d\n",nmismatches,max_mismatches_allowed));
	    } else if (splicetypes[j] == ACCEPTOR) {
	      debug4e(printf("Known acceptor #%d at querypos %d\n",j,splicesites[j] - segment_left));
	      debug4e(printf("Known acceptor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
			     segment_left,splice_pos,nmismatches,splice_pos_start));
	      sensep = (plusp == true) ? true : false;
	      if ((hit = Substring_new_acceptor(j,/*joffset*/0,splice_pos,nmismatches,
						/*prob*/2.0,/*left*/segment_left,query_compress,
						querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength)) != NULL) {
		debug4e(printf("=> %s acceptor: known at %d (%d mismatches)\n",
			       plusp == true ? "plus" : "minus",Substring_chimera_pos(hit),nmismatches));
		(*shortend_acceptors)[nmismatches] = List_push((*shortend_acceptors)[nmismatches],(void *) hit);
	      }

	    } else if (splicetypes[j] == ANTIDONOR) {
	      debug4e(printf("Known antidonor #%d at querypos %d\n",j,splicesites[j] - segment_left));
	      debug4e(printf("Known antidonor for segmenti at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
			     segment_left,splice_pos,nmismatches,splice_pos_start));
	      sensep = (plusp == true) ? false : true;
	      if ((hit = Substring_new_donor(j,/*joffset*/0,splice_pos,nmismatches,
					     /*prob*/2.0,/*left*/segment_left,query_compress,
					     querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
					     segment->chrhigh,segment->chrlength)) != NULL) {
		debug4e(printf("=> %s antidonor: known at %d (%d mismatches)\n",
			       plusp == true ? "plus" : "minus",Substring_chimera_pos(hit),nmismatches));
		(*shortend_antidonors)[nmismatches] = List_push((*shortend_antidonors)[nmismatches],(void *) hit);
	      }
	    }
	  }
	}
      }
    }
  }
    
  return;
}


static void
find_spliceends_distant (List_T **distant_donors, List_T **distant_antidonors,
			 List_T **distant_acceptors, List_T **distant_antiacceptors,
			 struct Segment_T *segments, int nsegments,
#ifdef DEBUG4E
			 char *queryptr,
#endif
			 Floors_T floors, int querylength, int query_lastpos, Compress_T query_compress,
			 int max_mismatches_allowed, bool plusp, int genestrand) {
#ifdef DEBUG4E
  char *gbuffer;
#endif

  Segment_T segment;
  Substring_T hit;
  Univcoord_T segment_left;
  int nmismatches, j, i;
  int splice_pos;
  double prob;

  int mismatch_positions[MAX_READLENGTH+1];
  int nmismatches_left, nmismatches_right;
  int *floors_from_neg3, *floors_to_pos3;
  bool sensep;

  int splice_pos_start, splice_pos_end;

  int segment_donor_knownpos[MAX_READLENGTH+1], segment_acceptor_knownpos[MAX_READLENGTH+1];
  int segment_antidonor_knownpos[MAX_READLENGTH+1], segment_antiacceptor_knownpos[MAX_READLENGTH+1];
  int segment_donor_knowni[MAX_READLENGTH+1], segment_acceptor_knowni[MAX_READLENGTH+1];
  int segment_antidonor_knowni[MAX_READLENGTH+1], segment_antiacceptor_knowni[MAX_READLENGTH+1];
  int segment_donor_nknown, segment_acceptor_nknown, segment_antidonor_nknown, segment_antiacceptor_nknown;

  int positions_alloc[MAX_READLENGTH+1];
  int knowni_alloc[MAX_READLENGTH+1];
  int donori_nsites, acceptorj_nsites, antiacceptori_nsites, antidonorj_nsites;
  int *donori_positions, *acceptorj_positions, *antiacceptori_positions, *antidonorj_positions;
  int *donori_knowni, *acceptorj_knowni, *antiacceptori_knowni, *antidonorj_knowni;


  debug4e(printf("Entering find_spliceends_distant with %d segments\n",nsegments));

  if (nsegments > 0) {
    floors_from_neg3 = floors->scorefrom[-index1interval];
    floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

    for (segment = segments; segment < &(segments[nsegments]); segment++) {
      if (segment->diagonal < (Univcoord_T) -1) {

	segment_left = segment->diagonal - querylength; /* FORMULA: Corresponds to querypos 0 */
	debug4e(printf("find_spliceends: Checking up to %d mismatches at diagonal %lu (querypos %d..%d) - querylength %d = %lu, floors %d and %d\n",
		       max_mismatches_allowed,segment->diagonal,segment->querypos5,segment->querypos3,querylength,segment_left,
		       floors_from_neg3[segment->querypos5],floors_to_pos3[segment->querypos3]));

	debug4e(
		gbuffer = (char *) CALLOC(querylength+1,sizeof(char));
		Genome_fill_buffer_blocks(segment_left,querylength,gbuffer);
		printf("genome 0..: %s\n",gbuffer);
		printf("query  0..: %s\n",queryptr);
		FREE(gbuffer);
		);

	/* Splice ends from left to splice site */
	if ((plusp == true && floors_from_neg3[segment->querypos5] <= max_mismatches_allowed) ||
	    (plusp == false && floors_to_pos3[segment->querypos3] <= max_mismatches_allowed)) {

	  /* pos3 was trimpos */
	  nmismatches_left = Genome_mismatches_left(mismatch_positions,max_mismatches_allowed,
						    query_compress,/*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
						    plusp,genestrand);

	  debug4e(
		  printf("%d mismatches on left (%d allowed) at:",
			 nmismatches_left,max_mismatches_allowed);
		  for (i = 0; i <= nmismatches_left; i++) {
		    printf(" %d",mismatch_positions[i]);
		  }
		  printf("\n");
		  );

	  splice_pos_start = index1part;
	  if (nmismatches_left <= max_mismatches_allowed) {
	    splice_pos_end = querylength - 1;
	  } else if ((splice_pos_end = mismatch_positions[nmismatches_left-1]) > querylength - 1) {
	    splice_pos_end = querylength - 1;
	  }

	  if (splice_pos_start <= splice_pos_end) {
	    debug4e(printf("Search for splice sites from %d up to %d\n",splice_pos_start,splice_pos_end));

	    segment_donor_nknown = 0;
	    segment_antiacceptor_nknown = 0;
	    if ((j = segment->splicesites_i) >= 0) {
	      /* Known splicing */
	      /* Ends 1 (donor, plus) and 8 (antiacceptor, plus): mark known splice sites in segment */
	      while (j < nsplicesites && splicesites[j] <= segment_left + splice_pos_end) { /* Needs to be <= */
		if (splicetypes[j] == DONOR) {
		  segment_donor_knownpos[segment_donor_nknown] = splicesites[j] - segment_left;
		  segment_donor_knowni[segment_donor_nknown++] = j;
		} else if (splicetypes[j] == ANTIACCEPTOR) {
		  segment_antiacceptor_knownpos[segment_antiacceptor_nknown] = splicesites[j] - segment_left;
		  segment_antiacceptor_knowni[segment_antiacceptor_nknown++] = j;
		}
		j++;
	      }
	    }
	    segment_donor_knownpos[segment_donor_nknown] = MAX_READLENGTH;
	    segment_antiacceptor_knownpos[segment_antiacceptor_nknown] = MAX_READLENGTH;

	    /* Originally on plus strand.  No complement */
	    sensep = (plusp == true) ? true : false;
	    if (novelsplicingp && segment_left + splice_pos_start >= DONOR_MODEL_LEFT_MARGIN) {
	      donori_nsites = Genome_donor_positions(positions_alloc,knowni_alloc,
						     segment_donor_knownpos,segment_donor_knowni,
						     segment_left,splice_pos_start,splice_pos_end+1);
	      donori_positions = positions_alloc;
	      donori_knowni = knowni_alloc;
	      debug4e(
		      printf("Donor dinucleotides:");
		      for (i = 0; i < donori_nsites; i++) {
			printf(" %d",donori_positions[i]);
		      }
		      printf("\n");
		      );
	    } else {
	      donori_nsites = segment_donor_nknown;
	      donori_positions = segment_donor_knownpos;
	      donori_knowni = segment_donor_knowni;
	    }

	    i = 0;
	    nmismatches = 0;
	    while (i < donori_nsites && nmismatches <= max_mismatches_allowed) {
	      splice_pos = donori_positions[i];
	      while (nmismatches < nmismatches_left && mismatch_positions[nmismatches] < splice_pos) { /* Changed from <= to < */
		debug4e(printf("  mismatch at %d\n",mismatch_positions[nmismatches]));
		nmismatches++;
	      }
	      debug4e(printf(" splice pos %d, nmismatches %d\n",splice_pos,nmismatches));
#if 0
	      assert(nmismatches == Genome_count_mismatches_substring(query_compress,segment_left,/*pos5*/0,/*pos3*/splice_pos,plusp,genestrand));
#endif
	      if (nmismatches <= max_mismatches_allowed) {
		if (donori_knowni[i] >= 0) {
		  debug4e(printf("Known donor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				 segment_left,splice_pos,nmismatches,splice_pos_end));
		
		  if ((hit = Substring_new_donor(donori_knowni[i],/*joffset*/0,splice_pos,nmismatches,
						 /*prob*/2.0,/*left*/segment_left,query_compress,
						 querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						 segment->chrhigh,segment->chrlength)) != NULL) {
		    debug4e(printf("=> %s donor: %f at %d (%d mismatches)\n",
				   plusp == true ? "plus" : "minus",Maxent_hr_donor_prob(segment_left + splice_pos,segment->chroffset),
				   Substring_chimera_pos(hit),nmismatches));
		    debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		    (*distant_donors)[nmismatches] = List_push((*distant_donors)[nmismatches],(void *) hit);
		  }

		} else {
		  prob = Maxent_hr_donor_prob(segment_left + splice_pos,segment->chroffset);
		  debug4e(printf("splice pos %d, nmismatches %d, prob %f, sufficient %d\n",
				 splice_pos,nmismatches,prob,sufficient_splice_prob_distant(splice_pos,nmismatches,prob)));
		  if (sufficient_splice_prob_distant(/*support*/splice_pos,nmismatches,prob)) {
		    debug4e(printf("Novel donor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				   segment_left,splice_pos,nmismatches,splice_pos_end));
		    if ((hit = Substring_new_donor(/*knowni*/-1,/*joffset*/0,splice_pos,nmismatches,
						   prob,/*left*/segment_left,query_compress,
						   querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						   segment->chrhigh,segment->chrlength)) != NULL) {
		      debug4e(printf("=> %s donor: %f at %d (%d mismatches)\n",
				     plusp == true ? "plus" : "minus",prob,Substring_chimera_pos(hit),nmismatches));
		      debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		      (*distant_donors)[nmismatches] = List_push((*distant_donors)[nmismatches],(void *) hit);
		    }
		  }
		}
	      }

	      i++;
	    }


	    /* Splicing originally on minus strand.  Complement */
	    sensep = (plusp == true) ? false : true;
	    if (novelsplicingp && segment_left + splice_pos_start >= ACCEPTOR_MODEL_RIGHT_MARGIN) {
	      antiacceptori_nsites = Genome_antiacceptor_positions(positions_alloc,knowni_alloc,
								   segment_antiacceptor_knownpos,segment_antiacceptor_knowni,
								   segment_left,splice_pos_start,splice_pos_end+1);
	      antiacceptori_positions = positions_alloc;
	      antiacceptori_knowni = knowni_alloc;
	      debug4e(
		      printf("Antiacceptor dinucleotides:");
		      for (i = 0; i < antiacceptori_nsites; i++) {
			printf(" %d",antiacceptori_positions[i]);
		      }
		      printf("\n");
		      );
	    } else {
	      antiacceptori_nsites = segment_antiacceptor_nknown;
	      antiacceptori_positions = segment_antiacceptor_knownpos;
	      antiacceptori_knowni = segment_antiacceptor_knowni;
	    }

	    i = 0;
	    nmismatches = 0;
	    while (i < antiacceptori_nsites && nmismatches <= max_mismatches_allowed) {
	      splice_pos = antiacceptori_positions[i];
	      while (nmismatches < nmismatches_left && mismatch_positions[nmismatches] < splice_pos) { /* Changed from <= to < */
		debug4e(printf("  mismatch at %d\n",mismatch_positions[nmismatches]));
		nmismatches++;
	      }
	      debug4e(printf(" splice pos %d, nmismatches %d\n",splice_pos,nmismatches));
#if 0
	      assert(nmismatches == Genome_count_mismatches_substring(query_compress,segment_left,/*pos5*/0,/*pos3*/splice_pos,plusp,genestrand));
#endif
	      if (nmismatches <= max_mismatches_allowed) {
		if (antiacceptori_knowni[i] >= 0) {
		  debug4e(printf("Known antiacceptor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				 segment_left,splice_pos,nmismatches,splice_pos_end));
		  if ((hit = Substring_new_acceptor(antiacceptori_knowni[i],/*joffset*/0,splice_pos,nmismatches,
						    /*prob*/2.0,/*left*/segment_left,query_compress,
						    querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						    segment->chrhigh,segment->chrlength)) != NULL) {
		    debug4e(printf("=> %s antiacceptor : %f at %d (%d mismatches)\n",
				   plusp == true ? "plus" : "minus",Maxent_hr_antiacceptor_prob(segment_left + splice_pos,segment->chroffset),
				   Substring_chimera_pos(hit),nmismatches));
		    debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		    (*distant_antiacceptors)[nmismatches] = List_push((*distant_antiacceptors)[nmismatches],(void *) hit);
		  }

		} else {
		  prob = Maxent_hr_antiacceptor_prob(segment_left + splice_pos,segment->chroffset);
		  debug4e(printf("splice pos %d, nmismatches %d, prob %f, sufficient %d\n",
				 splice_pos,nmismatches,prob,sufficient_splice_prob_distant(splice_pos,nmismatches,prob)));
		  if (sufficient_splice_prob_distant(/*support*/splice_pos,nmismatches,prob)) {
		    debug4e(printf("Novel antiacceptor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				   segment_left,splice_pos,nmismatches,splice_pos_end));
		    if ((hit = Substring_new_acceptor(/*knowni*/-1,/*joffset*/0,splice_pos,nmismatches,
						      prob,/*left*/segment_left,query_compress,
						      querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						      segment->chrhigh,segment->chrlength)) != NULL) {
		      debug4e(printf("=> %s antiacceptor : %f at %d (%d mismatches)\n",
				     plusp == true ? "plus" : "minus",prob,Substring_chimera_pos(hit),nmismatches));
		      debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		      (*distant_antiacceptors)[nmismatches] = List_push((*distant_antiacceptors)[nmismatches],(void *) hit);
		    }
		  }
		}
	      }

	      i++;
	    }
	  }

	}

	/* Splice ends from splice site to right end */
	if ((plusp == true && floors_to_pos3[segment->querypos3] <= max_mismatches_allowed) ||
	    (plusp == false && floors_from_neg3[segment->querypos5] <= max_mismatches_allowed)) {

	  /* pos5 was trimpos+1 */
	  nmismatches_right = Genome_mismatches_right(mismatch_positions,max_mismatches_allowed,
						      query_compress,/*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
						      plusp,genestrand);

	  debug4e(
		  printf("%d mismatches on right (%d allowed) at:",nmismatches_right,max_mismatches_allowed);
		  for (i = 0; i <= nmismatches_right; i++) {
		    printf(" %d",mismatch_positions[i]);
		  }
		  printf("\n");
		  );

	  splice_pos_end = query_lastpos;
	  if (nmismatches_right <= max_mismatches_allowed) {
	    splice_pos_start = 1;
	  } else if ((splice_pos_start = mismatch_positions[nmismatches_right-1]) < 1) {
	    splice_pos_start = 1;
	  }

	  if (splice_pos_start <= splice_pos_end) {
	    debug4e(printf("Search for splice sites from %d down to %d\n",splice_pos_end,splice_pos_start));

	    segment_acceptor_nknown = 0;
	    segment_antidonor_nknown = 0;
	    if ((j = segment->splicesites_i) >= 0) {
	      /* Known splicing */
	      while (j < nsplicesites && splicesites[j] <= segment_left + splice_pos_end) { /* Needs to be <= */
		if (splicetypes[j] == ACCEPTOR) {
		  debug4k(printf("Setting known acceptor %d for segment at %lu\n",j,splicesites[j]));
		  segment_acceptor_knownpos[segment_acceptor_nknown] = splicesites[j] - segment_left;
		  segment_acceptor_knowni[segment_acceptor_nknown++] = j;
		} else if (splicetypes[j] == ANTIDONOR) {
		  debug4k(printf("Setting known antidonor %d for segment at %lu\n",j,splicesites[j]));
		  segment_antidonor_knownpos[segment_antidonor_nknown] = splicesites[j] - segment_left;
		  segment_antidonor_knowni[segment_antidonor_nknown++] = j;
		}
		j++;
	      }
	    }
	    segment_acceptor_knownpos[segment_acceptor_nknown] = MAX_READLENGTH;
	    segment_antidonor_knownpos[segment_antidonor_nknown] = MAX_READLENGTH;


	    /* Splicing originally on plus strand.  No complement. */
	    sensep = (plusp == true) ? true : false;
	    if (novelsplicingp && segment_left + splice_pos_start >= ACCEPTOR_MODEL_LEFT_MARGIN) {
	      acceptorj_nsites = Genome_acceptor_positions(positions_alloc,knowni_alloc,
							   segment_acceptor_knownpos,segment_acceptor_knowni,
							   segment_left,splice_pos_start,splice_pos_end+1);
	      acceptorj_positions = positions_alloc;
	      acceptorj_knowni = knowni_alloc;
	      debug4e(
		      printf("Acceptor dinucleotides:");
		      for (i = 0; i < acceptorj_nsites; i++) {
			printf(" %d",acceptorj_positions[i]);
		      }
		      printf("\n");
		      );
	    } else {
	      acceptorj_nsites = segment_acceptor_nknown;
	      acceptorj_positions = segment_acceptor_knownpos;
	      acceptorj_knowni = segment_acceptor_knowni;
	    }

	    i = acceptorj_nsites - 1;
	    nmismatches = 0;
	    while (i >= 0 && nmismatches <= max_mismatches_allowed) {
	      splice_pos = acceptorj_positions[i];
	      while (nmismatches < nmismatches_right && mismatch_positions[nmismatches] >= splice_pos) { /* Must be >= */
		debug4e(printf("  mismatch at %d\n",mismatch_positions[nmismatches]));
		nmismatches++;
	      }
	      debug4e(printf(" splice pos %d, nmismatches %d\n",splice_pos,nmismatches));
#if 0
	      assert(nmismatches == Genome_count_mismatches_substring(query_compress,segment_left,/*pos5*/splice_pos,/*pos3*/querylength,plusp,genestrand));
#endif
	      if (nmismatches <= max_mismatches_allowed) {
		if (acceptorj_knowni[i] >= 0) {
		  debug4e(printf("Known acceptor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				 segment_left,splice_pos,nmismatches,splice_pos_start));
		  if ((hit = Substring_new_acceptor(acceptorj_knowni[i],/*joffset*/0,splice_pos,nmismatches,
						    /*prob*/2.0,/*left*/segment_left,query_compress,
						    querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						    segment->chrhigh,segment->chrlength)) != NULL) {
		    debug4e(printf("=> %s acceptor: %f at %d (%d mismatches)\n",
				   plusp == true ? "plus" : "minus",Maxent_hr_acceptor_prob(segment_left + splice_pos,segment->chroffset),
				   Substring_chimera_pos(hit),nmismatches));
		    debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		    (*distant_acceptors)[nmismatches] = List_push((*distant_acceptors)[nmismatches],(void *) hit);
		  }

		} else {
		  prob = Maxent_hr_acceptor_prob(segment_left + splice_pos,segment->chroffset);
		  debug4e(printf("splice pos %d, nmismatches %d, prob %f, sufficient %d\n",
				 splice_pos,nmismatches,prob,sufficient_splice_prob_distant(querylength - splice_pos,nmismatches,prob)));
		  if (sufficient_splice_prob_distant(/*support*/querylength - splice_pos,nmismatches,prob)) {
		    debug4e(printf("Novel acceptor for segment at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				   segment_left,splice_pos,nmismatches,splice_pos_start));
		    if ((hit = Substring_new_acceptor(/*knowni*/-1,/*joffset*/0,splice_pos,nmismatches,
						      prob,/*left*/segment_left,query_compress,
						      querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						      segment->chrhigh,segment->chrlength)) != NULL) {
		      debug4e(printf("=> %s acceptor: %f at %d (%d mismatches)\n",
				     plusp == true ? "plus" : "minus",prob,Substring_chimera_pos(hit),nmismatches));
		      debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		      (*distant_acceptors)[nmismatches] = List_push((*distant_acceptors)[nmismatches],(void *) hit);
		    }
		  }
		}
	      }
	  
	      i--;
	    }


	    /* Splicing originally on minus strand.  Complement.  */
	    sensep = (plusp == true) ? false : true;
	    if (novelsplicingp && segment_left + splice_pos_start >= DONOR_MODEL_RIGHT_MARGIN) {
	      antidonorj_nsites = Genome_antidonor_positions(positions_alloc,knowni_alloc,
							     segment_antidonor_knownpos,segment_antidonor_knowni,
							     segment_left,splice_pos_start,splice_pos_end+1);
	      antidonorj_positions = positions_alloc;
	      antidonorj_knowni = knowni_alloc;
	      debug4e(
		      printf("Antidonor dinucleotides:");
		      for (i = 0; i < antidonorj_nsites; i++) {
			printf(" %d",antidonorj_positions[i]);
		      }
		      printf("\n");
		      );
	    } else {
	      antidonorj_nsites = segment_antidonor_nknown;
	      antidonorj_positions = segment_antidonor_knownpos;
	      antidonorj_knowni = segment_antidonor_knowni;
	    }

	    i = antidonorj_nsites - 1;
	    nmismatches = 0;
	    while (i >= 0 && nmismatches <= max_mismatches_allowed) {
	      splice_pos = antidonorj_positions[i];
	      while (nmismatches < nmismatches_right && mismatch_positions[nmismatches] >= splice_pos) { /* Must be >= */
		debug4e(printf("  mismatch at %d\n",mismatch_positions[nmismatches]));
		nmismatches++;
	      }
	      debug4e(printf(" splice pos %d, nmismatches %d\n",splice_pos,nmismatches));
#if 0
	      assert(nmismatches == Genome_count_mismatches_substring(query_compress,segment_left,/*pos5*/splice_pos,/*pos3*/querylength,plusp,genestrand));
#endif
	      if (nmismatches <= max_mismatches_allowed) {
		if (antidonorj_knowni[i] >= 0) {
		  debug4e(printf("Known antidonor for segmenti at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				 segment_left,splice_pos,nmismatches,splice_pos_start));
		  if ((hit = Substring_new_donor(antidonorj_knowni[i],/*joffset*/0,splice_pos,nmismatches,
						 /*prob*/2.0,/*left*/segment_left,query_compress,
						 querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						 segment->chrhigh,segment->chrlength)) != NULL) {
		    debug4e(printf("=> %s antidonor: %f at %d (%d mismatches)\n",
				   plusp == true ? "plus" : "minus",Maxent_hr_antidonor_prob(segment_left + splice_pos,segment->chroffset),
				   Substring_chimera_pos(hit),nmismatches));
		    debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		    (*distant_antidonors)[nmismatches] = List_push((*distant_antidonors)[nmismatches],(void *) hit);
		  }
		  
		} else {
		  prob = Maxent_hr_antidonor_prob(segment_left + splice_pos,segment->chroffset);
		  debug4e(printf("splice pos %d, nmismatches %d, prob %f, sufficient %d\n",
				 splice_pos,nmismatches,prob,sufficient_splice_prob_distant(querylength - splice_pos,nmismatches,prob)));
		  if (sufficient_splice_prob_distant(/*support*/querylength - splice_pos,nmismatches,prob)) {
		    debug4e(printf("Novel antidonor for segmenti at %lu, splice_pos %d (%d mismatches), stopi = %d\n",
				   segment_left,splice_pos,nmismatches,splice_pos_start));
		    if ((hit = Substring_new_donor(/*knowni*/-1,/*joffset*/0,splice_pos,nmismatches,
						   prob,/*left*/segment_left,query_compress,
						   querylength,plusp,genestrand,sensep,segment->chrnum,segment->chroffset,
						   segment->chrhigh,segment->chrlength)) != NULL) {
		      debug4e(printf("=> %s antidonor: %f at %d (%d mismatches)\n",
				     plusp == true ? "plus" : "minus",prob,Substring_chimera_pos(hit),nmismatches));
		      debug4e(printf("q: %s\ng: %s\n",queryptr,gbuffer));
		      (*distant_antidonors)[nmismatches] = List_push((*distant_antidonors)[nmismatches],(void *) hit);
		    }
		  }
		}
	      }

	      i--;
	    }
	  }

	}
      }
    }
  }

  return;
}



/* Integrates terminals found from ends by counting mismatches, and
   those where querypos3 - querypos5 is long enough */
static List_T
find_terminals (struct Segment_T *plus_segments, int plus_nsegments,
		struct Segment_T *minus_segments, int minus_nsegments,
#ifdef DEBUG4T
		char *queryuc_ptr, /* for debugging */ char *queryrc,
#endif
		Floors_T floors, int querylength, int query_lastpos,
		Compress_T query_compress_fwd, Compress_T query_compress_rev,
		int max_mismatches_allowed, int max_terminal_length,
		int genestrand) {
#ifdef DEBUG4T
  char *gbuffer;
#endif
  List_T terminals = NULL;
  Segment_T segment;
  Stage3end_T hit;
  Univcoord_T segment_left;
  int nmismatches_left, nmismatches_right;
  Endtype_T start_endtype, end_endtype;

  int mismatch_positions[MAX_READLENGTH+1];
  int *floors_from_neg3, *floors_to_pos3;

#ifdef DEBUG4T
  int i;
#endif

  debug(printf("identify_terminals: Checking up to %d mismatches\n",max_mismatches_allowed));

  if (floors == NULL) {
    return (List_T) NULL;

  } else {
    floors_from_neg3 = floors->scorefrom[-index1interval];
    floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

    /* Needs to be /3 for long_terminals and short_terminals to work */
    max_terminal_length = querylength/3;
  }

  if (plus_nsegments > 0) {
    for (segment = plus_segments; segment < &(plus_segments[plus_nsegments]); segment++) {
      if (0 && segment->usedp == true) {
	/* Previously skipped, but looks like a bad idea */
      } else if (segment->diagonal < (Univcoord_T) -1) {
	debug4t(printf("plus: %lu, %d..%d\n",segment->diagonal,segment->querypos5,segment->querypos3));
	segment_left = segment->diagonal - querylength; /* FORMULA: Corresponds to querypos 0 */
	debug4t(printf("identify_terminals_plus: Checking up to %d mismatches at diagonal %lu (querypos %d..%d) - querylength %d = %lu\n",
		       max_mismatches_allowed,segment->diagonal,segment->querypos5,segment->querypos3,querylength,segment_left));
	debug4t(
		gbuffer = (char *) CALLOC(querylength+1,sizeof(char));
		Genome_fill_buffer_blocks(segment_left,querylength,gbuffer);
		printf("genome 0..: %s\n",gbuffer);
		printf("query  0..: %s\n",queryuc_ptr);
		FREE(gbuffer);
		);

	if (segment->querypos3 - segment->querypos5 > index1part) {
	  debug4t(printf(" => ? Terminal based on querypos3 %d - querypos5 %d > index1part %d",
			 segment->querypos3,segment->querypos5,index1part));
	  start_endtype = (segment->querypos5 < index1interval) ? END : TERM;
	  end_endtype = (segment->querypos3 >= query_lastpos - index1interval) ? END : TERM;
	  debug4t(printf("  querypos3 %d vs index1interval %d => start_endtype %s\n",
			 segment->querypos3,index1interval,Endtype_string(start_endtype)));
	  debug4t(printf("  querypos5 %d vs query_lastpos %d - index1interval %d => end_endtype %s\n",
			 segment->querypos5,query_lastpos,index1interval,Endtype_string(end_endtype)));

	  if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*//*truncate_pos_left*/querylength,
					    /*left*/segment_left,query_compress_fwd,
					    querylength,/*plusp*/true,genestrand,start_endtype,end_endtype,
					    segment->chrnum,segment->chroffset,
					    segment->chrhigh,segment->chrlength,max_mismatches_allowed,
					    /*sarrayp*/false)) != NULL) {
	    debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
	    terminals = List_push(terminals,(void *) hit);
	  } else {
	    debug4t(printf(" => no"));
	  }
	  debug4t(printf("\n"));

	} else {
	  if (segment->floor_left > max_mismatches_allowed) {
	    debug4t(printf("Not checking left because floor_left %d > max_mismatches_allowed %d\n",
			   segment->floor_left,max_mismatches_allowed));
	  } else {
	    /* Check from left */
	    debug4t(printf("Checking left because floor_left %d <= max_mismatches_allowed %d\n",
			   segment->floor_left,max_mismatches_allowed));

	    nmismatches_left = Genome_mismatches_left(mismatch_positions,max_mismatches_allowed,
						      query_compress_fwd,/*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
						      /*plusp*/true,genestrand);
	    
	    debug4t(
		    printf("%d mismatches on left at:",nmismatches_left);
		    for (i = 0; i <= nmismatches_left; i++) {
		      printf(" %d",mismatch_positions[i]);
		    }
		    printf("\n");
		    );

	    if (nmismatches_left == 0 || nmismatches_left <= max_mismatches_allowed || 
		mismatch_positions[nmismatches_left-1] > querylength - max_terminal_length) {
	      debug4t(printf(" => Long terminal at left: nmismatches_left %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_left,max_mismatches_allowed,mismatch_positions[nmismatches_left-1],querylength - max_terminal_length));
	      if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*//*truncate_pos_left*/querylength,
						/*left*/segment_left,query_compress_fwd,
						querylength,/*plusp*/true,genestrand,
						/*start_endtype*/END,/*end_endtype*/TERM,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));

	    } else if (mismatch_positions[(nmismatches_left-1)/2] > max_terminal_length) {
	      debug4t(printf(" => Short terminal at left: nmismatches_left %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_left,max_mismatches_allowed,mismatch_positions[(nmismatches_left-1)/2],max_terminal_length));

	      if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*//*truncate_pos_left*/querylength,
						/*left*/segment_left,query_compress_fwd,
						querylength,/*plusp*/true,genestrand,
						/*start_endtype*/END,/*end_endtype*/TERM,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));

	    }
	  }

	  if (segment->floor_right > max_mismatches_allowed) {
	    debug4t(printf("Not checking right because floor_right %d > max_mismatches_allowed %d\n",
			   segment->floor_right,max_mismatches_allowed));
	  } else {
	    /* Check from right */
	    debug4t(printf("Checking right because floor_right %d <= max_mismatches_allowed %d\n",
			   segment->floor_right,max_mismatches_allowed));
	    nmismatches_right = Genome_mismatches_right(mismatch_positions,max_mismatches_allowed,
							/*query_compress*/query_compress_fwd,
							/*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
							/*plusp*/true,genestrand);
	    
	    debug4t(
		    printf("%d mismatches on right at:",nmismatches_right);
		    for (i = 0; i <= nmismatches_right; i++) {
		      printf(" %d",mismatch_positions[i]);
		    }
		    printf("\n");
		    );
	    
	    debug4t(printf("last mismatch %d, half mismatch %d, long terminalpos %d, short terminalpos %d\n",
			   mismatch_positions[nmismatches_right-1],mismatch_positions[(nmismatches_right-1)/2],
			   max_terminal_length,querylength - max_terminal_length));

	    if (nmismatches_right == 0 || nmismatches_right <= max_mismatches_allowed ||
		mismatch_positions[nmismatches_right-1] < max_terminal_length) {
	      debug4t(printf(" => Long terminal at right: nmismatches_right %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_right,max_mismatches_allowed,mismatch_positions[nmismatches_right-1],max_terminal_length));
	      if ((hit = Stage3end_new_terminal(/*querystart*//*truncate_pos_right*/0,/*queryend*/querylength,
						/*left*/segment_left,query_compress_fwd,
						querylength,/*plusp*/true,genestrand,
						/*start_endtype*/TERM,/*end_endtype*/END,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));

	    } else if (mismatch_positions[(nmismatches_right-1)/2] < querylength - max_terminal_length) {
	      debug4t(printf(" => Short terminal at right: nmismatches_right %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_right,max_mismatches_allowed,mismatch_positions[(nmismatches_right-1)/2],querylength-max_terminal_length));
	      if ((hit = Stage3end_new_terminal(/*querystart*//*truncate_pos_right*/0,/*queryend*/querylength,
						/*left*/segment_left,query_compress_fwd,
						querylength,/*plusp*/true,genestrand,
						/*start_endtype*/TERM,/*end_endtype*/END,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));


	    }
	  }
	}
      }
    }
  }

  if (minus_nsegments > 0) {
    for (segment = minus_segments; segment < &(minus_segments[minus_nsegments]); segment++) {
      if (0 && segment->usedp == true) {
	/* Previously skipped, but looks like a bad idea */
	debug4t(printf("segment used\n"));
      } else if (segment->diagonal < (Univcoord_T) -1) {
	debug4t(printf("minus: %lu, %d..%d\n",segment->diagonal,segment->querypos5,segment->querypos3));
	segment_left = segment->diagonal - querylength;
	debug4t(printf("identify_terminals_minus: Getting genome at diagonal %lu (querypos %d..%d) + 12 - querylength %d = %lu\n",
		       segment->diagonal,segment->querypos5,segment->querypos3,querylength,segment_left));
	debug4t(
		gbuffer = (char *) CALLOC(querylength+1,sizeof(char));
		Genome_fill_buffer_blocks(segment_left,querylength,gbuffer);
		printf("genome   0..: %s\n",gbuffer);
		printf("query.rc 0..: %s\n",queryrc);
		FREE(gbuffer);
		);


	if (segment->querypos3 - segment->querypos5 > index1part) {
	  debug4t(printf(" => ? Terminal based on querypos3 %d - querypos5 %d > index1part %d",
			 segment->querypos3,segment->querypos5,index1part));
	  start_endtype = (segment->querypos5 < index1interval) ? END : TERM;
	  end_endtype = (segment->querypos3 >= query_lastpos - index1interval) ? END : TERM;
	  debug4t(printf("  querypos3 %d vs index1interval %d => start_endtype %s\n",
			 segment->querypos3,index1interval,Endtype_string(start_endtype)));
	  debug4t(printf("  querypos5 %d vs query_lastpos %d - index1interval %d => end_endtype %s\n",
			 segment->querypos5,query_lastpos,index1interval,Endtype_string(end_endtype)));

	  if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*/querylength,
					    /*left*/segment_left,query_compress_rev,
					    querylength,/*plusp*/false,genestrand,start_endtype,end_endtype,
					    segment->chrnum,segment->chroffset,
					    segment->chrhigh,segment->chrlength,max_mismatches_allowed,
					    /*sarrayp*/false)) != NULL) {
	    debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
	    terminals = List_push(terminals,(void *) hit);
	  } else {
	    debug4t(printf(" => no"));
	  }
	  debug4t(printf("\n"));

	} else {
	  /* Need to reverse floor_left and floor_right */
	  if (segment->floor_right > max_mismatches_allowed) {
	    debug4t(printf("Not checking left because floor_right %d > max_mismatches_allowed %d\n",
			   segment->floor_right,max_mismatches_allowed));
	  } else {
	    /* Check from left */
	    debug4t(printf("Checking left because floor_right %d <= max_mismatches_allowed %d\n",
			   segment->floor_right,max_mismatches_allowed));
	    nmismatches_left = Genome_mismatches_left(mismatch_positions,max_mismatches_allowed,
						      /*query_compress*/query_compress_rev,
						      /*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
						      /*plusp*/false,genestrand);

	    debug4t(
		    printf("%d mismatches on left at:",nmismatches_left);
		    for (i = 0; i <= nmismatches_left; i++) {
		      printf(" %d",mismatch_positions[i]);
		    }
		    printf("\n");
		    );

	    if (nmismatches_left == 0 || nmismatches_left <= max_mismatches_allowed || 
		mismatch_positions[nmismatches_left-1] > querylength - max_terminal_length) {
	      debug4t(printf(" => Long terminal at left: nmismatches_left %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_left,max_mismatches_allowed,mismatch_positions[nmismatches_left-1],querylength - max_terminal_length));
	      if ((hit = Stage3end_new_terminal(/*querystart*//*querylength-truncate_pos_left*/0,/*queryend*/querylength,
						/*left*/segment_left,query_compress_rev,
						querylength,/*plusp*/false,genestrand,
						/*start_endtype*/TERM,/*end_endtype*/END,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));

	    } else if (mismatch_positions[(nmismatches_left-1)/2] > max_terminal_length) {
	      debug4t(printf(" => Short terminal at left: nmismatches_left %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_left,max_mismatches_allowed,mismatch_positions[(nmismatches_left-1)/2],max_terminal_length));
	      if ((hit = Stage3end_new_terminal(/*querystart*//*querylength-truncate_pos_left*/0,/*queryend*/querylength,
						/*left*/segment_left,query_compress_rev,
						querylength,/*plusp*/false,genestrand,
						/*start_endtype*/TERM,/*end_endtype*/END,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));
	    }
	  }

	  if (segment->floor_left > max_mismatches_allowed) {
	    debug4t(printf("Not checking right because floor_left %d > max_mismatches_allowed %d\n",
			   segment->floor_left,max_mismatches_allowed));
	  } else {
	    /* Check from right */
	    debug4t(printf("Checking right because floor_left %d <= max_mismatches_allowed %d\n",
			   segment->floor_left,max_mismatches_allowed));
	    nmismatches_right = Genome_mismatches_right(mismatch_positions,max_mismatches_allowed,
							/*query_compress*/query_compress_rev,
							/*left*/segment_left,/*pos5*/0,/*pos3*/querylength,
							/*plusp*/false,genestrand);

	    debug4t(
		    printf("%d mismatches on right at:",nmismatches_right);
		    for (i = 0; i <= nmismatches_right; i++) {
		      printf(" %d",mismatch_positions[i]);
		    }
		    printf("\n");
		    );

	    if (nmismatches_right == 0 || nmismatches_right <= max_mismatches_allowed ||
		mismatch_positions[nmismatches_right-1] < max_terminal_length) {
	      debug4t(printf(" => Long terminal at right: nmismatches_right %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_right,max_mismatches_allowed,mismatch_positions[nmismatches_right-1],max_terminal_length));
	      if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*//*querylength-truncate_pos_right*/querylength,
						/*left*/segment_left,query_compress_rev,
						querylength,/*plusp*/false,genestrand,
						/*start_endtype*/END,/*end_endtype*/TERM,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));

	    } else if (mismatch_positions[(nmismatches_right-1)/2] < querylength - max_terminal_length) {
	      debug4t(printf(" => Short terminal at right: nmismatches_right %d vs max_mismatches_allowed %d, last mismatch %d vs terminal pos %d",
			     nmismatches_right,max_mismatches_allowed,mismatch_positions[(nmismatches_right-1)/2],querylength-max_terminal_length));
	      if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*//*querylength-truncate_pos_right*/querylength,
						/*left*/segment_left,query_compress_rev,
						querylength,/*plusp*/false,genestrand,
						/*start_endtype*/END,/*end_endtype*/TERM,
						segment->chrnum,segment->chroffset,
						segment->chrhigh,segment->chrlength,max_mismatches_allowed,
						/*sarrayp*/false)) != NULL) {
		debug4t(printf(" => yes, with %d matches",Stage3end_nmatches(hit)));
		terminals = List_push(terminals,(void *) hit);
	      } else {
		debug4t(printf(" => no"));
	      }
	      debug4t(printf("\n"));
	    }
	  }
	}
      }
    }
  }

  debug4t(printf("Total number of terminals: %d\n",List_length(terminals)));

  return terminals;
}



#if 0
/* Finds terminals just by looking at querypos5 and querypos3 */
static List_T
find_terminals_by_width_only (struct Segment_T *plus_segments, int plus_nsegments,
			      struct Segment_T *minus_segments, int minus_nsegments,
#ifdef DEBUG4T
			      char *queryuc_ptr, /* for debugging */ char *queryrc,
#endif
			      Floors_T floors, int querylength, int query_lastpos,
			      Compress_T query_compress_fwd, Compress_T query_compress_rev,
			      int max_mismatches_allowed, int max_terminal_length,
			      int genestrand) {
#ifdef DEBUG4T
  char *gbuffer;
#endif
  List_T terminals = (List_T) NULL;
  Segment_T segment;
  Stage3end_T hit;
  Univcoord_T segment_left;

#ifdef DEBUG4T
  int i;
#endif

  debug(printf("identify_terminals: Checking up to %d mismatches\n",max_mismatches_allowed));

#if 0
  if (floors == NULL) {
    return (List_T) NULL;

  } else {
    floors_from_neg3 = floors->scorefrom[-index1interval];
    floors_to_pos3 = floors->scoreto[query_lastpos+index1interval];

    if (max_terminal_length > querylength/3) {
      max_terminal_length = querylength/3;
    }
  }
#endif

  if (plus_nsegments > 0) {
    for (segment = plus_segments; segment < &(plus_segments[plus_nsegments]); segment++) {
      if (0 && segment->usedp == true) {
	/* Previously skipped, but looks like a bad idea */
      } else if (segment->diagonal < (Univcoord_T) -1) {
	debug4t(printf("identify_terminals_plus: Checking up to %d mismatches at diagonal %lu (querypos %d..%d)\n",
		       max_mismatches_allowed,segment->diagonal,segment->querypos5,segment->querypos3));

	if (segment->querypos3 - segment->querypos5 > index1part) {
	  segment_left = segment->diagonal - querylength; /* FORMULA: Corresponds to querypos 0 */
	  debug4t(
		  gbuffer = (char *) CALLOC(querylength+1,sizeof(char));
		  Genome_fill_buffer_blocks(segment_left,querylength,gbuffer);
		  printf("genome 0..: %s\n",gbuffer);
		  printf("query  0..: %s\n",queryuc_ptr);
		  FREE(gbuffer);
		  );

	  if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*/querylength,
					    /*left*/segment_left,query_compress_fwd,
					    querylength,/*plusp*/true,genestrand,
					    segment->chrnum,segment->chroffset,
					    segment->chrhigh,segment->chrlength,max_mismatches_allowed,
					    /*sarrayp*/false)) != NULL) {
	    debug4t(printf(" => Terminal\n"));
	    terminals = List_push(terminals,(void *) hit);
	  }
	}
      }
    }
  }

  if (minus_nsegments > 0) {
    for (segment = minus_segments; segment < &(minus_segments[minus_nsegments]); segment++) {
      if (0 && segment->usedp == true) {
	/* Previously skipped, but looks like a bad idea */
      } else if (segment->diagonal < (Univcoord_T) -1) {
	debug4t(printf("identify_terminals_minus: Getting genome at diagonal %lu (querypos %d..%d)\n",
		       segment->diagonal,segment->querypos5,segment->querypos3));

	if (segment->querypos3 - segment->querypos5 > index1part) {
	  segment_left = segment->diagonal - querylength;
	  debug4t(
		  gbuffer = (char *) CALLOC(querylength+1,sizeof(char));
		  Genome_fill_buffer_blocks(segment_left,querylength,gbuffer);
		  printf("genome   0..: %s\n",gbuffer);
		  printf("query.rc 0..: %s\n",queryrc);
		  FREE(gbuffer);
		  );

	  if ((hit = Stage3end_new_terminal(/*querystart*/0,/*queryend*/querylength,
					    /*left*/segment_left,query_compress_rev,
					    querylength,/*plusp*/false,genestrand,
					    segment->chrnum,segment->chroffset,
					    segment->chrhigh,segment->chrlength,max_mismatches_allowed,
					    /*sarrayp*/false)) != NULL) {
	    debug4t(printf(" => Terminal\n"));
	    terminals = List_push(terminals,(void *) hit);
	  }
	}
      }
    }
  }

  debug4t(printf("Total number of terminals: %d\n",List_length(terminals)));
  fprintf(stderr,"Total number of terminals: %d\n",List_length(terminals));
  return terminals;
}
#endif



static void
fetch_positions_for_all_12mers (T this, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev, int query_lastpos) {
  int querypos;

  /* querypos -2, -1, query_lastpos+1, and query_lastpos+2 are special cases */
  /* if allvalidp is true, then 0 and query_lastpos should have been done already */
  for (querypos = 0; querypos <= query_lastpos; querypos++) {
    if (this->plus_retrievedp[querypos] == false) {
      /* FORMULA */
      this->plus_positions[querypos] =
	Indexdb_read_inplace(&(this->plus_npositions[querypos]),indexdb_fwd,this->forward_oligos[querypos]);
      debug(printf("Retrieving at querypos %d, plus_npositions = %d\n",
		   querypos,this->plus_npositions[querypos]));
      this->plus_retrievedp[querypos] = true;
#ifdef USE_ALLOCP
      this->plus_allocp[querypos] = false;
#endif
    }
    if (this->minus_retrievedp[querypos] == false) {
      /* FORMULA */
      this->minus_positions[querypos] =
	Indexdb_read_inplace(&(this->minus_npositions[querypos]),indexdb_rev,this->revcomp_oligos[querypos]);
      debug(printf("Retrieving at querypos %d, minus_npositions = %d\n",
		   querypos,this->minus_npositions[querypos]));
      this->minus_retrievedp[querypos] = true;
#ifdef USE_ALLOCP
      this->minus_allocp[querypos] = false;
#endif
    }
  }

  this->all_positions_fetched_p = true;

  return;
}



static bool
intragenic_splice_p (Chrpos_T splicedistance, Substring_T donor, Substring_T acceptor) {
  int knowni;

  if ((knowni = Substring_splicesites_i(donor)) >= 0) {
    if (splicedists[knowni] >= splicedistance) {
      return true;
    }
  }

  if ((knowni = Substring_splicesites_i(acceptor)) >= 0) {
    if (splicedists[knowni] >= splicedistance) {
      return true;
    }
  }

  return false;
}



static List_T
find_splicepairs_distant (int *found_score, int *ndistantsplicepairs,
			  List_T *localsplicing, List_T distantsplicing_orig,
			  List_T *donors_plus, List_T *antidonors_plus,
			  List_T *acceptors_plus, List_T *antiacceptors_plus,
			  List_T *donors_minus, List_T *antidonors_minus,
			  List_T *acceptors_minus, List_T *antiacceptors_minus,
			  int localsplicing_penalty, int distantsplicing_penalty,
			  int querylength, int nmismatches_allowed, bool first_read_p) {
  List_T distantsplicing = NULL, p, q, qsave;
  Substring_T donor, acceptor;
  int min_endlength_1, min_endlength_2, nmismatches1, nmismatches2, pos;
  Chrpos_T distance;
  Univcoord_T donor_genomicstart, acceptor_genomicstart;
  bool shortdistancep;
  double nonidentity = 1.0 - min_distantsplicing_identity;
  Chrnum_T chrnum;

  debug(printf("Starting find_splicepairs_distant with nonidentity %f\n",nonidentity));
  debug4l(printf("Starting find_splicepairs_distant with nonidentity %f\n",nonidentity));

  if (nonidentity == 0.0) {
    nmismatches_allowed = 0;
  }

  for (nmismatches1 = 0; nmismatches1 <= nmismatches_allowed; nmismatches1++) {
    nmismatches2 = nmismatches_allowed - nmismatches1;

    if (nonidentity == 0.0) {
      min_endlength_1 = min_endlength_2 = min_distantsplicing_end_matches;
    } else {
      min_endlength_1 = rint((double) nmismatches1/nonidentity);
      if (min_endlength_1 < min_distantsplicing_end_matches) {
	min_endlength_1 = min_distantsplicing_end_matches;
      }
      min_endlength_2 = rint((double) nmismatches2/nonidentity);
      if (min_endlength_2 < min_distantsplicing_end_matches) {
	min_endlength_2 = min_distantsplicing_end_matches;
      }
    }

    debug4l(printf("  nmismatches1 = %d, nmismatches2 = %d, min_endlength_1 = %d, min_endlength_2 = %d\n",
		   nmismatches1,nmismatches2,min_endlength_1,min_endlength_2));

    /************************************************************************
     *   Same strands
     ************************************************************************/

    /* 1.  End 1 to End 2.  Same strands. */
    p = donors_plus[nmismatches1];
    q = acceptors_plus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): donors+ (%d) to acceptors+ (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end1-end2: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_1) {
	debug4ld(printf("chimera_pos of donor < min_endlength_1\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_2) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_2\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	/* Generate all pairs at this splice_pos */
	qsave = q;
	while (p != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_genomicstart(acceptor) == Substring_genomicstart(donor)) {
	      /* Skip.  Really a continuous match. */
	    } else {
	      if ((chrnum = Substring_chrnum(donor)) != Substring_chrnum(acceptor)) {
		distance = 0U;
		shortdistancep = false;
	      } else if ((acceptor_genomicstart = Substring_genomicstart(acceptor)) > (donor_genomicstart = Substring_genomicstart(donor))) {
		distance = acceptor_genomicstart - donor_genomicstart;
		if (distance <= shortsplicedist) {
		  shortdistancep = true;
		} else if (distances_observed_p == true &&
			   intragenic_splice_p(distance,donor,acceptor) == true) {
		  shortdistancep = true;
		} else {
		  shortdistancep = false;
		}
	      } else {
		distance = donor_genomicstart - acceptor_genomicstart;
		shortdistancep = false; /* scramble */
	      }
	      debug4ld(printf("1-2. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  shortdistancep = %d\n",
			      pos,min_endlength_1,querylength-min_endlength_2,
			      Substring_genomicstart(donor),Substring_genomicstart(acceptor),shortdistancep));

	      if (shortdistancep) {
		*localsplicing = List_push(*localsplicing,
					   (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									 donor,acceptor,distance,
									 /*shortdistancep*/true,localsplicing_penalty,querylength,
									 /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									 /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									 /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									 /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	      } else if (*ndistantsplicepairs <= MAXCHIMERAPATHS) {
		distantsplicing = List_push(distantsplicing,
					    (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									  donor,acceptor,distance,
									  /*shortdistancep*/false,distantsplicing_penalty,querylength,
									  /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									  /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									  /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									  /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
		(*ndistantsplicepairs)++;
	      }

	    }
	    q = q->rest;

	  }
	  p = p->rest;
	}
      }
    }

    /* 4. End 3 to End 4.  Same strands. */
    p = donors_minus[nmismatches1];
    q = acceptors_minus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): donors- (%d) to acceptors- (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end3-end4: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_1) {
	debug4ld(printf("chimera_pos of donor < min_endlength_1\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_2) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_2\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	qsave = q;
	while (p != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_genomicstart(acceptor) == Substring_genomicstart(donor)) {
	      /* Skip.  Really a continuous match. */
	    } else {
	      if ((chrnum = Substring_chrnum(donor)) != Substring_chrnum(acceptor)) {
		distance = 0U;
		shortdistancep = false;
	      } else if ((acceptor_genomicstart = Substring_genomicstart(acceptor)) > (donor_genomicstart = Substring_genomicstart(donor))) {
		distance = acceptor_genomicstart - donor_genomicstart;
		shortdistancep = false; /* scramble */
	      } else {
		distance = donor_genomicstart - acceptor_genomicstart;
		if (distance <= shortsplicedist) {
		  shortdistancep = true;
		} else if (distances_observed_p == true &&
			   intragenic_splice_p(distance,donor,acceptor) == true) {
		  shortdistancep = true;
		} else {
		  shortdistancep = false;
		}
	      }
	      debug4ld(printf("3-4. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  shortdistancep = %d.\n",
			      pos,min_endlength_1,querylength-min_endlength_2,
			      Substring_genomicstart(donor),Substring_genomicstart(acceptor),shortdistancep));
	      if (shortdistancep) {
		*localsplicing = List_push(*localsplicing,
					   (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									 donor,acceptor,distance,
									 /*shortdistancep*/true,localsplicing_penalty,querylength,
									 /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									 /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									 /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									 /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	      } else if (*ndistantsplicepairs <= MAXCHIMERAPATHS) {
		distantsplicing = List_push(distantsplicing,
					    (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									  donor,acceptor,distance,
									  /*shortdistancep*/false,distantsplicing_penalty,querylength,
									  /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									  /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									  /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									  /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
		(*ndistantsplicepairs)++;
	      }
	    }
	    q = q->rest;

	  }
	  p = p->rest;
	}
      }
    }

    /* 5. End 5 to End 6.  Same strands. */
    p = antidonors_plus[nmismatches1];
    q = antiacceptors_plus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): antidonors+ (%d) to antiacceptors+ (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end5-end6: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_2) {
	debug4ld(printf("chimera_pos of donor < min_endlength_2\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_1) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_1\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	qsave = q;
	while (p != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_genomicstart(acceptor) == Substring_genomicstart(donor)) {
	      /* Skip.  Really an continuous match. */
	    } else {
	      if ((chrnum = Substring_chrnum(donor)) != Substring_chrnum(acceptor)) {
		distance = 0U;
		shortdistancep = false;
	      } else if ((acceptor_genomicstart = Substring_genomicstart(acceptor)) > (donor_genomicstart = Substring_genomicstart(donor))) {
		distance = acceptor_genomicstart - donor_genomicstart;
		shortdistancep = false; /* scramble */
	      } else {
		distance = donor_genomicstart - acceptor_genomicstart;
		if (distance <= shortsplicedist) {
		  shortdistancep = true;
		} else if (distances_observed_p == true &&
			   intragenic_splice_p(distance,donor,acceptor) == true) {
		  shortdistancep = true;
		} else {
		  shortdistancep = false;
		}
	      }

	      debug4ld(printf("5-6. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  shortdistancep = %d\n",
			      pos,min_endlength_2,querylength-min_endlength_1,
			      Substring_genomicstart(donor),Substring_genomicstart(acceptor),shortdistancep));
	      if (shortdistancep) {
		*localsplicing = List_push(*localsplicing,
					   (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									 donor,acceptor,distance,
									 /*shortdistancep*/true,localsplicing_penalty,querylength,
									 /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									 /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									 /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									 /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	      } else if (*ndistantsplicepairs <= MAXCHIMERAPATHS) {
		distantsplicing = List_push(distantsplicing,
					    (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									  donor,acceptor,distance,
									  /*shortdistancep*/false,distantsplicing_penalty,querylength,
									  /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									  /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									  /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									  /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
		(*ndistantsplicepairs)++;
	      }
	    }
	    q = q->rest;

	  }
	  p = p->rest;
	}
      }
    }

    /* 8. End 7 to End 8.  Same strands. */
    p = antidonors_minus[nmismatches1];
    q = antiacceptors_minus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): antidonors- (%d) to antiacceptors- (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end7-end8: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_2) {
	debug4ld(printf("chimera_pos of donor < min_endlength_2\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_1) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_1\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	qsave = q;

	while (p != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL /* && *nsplicepairs <= MAXCHIMERAPATHS */ && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_genomicstart(acceptor) == Substring_genomicstart(donor)) {
	      /* Skip.  Really a continuous match. */
	    } else {
	      if ((chrnum = Substring_chrnum(donor)) != Substring_chrnum(acceptor)) {
		distance = 0U;
		shortdistancep = false;
	      } else if ((acceptor_genomicstart = Substring_genomicstart(acceptor)) > (donor_genomicstart = Substring_genomicstart(donor))) {
		distance = acceptor_genomicstart - donor_genomicstart;
		if (distance <= shortsplicedist) {
		  shortdistancep = true;
		} else if (distances_observed_p == true &&
			   intragenic_splice_p(distance,donor,acceptor) == true) {
		  shortdistancep = true;
		} else {
		  shortdistancep = false;
		}
	      } else {
		distance = donor_genomicstart - acceptor_genomicstart;
		shortdistancep = false; /* scramble */
	      }
	      debug4ld(printf("7-8. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  shortdistancep = %d.\n",
			      pos,min_endlength_2,querylength-min_endlength_1,
			      Substring_genomicstart(donor),Substring_genomicstart(acceptor),shortdistancep));
	      if (shortdistancep) {
		*localsplicing = List_push(*localsplicing,
					   (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									 donor,acceptor,distance,
									 /*shortdistancep*/true,localsplicing_penalty,querylength,
									 /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									 /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									 /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									 /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	      } else if (*ndistantsplicepairs <= MAXCHIMERAPATHS) {
		distantsplicing = List_push(distantsplicing,
					    (void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
									  donor,acceptor,distance,
									  /*shortdistancep*/false,distantsplicing_penalty,querylength,
									  /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
									  /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
									  /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
									  /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
		(*ndistantsplicepairs)++;
	      }
	    }
	    q = q->rest;

	  }
	  p = p->rest;
	}
      }
    }


    /************************************************************************
     *   Different strands
     ************************************************************************/

    /* 2. End 1 to End 4.  Different strands. */
    p = donors_plus[nmismatches1];
    q = acceptors_minus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): donors+ (%d) to acceptors- (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end1-end4: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_1) {
	debug4ld(printf("chimera_pos of donor < min_endlength_1\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_2) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_2\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	qsave = q;
	while (p != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_chrnum(donor) != Substring_chrnum(acceptor)) {
	      distance = 0U;
	    } else if ((Substring_genomicstart(acceptor) - pos) > (Substring_genomicstart(donor) + pos)) {
	      distance = (Substring_genomicstart(acceptor) - pos) - (Substring_genomicstart(donor) + pos);
	    } else {
	      distance = (Substring_genomicstart(donor) + pos) - (Substring_genomicstart(acceptor) - pos);
	    }
	    debug4ld(printf("1-4. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  Different strands, so not shortdistance.\n",
			    pos,min_endlength_1,querylength-min_endlength_2,
			    Substring_genomicstart(donor),Substring_genomicstart(acceptor)));
	    distantsplicing = List_push(distantsplicing,
					(void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
								      donor,acceptor,distance,
								      /*shortdistancep*/false,distantsplicing_penalty,querylength,
								      /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								      /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								      /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
								      /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	    (*ndistantsplicepairs)++;
	    q = q->rest;
	  }
	  p = p->rest;
	}
      }
    }

    /* 3. End 3 to End 2.  Different strands. */
    p = donors_minus[nmismatches1];
    q = acceptors_plus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): donors- (%d) to acceptors+ (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end3-end2: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_1) {
	debug4ld(printf("chimera_pos of donor < min_endlength_1\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_2) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_2\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	qsave = q;
	while (p != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_chrnum(donor) != Substring_chrnum(acceptor)) {
	      distance = 0U;
	    } else if (Substring_genomicstart(acceptor) > Substring_genomicstart(donor)) {
	      distance = (Substring_genomicstart(acceptor) + pos) - (Substring_genomicstart(donor) - pos);
	    } else {
	      distance = (Substring_genomicstart(donor) - pos) - (Substring_genomicstart(acceptor) + pos);
	    }
	    debug4ld(printf("3-2. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  Different strands so not shortdistance.\n",
			    pos,min_endlength_1,querylength-min_endlength_2,
			    Substring_genomicstart(donor),Substring_genomicstart(acceptor)));
	    distantsplicing = List_push(distantsplicing,
					(void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
								      donor,acceptor,distance,
								      /*shortdistancep*/false,distantsplicing_penalty,querylength,
								      /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								      /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								      /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
								      /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	    (*ndistantsplicepairs)++;
	    q = q->rest;
	  }
	  p = p->rest;
	}
      }
    }


    /* 6. End 5 to End 8.  Different strands. */
    p = antidonors_plus[nmismatches1];
    q = antiacceptors_minus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): antidonors+ (%d) to antiacceptors- (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end5-end8: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_2) {
	debug4ld(printf("chimera_pos of donor < min_endlength_2\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_1) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_1\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	qsave = q;
	while (p != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_chrnum(donor) != Substring_chrnum(acceptor)) {
	      distance = 0U;
	    } else if ((Substring_genomicstart(acceptor) - pos) > (Substring_genomicstart(donor) + pos)) {
	      distance = (Substring_genomicstart(acceptor) - pos) - (Substring_genomicstart(donor) + pos);
	    } else {
	      distance = (Substring_genomicstart(donor) + pos) - (Substring_genomicstart(acceptor) - pos);
	    }
	    debug4ld(printf("5-8. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  Different strands so not shortdistance.\n",
			    pos,min_endlength_2,querylength-min_endlength_1,
			    Substring_genomicstart(donor),Substring_genomicstart(acceptor)));
	    distantsplicing = List_push(distantsplicing,
					(void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
								      donor,acceptor,distance,
								      /*shortdistancep*/false,distantsplicing_penalty,querylength,
								      /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								      /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								      /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
								      /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	    (*ndistantsplicepairs)++;
	    q = q->rest;
	  }
	  p = p->rest;
	}
      }
    }

    /* 7. End 7 to End 6.  Different strands. */
    p = antidonors_minus[nmismatches1];
    q = antiacceptors_plus[nmismatches2];
    debug4l(printf("find_splicepairs_known_distant (%d+%d mismatches): antidonors- (%d) to antiacceptors+ (%d)\n",
		   nmismatches1,nmismatches2,List_length(p),List_length(q)));
    while (p != NULL && q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS) {
      donor = (Substring_T) p->first;
      acceptor = (Substring_T) q->first;
      debug4ld(printf("end7-end6: donor at %lu and acceptor at %lu\n",Substring_genomicstart(donor),Substring_genomicstart(acceptor)));

      if ((pos = Substring_chimera_pos(donor)) < min_endlength_2) {
	debug4ld(printf("chimera_pos of donor < min_endlength_2\n"));
	p = p->rest;
      } else if (pos > querylength - min_endlength_1) {
	debug4ld(printf("chimera_pos of donor > querylength - min_endlength_1\n"));
	p = p->rest;
      } else if (pos < Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d < chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	p = p->rest;
      } else if (pos > Substring_chimera_pos(acceptor)) {
	debug4ld(printf("chimera_pos of donor %d > chimera_pos of acceptor %d\n",pos,Substring_chimera_pos(acceptor)));
	q = q->rest;
      } else {
	qsave = q;
	while (p != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) p->first)) == pos) {
	  donor = (Substring_T) p->first;
	  debug4ld(printf("donor at %lu, pos %d\n",Substring_genomicstart(donor),pos));
	  q = qsave;
	  while (q != NULL && *ndistantsplicepairs <= MAXCHIMERAPATHS && Substring_chimera_pos(((Substring_T) q->first)) == pos) {
	    acceptor = (Substring_T) q->first;
	    debug4ld(printf("acceptor at %lu, pos %d\n",Substring_genomicstart(acceptor),pos));
	    if (Substring_chrnum(donor) != Substring_chrnum(acceptor)) {
	      distance = 0U;
	    } else if ((Substring_genomicstart(acceptor) + pos) > (Substring_genomicstart(donor) - pos)) {
	      distance = (Substring_genomicstart(acceptor) + pos) - (Substring_genomicstart(donor) - pos);
	    } else {
	      distance = (Substring_genomicstart(donor) - pos) - (Substring_genomicstart(acceptor) + pos);
	    }
	    debug4ld(printf("7-6. Pushing a candidate at splice_pos %d (%d..%d), donor %lu to acceptor %lu.  Different strands so not shortdistance.\n",
			    pos,min_endlength_2,querylength-min_endlength_1,
			    Substring_genomicstart(donor),Substring_genomicstart(acceptor)));
	    distantsplicing = List_push(distantsplicing,
					(void *) Stage3end_new_splice(&(*found_score),nmismatches1,nmismatches2,
								      donor,acceptor,distance,
								      /*shortdistancep*/false,distantsplicing_penalty,querylength,
								      /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								      /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								      /*copy_donor_p*/true,/*copy_acceptor_p*/true,first_read_p,
								      /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	    (*ndistantsplicepairs)++;
	    q = q->rest;
	  }
	  p = p->rest;
	}
      }
    }
  }

  debug4l(printf("ndistantsplicepairs %d, maxchimerapaths %d\n",*ndistantsplicepairs,MAXCHIMERAPATHS));
  if (*ndistantsplicepairs > MAXCHIMERAPATHS) {
    /* Can afford to ignore these if MAXCHIMERAPATHS is set high enough */
    stage3list_gc(&distantsplicing);
    return distantsplicing_orig;
  } else {
    return List_append(distantsplicing_orig,distantsplicing);
  }
}


static List_T
find_splicepairs_shortend (int *found_score, List_T hits,
			   List_T *donors_plus, List_T *antidonors_plus,
			   List_T *acceptors_plus, List_T *antiacceptors_plus,
			   List_T *donors_minus, List_T *antidonors_minus,
			   List_T *acceptors_minus, List_T *antiacceptors_minus,
			   Compress_T query_compress_fwd, Compress_T query_compress_rev,
			   char *queryuc_ptr, char *queryrc, int min_shortend,
			   int localsplicing_penalty,
			   int max_mismatches_allowed, int querylength, bool pairedp, bool first_read_p,
			   int genestrand) {
  List_T p;
  Substring_T donor, acceptor;
  Intlist_T splicesites_i;
  Intlist_T nmismatches_list;
  int nmismatches, nmismatches_shortend, nmisses_allowed, support, endlength;
  int amb_nmatches;
#ifdef DEBUG4H
  Univcoord_T leftbound, rightbound;
#endif
  Univcoord_T bestleft, origleft, chroffset, chrhigh;
  int i;
  int bestj = 0;


  debug(printf("Starting find_splicepairs_shortend\n"));
  debug(
	for (nmismatches = 0; nmismatches <= max_mismatches_allowed; nmismatches++) {
	  printf("At %d nmismatches: +donors/acceptors %d/%d, +antidonors/antiacceptors %d/%d, -donors/acceptors %d/%d, -antidonors/antiacceptors %d/%d\n",
		 nmismatches,
		 List_length(donors_plus[nmismatches]),
		 List_length(acceptors_plus[nmismatches]),
		 List_length(antidonors_plus[nmismatches]),
		 List_length(antiacceptors_plus[nmismatches]),
		 List_length(donors_minus[nmismatches]),
		 List_length(acceptors_minus[nmismatches]),
		 List_length(antidonors_minus[nmismatches]),
		 List_length(antiacceptors_minus[nmismatches]));
	});

  /* Donors and antiacceptors => Want chimera_pos to be at end */
  /* Acceptors and antidonors => Want chimera_pos to be at beginning */

  /* Note: Previously checked endlength <=
     min_localsplicing_end_matches.  But this missed some ends.  Now
     just checking endlength <= support, to see if we are more than
     halfway. */

  /* Don't want to end when first set of hits found */
  for (nmismatches = 0; /* hits == NULL && */ nmismatches <= max_mismatches_allowed;
       nmismatches++) {
    nmisses_allowed = max_mismatches_allowed - nmismatches;

    /* End 1 */
    for (p = donors_plus[nmismatches]; p != NULL; p = p->rest) {
      donor = (Substring_T) p->first;
      support = Substring_chimera_pos(donor);
      endlength = querylength - support;
      chrhigh = Substring_chrhigh(donor);
      
#ifdef DEBUG4H
      chroffset = Substring_chroffset(donor);
      leftbound = Substring_alignend_trim(donor) + 1;
#endif
      debug4h(printf("End 1: short-overlap donor_plus: #%d:%u, endlength %d\n",
		     Substring_chrnum(donor),(Chrpos_T) (leftbound-1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 1: short-overlap donor_plus: #%d:%u (%d mismatches) => searching right\n",
		       Substring_chrnum(donor),(Chrpos_T) (leftbound-1-chroffset),Substring_nmismatches_whole(donor)));

	if ((i = Substring_splicesites_i(donor)) >= 0) {
	  origleft = Substring_genomicstart(donor);
	  if ((splicesites_i = 
	       Splicetrie_find_right(&nmismatches_shortend,&nmismatches_list,i,
				     origleft,/*pos5*/support,/*pos3*/querylength,chrhigh,
				     query_compress_fwd,/*queryptr*/queryuc_ptr,
				     nmisses_allowed,/*plusp*/true,genestrand,
				     /*collect_all_p*/pairedp == true && first_read_p == true)) != NULL) {
	    
	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 1: short-overlap donor_plus: Successful ambiguous from donor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(donor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								  donor,/*acceptor*/NULL,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/NULL,/*ambi_right*/splicesites_i,
								  /*amb_nmismatches_left*/NULL,nmismatches_list,
								  /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								  /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - support;
	      if ((acceptor = Substring_new_acceptor(/*splicesites_i*/bestj,/*joffset*/0,Substring_chimera_pos(donor),nmismatches_shortend,
						     /*prob*/2.0,/*left*/bestleft,query_compress_fwd,
						     querylength,/*plusp*/true,genestrand,/*sensep*/true,
						     Substring_chrnum(donor),Substring_chroffset(donor),
						     Substring_chrhigh(donor),Substring_chrlength(donor))) != NULL) {
		debug4h(printf("End 1: short-overlap donor_plus: Successful splice from donor #%d to acceptor #%d\n",
			       Substring_splicesites_i(donor),Substring_splicesites_i(acceptor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								    donor,acceptor,/*distance*/bestleft-origleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								    /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

    /* End 2 */
    for (p = acceptors_plus[nmismatches]; p != NULL; p = p->rest) {
      acceptor = (Substring_T) p->first;
      endlength = Substring_chimera_pos(acceptor);
      support = querylength - endlength;
      chroffset = Substring_chroffset(acceptor);

#ifdef DEBUG4H
      rightbound = Substring_alignstart_trim(acceptor);
#endif

      debug4h(printf("End 2: short-overlap acceptor_plus: #%d:%u, endlength %d\n",
		     Substring_chrnum(acceptor),(Chrpos_T) (rightbound+1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 2: short-overlap acceptor_plus: #%d:%u (%d mismatches) => searching left\n",
		       Substring_chrnum(acceptor),(Chrpos_T) (rightbound+1-chroffset),Substring_nmismatches_whole(acceptor)));

	if ((i = Substring_splicesites_i(acceptor)) >= 0) {
	  origleft = Substring_genomicstart(acceptor);
	  if ((splicesites_i =
	       Splicetrie_find_left(&nmismatches_shortend,&nmismatches_list,i,
				    origleft,/*pos5*/0,/*pos3*/endlength,chroffset,
				    query_compress_fwd,/*queryptr*/queryuc_ptr,querylength,
				    nmisses_allowed,/*plusp*/true,genestrand,
				    /*collect_all_p*/pairedp == true && first_read_p == false)) != NULL) {

	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 2: short-overlap acceptor_plus: Successful ambiguous from acceptor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(acceptor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								  /*donor*/NULL,acceptor,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/splicesites_i,/*ambi_right*/NULL,
								  nmismatches_list,/*amb_nmismatches_right*/NULL,
								  /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								  /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));

	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - endlength;
	      if ((donor = Substring_new_donor(/*splicesites_i*/bestj,/*joffset*/0,Substring_chimera_pos(acceptor),nmismatches_shortend,
					       /*prob*/2.0,/*left*/bestleft,query_compress_fwd,
					       querylength,/*plusp*/true,genestrand,/*sensep*/true,
					       Substring_chrnum(acceptor),Substring_chroffset(acceptor),
					       Substring_chrhigh(acceptor),Substring_chrlength(acceptor))) != NULL) {
		debug4h(printf("End 2: short-overlap acceptor_plus: Successful splice from acceptor #%d to donor #%d\n",
			       Substring_splicesites_i(acceptor),Substring_splicesites_i(donor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								    donor,acceptor,/*distance*/origleft-bestleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								    /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

    /* End 3 */
    for (p = donors_minus[nmismatches]; p != NULL; p = p->rest) {
      donor = (Substring_T) p->first;
      support = Substring_chimera_pos(donor);
      endlength = querylength - support;
      chroffset = Substring_chroffset(donor);

#ifdef DEBUG4H
      rightbound = Substring_alignend_trim(donor);
#endif

      debug4h(printf("End 3: short-overlap donor_minus: #%d:%u, endlength %d\n",
		     Substring_chrnum(donor),(Chrpos_T) (rightbound+1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 3: short-overlap donor_minus: #%d:%u (%d mismatches) => searching left\n",
		       Substring_chrnum(donor),(Chrpos_T) (rightbound+1-chroffset),Substring_nmismatches_whole(donor)));

	if ((i = Substring_splicesites_i(donor)) >= 0) {
	  origleft = Substring_genomicend(donor);
	  if ((splicesites_i =
	       Splicetrie_find_left(&nmismatches_shortend,&nmismatches_list,i,
				    origleft,/*pos5*/0,/*pos3*/endlength,chroffset,
				    query_compress_rev,/*queryptr*/queryrc,querylength,
				    nmisses_allowed,/*plusp*/false,genestrand,
				    /*collect_all_p*/pairedp == true && first_read_p == true)) != NULL) {

	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 3: short-overlap donor_minus: Successful ambiguous from donor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(donor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								  donor,/*acceptor*/NULL,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/splicesites_i,/*ambi_right*/NULL,
								  nmismatches_list,/*amb_nmismatches_right*/NULL,
								  /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								  /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - endlength;
	      if ((acceptor = Substring_new_acceptor(/*splicesites_i*/bestj,/*joffset*/0,
						     querylength-Substring_chimera_pos(donor),nmismatches_shortend,
						     /*prob*/2.0,/*left*/bestleft,query_compress_rev,
						     querylength,/*plusp*/false,genestrand,/*sensep*/true,
						     Substring_chrnum(donor),Substring_chroffset(donor),
						     Substring_chrhigh(donor),Substring_chrlength(donor))) != NULL) {
		debug4h(printf("End 3: short-overlap donor_minus: Successful splice from donor #%d to acceptor #%d\n",
			       Substring_splicesites_i(donor),Substring_splicesites_i(acceptor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								    donor,acceptor,/*distance*/origleft-bestleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								    /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

    /* End 4 */
    for (p = acceptors_minus[nmismatches]; p != NULL; p = p->rest) {
      acceptor = (Substring_T) p->first;
      endlength = Substring_chimera_pos(acceptor);
      support = querylength - endlength;
      chrhigh = Substring_chrhigh(acceptor);

#ifdef DEBUG4H
      chroffset = Substring_chroffset(acceptor);
      leftbound = Substring_alignstart_trim(acceptor) + 1;
#endif

      debug4h(printf("End 4: short-overlap acceptor_minus: #%d:%u, endlength %d\n",
		     Substring_chrnum(acceptor),(Chrpos_T) (leftbound-1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 4: short-overlap acceptor_minus: #%d:%u (%d mismatches) => searching right\n",
		       Substring_chrnum(acceptor),(Chrpos_T) (leftbound-1-chroffset),Substring_nmismatches_whole(acceptor)));

	if ((i = Substring_splicesites_i(acceptor)) >= 0) {
	  origleft = Substring_genomicend(acceptor);
	  if ((splicesites_i =
	       Splicetrie_find_right(&nmismatches_shortend,&nmismatches_list,i,
				     origleft,/*pos5*/support,/*pos3*/querylength,chrhigh,
				     query_compress_rev,/*queryptr*/queryrc,
				     nmisses_allowed,/*plusp*/false,genestrand,
				     /*collect_all_p*/pairedp == true && first_read_p == false)) != NULL) {

	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 4: short-overlap acceptor_minus: Successful ambiguous from acceptor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(acceptor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								  /*donor*/NULL,acceptor,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/NULL,/*ambi_right*/splicesites_i,
								  /*amb_nmismatches_left*/NULL,nmismatches_list,
								  /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								  /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - support;
	      if ((donor = Substring_new_donor(/*splicesites_i*/bestj,/*joffset*/0,
					       querylength-Substring_chimera_pos(acceptor),nmismatches_shortend,
					       /*prob*/2.0,/*left*/bestleft,query_compress_rev,
					       querylength,/*plusp*/false,genestrand,/*sensep*/true,
					       Substring_chrnum(acceptor),Substring_chroffset(acceptor),
					       Substring_chrhigh(acceptor),Substring_chrlength(acceptor))) != NULL) {
		debug4h(printf("End 4: short-overlap acceptor_minus: Successful splice from acceptor #%d to #%d\n",
			       Substring_splicesites_i(acceptor),Substring_splicesites_i(donor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								    donor,acceptor,/*distance*/bestleft-origleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								    /*sensedir*/SENSE_FORWARD,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

    /* End 5 */
    for (p = antidonors_plus[nmismatches]; p != NULL; p = p->rest) {
      donor = (Substring_T) p->first;
      endlength = Substring_chimera_pos(donor);
      support = querylength - endlength;
      chroffset = Substring_chroffset(donor);

#ifdef DEBUG4H
      rightbound = Substring_alignstart_trim(donor);
#endif

      debug4h(printf("End 5: short-overlap antidonor_plus: #%d:%u, endlength %d\n",
		     Substring_chrnum(donor),(Chrpos_T) (rightbound+1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 5: short-overlap antidonor_plus: #%d:%u (%d mismatches) => searching left\n",
		       Substring_chrnum(donor),(Chrpos_T) (rightbound+1-chroffset),Substring_nmismatches_whole(donor)));

	if ((i = Substring_splicesites_i(donor)) >= 0) {
	  origleft = Substring_genomicstart(donor);
	  if ((splicesites_i =
	       Splicetrie_find_left(&nmismatches_shortend,&nmismatches_list,i,
				    origleft,/*pos5*/0,/*pos3*/endlength,chroffset,
				    query_compress_fwd,/*queryptr*/queryuc_ptr,querylength,
				    nmisses_allowed,/*plusp*/true,genestrand,
				    /*collect_all_p*/pairedp == true && first_read_p == false)) != NULL) {

	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 5: short-overlap antidonor_plus: Successful ambiguous from antidonor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(donor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								  donor,/*acceptor*/NULL,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/splicesites_i,/*ambi_right*/NULL,
								  nmismatches_list,/*amb_nmismatches_right*/NULL,
								  /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								  /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - endlength;
	      if ((acceptor = Substring_new_acceptor(/*splicesites_i*/bestj,/*joffset*/0,Substring_chimera_pos(donor),nmismatches_shortend,
						     /*prob*/2.0,/*left*/bestleft,query_compress_fwd,
						     querylength,/*plusp*/true,genestrand,/*sensep*/false,
						     Substring_chrnum(donor),Substring_chroffset(donor),
						     Substring_chrhigh(donor),Substring_chrlength(donor))) != NULL) {
		debug4h(printf("End 5: short-overlap antidonor_plus: Successful splice from antidonor #%d to antiacceptor #%d\n",
			       Substring_splicesites_i(donor),Substring_splicesites_i(acceptor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								    donor,acceptor,/*distance*/origleft-bestleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								    /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

    /* End 6 */
    for (p = antiacceptors_plus[nmismatches]; p != NULL; p = p->rest) {
      acceptor = (Substring_T) p->first;
      support = Substring_chimera_pos(acceptor);
      endlength = querylength - support;
      chrhigh = Substring_chrhigh(acceptor);

#ifdef DEBUG4H
      chroffset = Substring_chroffset(acceptor);
      leftbound = Substring_alignend_trim(acceptor) + 1;
#endif

      debug4h(printf("End 6: short-overlap antiacceptor_plus: #%d:%u, endlength %d\n",
		     Substring_chrnum(acceptor),(Chrpos_T) (leftbound-1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 6: short-overlap antiacceptor_plus: #%d:%u (%d mismatches) => searching right\n",
		       Substring_chrnum(acceptor),(Chrpos_T) (leftbound-1-chroffset),Substring_nmismatches_whole(acceptor)));

	if ((i = Substring_splicesites_i(acceptor)) >= 0) {
	  origleft = Substring_genomicstart(acceptor);
	  if ((splicesites_i =
	       Splicetrie_find_right(&nmismatches_shortend,&nmismatches_list,i,
				     origleft,/*pos5*/support,/*pos3*/querylength,chrhigh,
				     query_compress_fwd,/*queryptr*/queryuc_ptr,
				     nmisses_allowed,/*plusp*/true,genestrand,
				     /*collect_all_p*/pairedp == true && first_read_p == true)) != NULL) {

	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 6: short-overlap antiacceptor_plus: Successful ambiguous from antiacceptor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(acceptor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								  /*donor*/NULL,acceptor,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/NULL,/*ambi_right*/splicesites_i,
								  /*amb_nmismatches_left*/NULL,nmismatches_list,
								  /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								  /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - support;
	      if ((donor = Substring_new_donor(/*splicesites_i*/bestj,/*joffset*/0,Substring_chimera_pos(acceptor),nmismatches_shortend,
					       /*prob*/2.0,/*left*/bestleft,query_compress_fwd,
					       querylength,/*plusp*/true,genestrand,/*sensep*/false,
					       Substring_chrnum(acceptor),Substring_chroffset(acceptor),
					       Substring_chrhigh(acceptor),Substring_chrlength(acceptor))) != NULL) {
		debug4h(printf("End 6: short-overlap antiacceptor_plus: Successful splice from antiacceptor #%d to antidonor #%d\n",
			       Substring_splicesites_i(acceptor),Substring_splicesites_i(donor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								    donor,acceptor,/*distance*/bestleft-origleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								    /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

    /* End 7 */
    for (p = antidonors_minus[nmismatches]; p != NULL; p = p->rest) {
      donor = (Substring_T) p->first;
      endlength = Substring_chimera_pos(donor);
      support = querylength - endlength;
      chrhigh = Substring_chrhigh(donor);

#ifdef DEBUG4H
      chroffset = Substring_chroffset(donor);
      leftbound = Substring_alignstart_trim(donor) + 1;
#endif

      debug4h(printf("End 7: short-overlap antidonor_minus: #%d:%u, endlength %d\n",
		     Substring_chrnum(donor),(Chrpos_T) (leftbound-1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 7: short-overlap antidonor_minus: #%d:%u (%d mismatches) => searching right\n",
		       Substring_chrnum(donor),(Chrpos_T) (leftbound-1-chroffset),Substring_nmismatches_whole(donor)));

	if ((i = Substring_splicesites_i(donor)) >= 0) {
	  origleft = Substring_genomicend(donor);
	  if ((splicesites_i =
	       Splicetrie_find_right(&nmismatches_shortend,&nmismatches_list,i,
				     origleft,/*pos5*/support,/*pos3*/querylength,chrhigh,
				     query_compress_rev,/*queryptr*/queryrc,
				     nmisses_allowed,/*plusp*/false,genestrand,
				     /*collect_all_p*/pairedp == true && first_read_p == false)) != NULL) {

	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 7: short-overlap antidonor_minus: Successful ambiguous from antidonor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(donor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								  donor,/*acceptor*/NULL,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/NULL,/*ambi_right*/splicesites_i,
								  /*amb_nmismatches_left*/NULL,nmismatches_list,
								  /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								  /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - support;
	      if ((acceptor = Substring_new_acceptor(/*splicesites_i*/bestj,/*joffset*/0,
						     querylength-Substring_chimera_pos(donor),nmismatches_shortend,
						     /*prob*/2.0,/*left*/bestleft,query_compress_rev,
						     querylength,/*plusp*/false,genestrand,/*sensep*/false,
						     Substring_chrnum(donor),Substring_chroffset(donor),
						     Substring_chrhigh(donor),Substring_chrlength(donor))) != NULL) {
		debug4h(printf("End 7: short-overlap antidonor_minus: Successful splice from antidonor #%d to antiacceptor #%d\n",
			       Substring_splicesites_i(donor),Substring_splicesites_i(acceptor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches,nmismatches_shortend,
								    donor,acceptor,/*distance*/bestleft-origleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/true,/*copy_acceptor_p*/false,first_read_p,
								    /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

    /* End 8 */
    for (p = antiacceptors_minus[nmismatches]; p != NULL; p = p->rest) {
      acceptor = (Substring_T) p->first;
      support = Substring_chimera_pos(acceptor);
      endlength = querylength - support;
      chroffset = Substring_chroffset(acceptor);

#ifdef DEBUG4H
      rightbound = Substring_alignend_trim(acceptor);
#endif

      debug4h(printf("End 8: short-overlap antiacceptor_minus: #%d:%u, endlength %d\n",
		     Substring_chrnum(acceptor),(Chrpos_T) (rightbound+1-chroffset),endlength));

      if (endlength <= support) {
	debug4h(printf("End 8: short-overlap antiacceptor_minus: #%d:%u (%d mismatches) => searching left\n",
		       Substring_chrnum(acceptor),(Chrpos_T) (rightbound+1-chroffset),Substring_nmismatches_whole(acceptor)));

	if ((i = Substring_splicesites_i(acceptor)) >= 0) {
	  origleft = Substring_genomicend(acceptor);
	  if ((splicesites_i =
	       Splicetrie_find_left(&nmismatches_shortend,&nmismatches_list,i,
				    origleft,/*pos5*/0,/*pos3*/endlength,chroffset,
				    query_compress_rev,/*queryptr*/queryrc,querylength,
				    nmisses_allowed,/*plusp*/false,genestrand,
				    /*collect_all_p*/pairedp == true && first_read_p == true)) != NULL) {

	    if (endlength < min_shortend || Intlist_length(splicesites_i) > 1) {
	      amb_nmatches = endlength - nmismatches_shortend;
	      debug4h(printf("End 8: short-overlap antiacceptor_minus: Successful ambiguous from antiacceptor #%d with amb_nmatches %d\n",
			     Substring_splicesites_i(acceptor),amb_nmatches));
	      hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								  /*donor*/NULL,acceptor,/*distance*/0U,
								  /*shortdistancep*/false,/*penalty*/0,querylength,
								  amb_nmatches,/*ambi_left*/splicesites_i,/*ambi_right*/NULL,
								  nmismatches_list,/*amb_nmismatches_right*/NULL,
								  /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								  /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	    } else {
	      bestj = Intlist_head(splicesites_i);
	      bestleft = splicesites[bestj] - endlength;
	      if ((donor = Substring_new_donor(/*splicesites_i*/bestj,/*joffset*/0,
					       querylength-Substring_chimera_pos(acceptor),nmismatches_shortend,
					       /*prob*/2.0,/*left*/bestleft,query_compress_rev,
					       querylength,/*plusp*/false,genestrand,/*sensep*/false,
					       Substring_chrnum(acceptor),Substring_chroffset(acceptor),
					       Substring_chrhigh(acceptor),Substring_chrlength(acceptor))) != NULL) {
		debug4h(printf("End 8: short-overlap antiacceptor_minus: Successful splice from antiacceptor #%d to antidonor #%d\n",
			       Substring_splicesites_i(acceptor),Substring_splicesites_i(donor)));
		hits = List_push(hits,(void *) Stage3end_new_splice(&(*found_score),nmismatches_shortend,nmismatches,
								    donor,acceptor,/*distance*/origleft-bestleft,
								    /*shortdistancep*/true,localsplicing_penalty,querylength,
								    /*amb_nmatches*/0,/*ambi_left*/NULL,/*ambi_right*/NULL,
								    /*amb_nmismatches_left*/NULL,/*amb_nmismatches_right*/NULL,
								    /*copy_donor_p*/false,/*copy_acceptor_p*/true,first_read_p,
								    /*sensedir*/SENSE_ANTI,/*sarrayp*/false));
	      }
	    }
	    Intlist_free(&nmismatches_list);
	    Intlist_free(&splicesites_i);
	  }
	}
      }
    }

  }
  debug(printf("Ending find_splicepairs_shortend\n"));

  return hits;
}



static void
find_12mer_bounds (int *firstbound, int *lastbound, bool *omitted, int query_lastpos) {
  int nconsecutive;
  int querypos;

  querypos = 0;
  nconsecutive = 0;
  while (nconsecutive < index1interval && querypos <= query_lastpos) {
    if (omitted[querypos]) {
      nconsecutive = 0;
    } else {
      nconsecutive++;
    }
    querypos++;
  }
  if (nconsecutive < index1interval) {
    *firstbound = 0;
    /* *lastbound = query_lastpos; */
    debug(printf("nconsecutive from left < 3, so setting firstbound 0\n"));
    /* return false; */
  } else {
    *firstbound = querypos + (index1interval-1) + index1part; /* From trial-and-error, this is the correct value */
    debug(printf("Assigning firstbound to be querypos %d + (%d - 1) + index1part = %d\n",
		 querypos,index1interval,*firstbound));
  }

  querypos = query_lastpos;
  nconsecutive = 0;
  while (nconsecutive < index1interval && querypos >= 0) {
    if (omitted[querypos]) {
      nconsecutive = 0;
    } else {
      nconsecutive++;
    }
    querypos--;
  }

  if (nconsecutive < index1interval) {
    /* *firstbound = 0 + index1part; */
    *lastbound = query_lastpos;
    debug(printf("nconsecutive from right < %d, so setting lastbound %d\n",
		 index1interval,query_lastpos));
    /* return false; */
  } else {
    *lastbound = querypos-1; /* From trial-and-error, this is the correct value */
    debug(printf("Assigning lastbound to be querypos %d - 1 = %d\n",
		 querypos,*lastbound));
  }

  return;

  /* The following extensions were causing misses on test set */
#if 0
  if (*firstbound > query_lastpos) {
    debug(printf("firstbound %d > query_lastpos %d, so setting firstbound 0 and lastbound %d\n",
		 *firstbound,query_lastpos,query_lastpos));
    *firstbound = 0;
    *lastbound = query_lastpos;
    return false;
#if 0
  } else if (*firstbound - index1part > *lastbound) {
    *firstbound = 0;
    *lastbound = query_lastpos;
    return false;
#endif
  } else if (*lastbound <= index1part) {
    debug(printf("lastbound %d <= %d, so setting firstbound 0 and lastbound %d\n",
		 *lastbound,index1part,query_lastpos));
    *firstbound = 0;
    *lastbound = query_lastpos;
    return false;
  } else {
    return true;
  }
#endif
}



static Floors_T
compute_floors (bool *any_omitted_p, bool *alloc_floors_p, Floors_T *floors_array,
		T this, int querylength, int query_lastpos, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev,
		int indexdb_size_threshold, int max_end_insertions,
		bool omit_frequent_p, bool omit_repetitive_p, bool keep_floors_p) {
  Floors_T floors;
  bool all_omitted_p;

  if (this->all_positions_fetched_p == true) {
    omit_oligos_clear(this,query_lastpos);
  } else {
    fetch_positions_for_all_12mers(this,indexdb_fwd,indexdb_rev,query_lastpos);
  }

  debug(printf("Omitting frequent/repetitive oligos\n"));
  omit_oligos(&all_omitted_p,&(*any_omitted_p),this,query_lastpos,indexdb_size_threshold,
	      omit_frequent_p,omit_repetitive_p);

  if (all_omitted_p == true) {
    debug(printf("Aborting because all oligos are omitted\n"));
    *alloc_floors_p = false;
    return (Floors_T) NULL;
  } else if (*any_omitted_p) {
    floors = Floors_new_omitted(querylength,max_end_insertions,this->omitted);
    *alloc_floors_p = true;
  } else if (keep_floors_p == false) {
    floors = Floors_new_standard(querylength,max_end_insertions,keep_floors_p);
    *alloc_floors_p = true;
  } else {
    if (floors_array[querylength] == NULL) {
      floors_array[querylength] = Floors_new_standard(querylength,max_end_insertions,keep_floors_p);
    }
    floors = floors_array[querylength];
    *alloc_floors_p = false;
  }

  return floors;
}


static void
complete_set_mm_indels (int *found_score, bool *segments_computed_p,
			int *opt_level, int *done_level, int user_maxlevel,
			bool revise_levels_p, int *nhits, List_T *subs, List_T *indels, T this,
			Compress_T query_compress_fwd, Compress_T query_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			char *queryuc_ptr, char *queryrc,
#endif
			int querylength, int query_lastpos, Floors_T floors,
			int subopt_levels, int indel_penalty_middle, int indel_penalty_end,
			bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
			int fast_level, int genestrand) {
  int firstbound, lastbound;
  int max_mismatches_allowed;
#if 0
  int indel_level;
#endif

  debug(printf("Starting complete_set_mm_indels\n"));
  this->plus_segments = NULL;
  this->minus_segments = NULL;

  /* 4 and 5. Mismatches and indels via complete set.  Requires compress and
     all positions fetched.  Omits oligos and creates segments for some diagonals. */

#if 0
  if (find_12mer_bounds(&firstbound,&lastbound,this->omitted,query_lastpos) == false) {
    debug(printf("Cannot find 12_mer bounds\n"));
    /* This was allowing end indels to be missed */
    /* allow_end_indels_p = false; */
  } else {
    debug(printf("Found firstbound %d and lastbound %d\n",firstbound,lastbound));
  }
#else
  find_12mer_bounds(&firstbound,&lastbound,this->omitted,query_lastpos);
#endif

  if (*done_level < indel_penalty_end) {
    /* Prevents accumulation of segments for indels */
    allow_end_indels_p = false;
  }

  /* 4. Complete set mismatches */
  /* Done as a single batch */
  max_mismatches_allowed = (*done_level <= fast_level) ? -1 : *done_level;
  debug(printf("*** Stage 5.  Complete set mismatches up to %d (done_level %d, fast_level %d) ***\n",
	       max_mismatches_allowed,*done_level,fast_level));

  if (max_mismatches_allowed >= 0) {
    this->plus_segments = identify_all_segments(&this->plus_nsegments,&this->plus_spliceable,&this->plus_nspliceable,
						this->plus_positions,this->plus_npositions,
						this->omitted,querylength,query_lastpos,floors,/*plusp*/true);
    this->minus_segments = identify_all_segments(&this->minus_nsegments,&this->minus_spliceable,&this->minus_nspliceable,
						 this->minus_positions,this->minus_npositions,
						 this->omitted,querylength,query_lastpos,floors,/*plusp*/false);

    *subs = find_complete_mm(&(*found_score),&(*nhits),*subs,this->plus_segments,this->plus_nsegments,
			     querylength,/*queryptr:queryuc_ptr,*/
			     /*query_compress*/query_compress_fwd,
			     max_mismatches_allowed,/*plusp*/true,genestrand);

    *subs = find_complete_mm(&(*found_score),&(*nhits),*subs,this->minus_segments,this->minus_nsegments,
			     querylength,/*queryptr:queryrc,*/
			     /*query_compress*/query_compress_rev,
			     max_mismatches_allowed,/*plusp*/false,genestrand);
    *segments_computed_p = true;

    debug(printf("5> found_score = %d, opt_level %d, done_level %d\n",*found_score,*opt_level,*done_level));
    debug(printf("plus_nsegments = %d, minus_nsegments = %d\n",this->plus_nsegments,this->minus_nsegments));
  }

  if (revise_levels_p == true) {
    *opt_level = (*found_score < *opt_level) ? *found_score : *opt_level;
    if ((*done_level = *opt_level + subopt_levels) > user_maxlevel) {
      *done_level = user_maxlevel;
    }
  }

  if (*done_level >= indel_penalty_middle || *done_level >= indel_penalty_end) {
    /* 6. Indels */
    /* Need to reverse, because middle indelsplicing procedure depends on ascending diagonal order */

    if (*segments_computed_p == false) {
      this->plus_segments = identify_all_segments(&this->plus_nsegments,&this->plus_spliceable,&this->plus_nspliceable,
						  this->plus_positions,this->plus_npositions,
						  this->omitted,querylength,query_lastpos,floors,/*plusp*/true);
      this->minus_segments = identify_all_segments(&this->minus_nsegments,&this->minus_spliceable,&this->minus_nspliceable,
						   this->minus_positions,this->minus_npositions,
						   this->omitted,querylength,query_lastpos,floors,/*plusp*/false);
      *segments_computed_p = true;
    }

#if 0
    /* Done iteratively */
    indel_level = indel_penalty;
    while (indel_level <= *done_level) {
      debug(printf("*** Stage 6.  Middle indels with %d-%d mismatches allowed\n",indel_level,indel_penalty));
      *indels = find_middle_indels(&(*found_score),&(*nhits),*indels,
				   this->plus_spliceable,this->plus_nspliceable,
				   this->minus_spliceable,this->minus_nspliceable,
#ifdef DEBUG2
				   queryuc_ptr,queryrc,
#endif
				   floors,querylength,query_lastpos,query_compress_fwd,query_compress_rev,
				   min_indel_end_matches,indel_penalty,
				   /*indel_mismatches_allowed*/indel_level - indel_penalty,genestrand);

      if (allow_end_indels_p == true) {
	debug(printf("*** Stage 6.  End indels with %d-%d mismatches allowed\n",indel_level,indel_penalty));
	*indels = find_end_indels(&(*found_score),&(*nhits),*indels,this->plus_segments,this->minus_segments,
				  this->plus_nsegments,this->minus_nsegments,
#ifdef DEBUG2E
				  queryuc_ptr,queryrc,
#endif
				  querylength,firstbound,lastbound,query_compress_fwd,query_compress_rev,
				  max_end_insertions,max_end_deletions,min_indel_end_matches,indel_penalty,
				  /*indel_mismatches_allowed*/indel_level - indel_penalty,genestrand);
      }
      if (revise_levels_p == true) {
	*opt_level = (*found_score < *opt_level) ? *found_score : *opt_level;
	if ((*done_level = *opt_level + subopt_levels) > user_maxlevel) {
	  *done_level = user_maxlevel;
	}
      }
      indel_level++;
      debug(printf("6> found_score = %d, opt_level %d, done_level %d\n",*found_score,*opt_level,*done_level));
    }
#else
    /* Do all in one sweep */
    debug(printf("*** Stage 6A.  Middle indels with %d-%d mismatches allowed\n",*done_level,indel_penalty_middle));
    *indels = find_middle_indels(&(*found_score),&(*nhits),*indels,
				 this->plus_spliceable,this->plus_nspliceable,
				 this->minus_spliceable,this->minus_nspliceable,
#ifdef DEBUG2
				 queryuc_ptr,queryrc,
#endif
				 floors,querylength,query_lastpos,query_compress_fwd,query_compress_rev,
				 min_indel_end_matches,indel_penalty_middle,
				 /*indel_mismatches_allowed*/(*done_level) - indel_penalty_middle,genestrand);
    if (revise_levels_p == true) {
      *opt_level = (*found_score < *opt_level) ? *found_score : *opt_level;
      if ((*done_level = *opt_level + subopt_levels) > user_maxlevel) {
	*done_level = user_maxlevel;
      }
    }
    debug(printf("6A> found_score = %d, opt_level %d, done_level %d\n",*found_score,*opt_level,*done_level));

    if (allow_end_indels_p == true) {
      debug(printf("*** Stage 6B.  End indels with %d-%d mismatches allowed\n",*done_level,indel_penalty_end));
      *indels = find_end_indels(&(*found_score),&(*nhits),*indels,this->plus_segments,this->minus_segments,
				this->plus_nsegments,this->minus_nsegments,
#ifdef DEBUG2E
				queryuc_ptr,queryrc,
#endif
				querylength,firstbound,lastbound,query_compress_fwd,query_compress_rev,
				max_end_insertions,max_end_deletions,min_indel_end_matches,indel_penalty_end,
				/*indel_mismatches_allowed*/(*done_level) - indel_penalty_end,genestrand);
      if (revise_levels_p == true) {
	*opt_level = (*found_score < *opt_level) ? *found_score : *opt_level;
	if ((*done_level = *opt_level + subopt_levels) > user_maxlevel) {
	  *done_level = user_maxlevel;
	}
      }
      debug(printf("6B> found_score = %d, opt_level %d, done_level %d\n",*found_score,*opt_level,*done_level));
    }
    /* Calling procedure will invoke Stage3_remove_duplicates */
#endif

  }

  debug(printf("Finished with complete_set_mm_indels\n"));

  return;
}



static List_T
complete_set_singlesplicing (int *found_score, List_T localsplicing, Floors_T floors, T this,
			     Compress_T query_compress_fwd, Compress_T query_compress_rev,
			     int querylength, int query_lastpos, int localsplicing_penalty,
			     int max_mismatches_allowed, bool first_read_p, int genestrand,
			     bool subs_or_indels_p) {
  List_T lowprob = NULL, p;
  Stage3end_T hit;
  int worst_nmatches;

  debug(printf("Starting complete_set_singlesplicing with %d mismatches allowed\n",max_mismatches_allowed));

  if (floors == NULL) {
    return (List_T) NULL;
  }

  localsplicing = find_singlesplices_plus(&(*found_score),localsplicing,&lowprob,
					  this->plus_spliceable,this->plus_nspliceable,
					  floors,querylength,query_lastpos,
					  /*query_compress*/query_compress_fwd,
					  /*splicing_penalty*/localsplicing_penalty,
					  max_mismatches_allowed,first_read_p,genestrand,
					  subs_or_indels_p);

  localsplicing = find_singlesplices_minus(&(*found_score),localsplicing,&lowprob,
					   this->minus_spliceable,this->minus_nspliceable,
					   floors,querylength,query_lastpos,
					   /*query_compress*/query_compress_rev,
					   /*splicing_penalty*/localsplicing_penalty,
					   max_mismatches_allowed,first_read_p,genestrand,
					   subs_or_indels_p);

  debug(printf("Finished with complete_set_singlesplicing\n"));

  if (localsplicing == NULL) {
    return lowprob;
  } else {
    worst_nmatches = querylength;
    for (p = localsplicing; p != NULL; p = List_next(p)) {
      hit = (Stage3end_T) List_head(p);
      if (Stage3end_nmatches(hit) < worst_nmatches) {
	worst_nmatches = Stage3end_nmatches(hit);
      }
    }
    for (p = lowprob; p != NULL; p = List_next(p)) {
      hit = (Stage3end_T) List_head(p);
      if (Stage3end_nmatches(hit) < worst_nmatches) {
	/* Dominated by both nmatches and probability */
	Stage3end_free(&hit);
      } else {
	/* Has worse probability but more matches, so keep */
	localsplicing = List_push(localsplicing,(void *) hit);
      }
    }
    List_free(&lowprob);

    return localsplicing;
  }
}


static List_T
complete_set_doublesplicing (int *found_score, List_T localsplicing, Floors_T floors, T this,
			     Compress_T query_compress_fwd, Compress_T query_compress_rev,
			     char *queryuc_ptr, char *queryrc, int querylength, int query_lastpos,
			     int localsplicing_penalty,
			     int min_shortend, int max_mismatches_allowed, bool pairedp,
			     bool first_read_p, int genestrand, bool subs_or_indels_p) {
  List_T lowprob = NULL, p;
  Stage3end_T hit;
  int worst_nmatches;
  
  debug(printf("Starting complete_set_doublesplicing with %d mismatches allowed\n",
	       max_mismatches_allowed));

  if (floors == NULL) {
    return (List_T) NULL;
  }

  localsplicing = find_doublesplices(&(*found_score),localsplicing,&lowprob,
				     this->plus_spliceable,this->plus_nspliceable,this->plus_segments,
				     /*queryptr*/queryuc_ptr,floors,querylength,query_lastpos,
				     /*query_compress*/query_compress_fwd,
				     /*max_distance*/shortsplicedist,/*splicing_penalty*/localsplicing_penalty,
				     min_shortend,max_mismatches_allowed,pairedp,first_read_p,
				     /*plusp*/true,genestrand,subs_or_indels_p);

  localsplicing = find_doublesplices(&(*found_score),localsplicing,&lowprob,
				     this->minus_spliceable,this->minus_nspliceable,this->minus_segments,
				     /*queryptr*/queryrc,floors,querylength,query_lastpos,
				     /*query_compress*/query_compress_rev,
				     /*max_distance*/shortsplicedist,/*splicing_penalty*/localsplicing_penalty,
				     min_shortend,max_mismatches_allowed,pairedp,first_read_p,
				     /*plusp*/false,genestrand,subs_or_indels_p);

  debug(printf("Finished with complete_set_doublesplicing\n"));

  if (localsplicing == NULL) {
    return lowprob;
  } else {
    worst_nmatches = querylength;
    for (p = localsplicing; p != NULL; p = List_next(p)) {
      hit = (Stage3end_T) List_head(p);
      if (Stage3end_nmatches(hit) < worst_nmatches) {
	worst_nmatches = Stage3end_nmatches(hit);
      }
    }
    for (p = lowprob; p != NULL; p = List_next(p)) {
      hit = (Stage3end_T) List_head(p);
      if (Stage3end_nmatches(hit) < worst_nmatches) {
	/* Dominated by both nmatches and probability */
	Stage3end_free(&hit);
      } else {
	/* Has worse probability but more matches, so keep */
	localsplicing = List_push(localsplicing,(void *) hit);
      }
    }
    List_free(&lowprob);

    return localsplicing;
  }
}

/* Simple table */
typedef struct History_T *History_T;
struct History_T {
  List_T keys;			/* List of Univinterval_T */
  List_T values;		/* List of List_T of Stage3end_T */
};

static void
History_free (History_T *old) {
  Univinterval_T interval;
  List_T hitlist, p;

  for (p = (*old)->keys; p != NULL; p = p->rest) {
    interval = (Univinterval_T) p->first;
    Univinterval_free(&interval);
  }
  List_free(&(*old)->keys);
  
  for (p = (*old)->values; p != NULL; p = p->rest) {
    hitlist = (List_T) p->first;
    Stage3end_list_free(&hitlist);
  }
  List_free(&(*old)->values);

  FREE(*old);
  return;
}

static History_T
History_new () {
  History_T new = (History_T) MALLOC(sizeof(*new));

  new->keys = (List_T) NULL;
  new->values = (List_T) NULL;
  return new;
}

static List_T
History_get (History_T this, Univinterval_T interval) {
  List_T p, q;

  for (p = this->keys, q = this->values; p != NULL; p = p->rest, q = q->rest) {
    if (Univinterval_equal(interval,(Univinterval_T) p->first) == true) {
      return (List_T) q->first;
    }
  }
  return (List_T) NULL;
}

static void
History_put (History_T this, Univinterval_T interval, List_T gmap_hits) {
  this->keys = List_push(this->keys,(void *) interval);
  this->values = List_push(this->values,(void *) gmap_hits);
  return;
}


#define add_bounded(x,plusterm,highbound) ((x + (plusterm) >= highbound) ? (highbound - 1) : x + (plusterm))
#define subtract_bounded(x,minusterm,lowbound) ((x < lowbound + (minusterm)) ? lowbound : x - (minusterm))


static List_T
run_gmap (bool *good_start_p, bool *good_end_p, History_T gmap_history,
	  List_T hits, char *queryuc_ptr, int querylength,
	  int sense_try, bool favor_right_p, int paired_favor_mode, int zero_offset,
	  Compress_T query_compress_fwd, Compress_T query_compress_rev,

	  Univcoord_T mappingstart, Univcoord_T mappingend,
	  Univcoord_T knownsplice_limit_low, Univcoord_T knownsplice_limit_high,
	  bool watsonp, int genestrand,
	  Chrnum_T chrnum, Univcoord_T chroffset, Univcoord_T chrhigh, Chrpos_T chrlength,

	  Oligoindex_T *oligoindices_major, int noligoindices_major,
	  Oligoindex_T *oligoindices_minor, int noligoindices_minor,
	  Pairpool_T pairpool, Diagpool_T diagpool,
	  Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
	  int user_maxlevel) {
  Stage3end_T hit;
#ifdef EXTRACT_GENOMICSEG
  char *genomicseg, *genomicseg_alloc;
#endif

  List_T stored_hits;
  List_T all_stage2results, p;
  int stage2_source, stage2_indexsize;
  Stage2_T stage2;

  Univinterval_T interval;
  List_T pairs;
  struct Pair_T *pairarray;
  Univcoord_T start, end;
  double min_splice_prob;
  int npairs, nsegments, nmismatches_whole, nindels, nintrons, nindelbreaks;
  int cdna_direction, sensedir;
  int matches, unknowns, mismatches, qopens, qindels, topens, tindels;
  int nmatches_posttrim, max_match_length, ambig_end_length_5, ambig_end_length_3;
  Splicetype_T ambig_splicetype_5, ambig_splicetype_3;
  int ncanonical, nsemicanonical, nnoncanonical;
  int maxintronlen_bound;

  debug13(printf("Running GMAP at mappingstart %lu + %d = mappingend %lu, watsonp %d, sense_try %d, querylength %d, limits %u..%u\n",
		 mappingstart-chroffset,mappingend-mappingstart,mappingend-chroffset,watsonp,sense_try,querylength,
		 (Chrpos_T) (knownsplice_limit_low-chroffset),(Chrpos_T) (knownsplice_limit_high-chroffset)));

  assert(mappingend > mappingstart);

  *good_start_p = *good_end_p = false;

  interval = Univinterval_new(mappingstart,mappingend,sense_try);
  debug13(printf("Checking history for interval at %u..%u (sense_try %d)\n",
		 mappingstart,mappingend,sense_try));
  if ((stored_hits = History_get(gmap_history,interval)) != NULL) {
    debug13(printf("Already ran these coordinates, and have results\n"));
    for (p = stored_hits; p != NULL; p = List_next(p)) {
      if ((hit = (Stage3end_T) List_head(p)) != NULL) {
	if (Stage3end_trim_left(hit) < GOOD_GMAP_END) {
	  *good_start_p = true;
	}
	if (Stage3end_trim_right(hit) < GOOD_GMAP_END) {
	  *good_end_p = true;
	}
	hits = List_push(hits,(void *) Stage3end_copy(hit));
      }
    }
    Univinterval_free(&interval);
    return hits;
  } else {
    debug13(printf("New coordinates\n"));
    /* stored_hits = (List_T) NULL; -- Already NULL */
  }

#ifdef EXTRACT_GENOMICSEG
  if (watsonp == true) {
    printf("Allocating %u bytes\n",genomiclength);
    genomicseg_alloc = (char *) CALLOC(genomiclength+MAX_INDEXSIZE+1,sizeof(char));
    genomicseg = &(genomicseg_alloc[MAX_INDEXSIZE]);
    Genome_fill_buffer_blocks(genomicstart-MAX_INDEXSIZE,genomiclength+MAX_INDEXSIZE,genomicseg_alloc);
  } else {
    printf("Allocating %u bytes\n",genomiclength);
    genomicseg_alloc = (char *) CALLOC(genomiclength+MAX_INDEXSIZE+1,sizeof(char));
    genomicseg = &(genomicseg_alloc[MAX_INDEXSIZE]);
    Genome_fill_buffer_blocks(genomicstart,genomiclength+MAX_INDEXSIZE,genomicseg_alloc);
    make_complement_inplace(genomicseg_alloc,genomiclength+MAX_INDEXSIZE);
  }
#endif

  if (chroffset + chrlength < chrhigh) {
    debug13(printf("Chromosome is circular because chroffset %u + chrlength %u < chrhigh %u\n",
		   chroffset,chrlength,chrhigh));
    maxintronlen_bound = 0;
  } else {
    maxintronlen_bound = shortsplicedist;
  }

  /* Note: Use nmatches post-trim to decide if the alignment is high
     quality or worth keeping.  But if so, then use nmatches_pretrim
     for ranking and scoring purposes. */

  /* use_shifted_canonical_p == true can be slow and can give wrong answers */
  all_stage2results = Stage2_compute(&stage2_source,&stage2_indexsize,
				     /*queryseq_ptr*/queryuc_ptr,queryuc_ptr,querylength,/*query_offset*/0,
				   
				     /*chrstart*/mappingstart-chroffset,/*chrend*/mappingend-chroffset,
				     chroffset,chrhigh,/*plusp*/watsonp,genestrand,

				     oligoindices_major,noligoindices_major,/*proceed_pctcoverage*/0.5,
				     pairpool,diagpool,sufflookback,nsufflookback,
				     maxintronlen_bound,/*localp*/true,
				     /*skip_repetitive_p*/true,favor_right_p,/*max_nalignments*/MAX_NALIGNMENTS,
				     /*debug_graphic_p*/false,/*diagnosticp*/false,
				     /*worker_stopwatch*/NULL,/*diag_debug*/false);
  debug13(printf("Got %d stage2 results\n",List_length(all_stage2results)));

  if (all_stage2results == NULL) {
    stored_hits = List_push(stored_hits,(void *) NULL);
  }

  for (p = all_stage2results; p != NULL; p = List_next(p)) {
    stage2 = (Stage2_T) List_head(p);
    if ((pairarray = Stage3_compute(&pairs,&npairs,&cdna_direction,&sensedir,
				    &matches,&nmatches_posttrim,&max_match_length,
				    &ambig_end_length_5,&ambig_end_length_3,
				    &ambig_splicetype_5,&ambig_splicetype_3,
				    &unknowns,&mismatches,&qopens,&qindels,&topens,&tindels,
				    &ncanonical,&nsemicanonical,&nnoncanonical,&min_splice_prob,stage2,
#ifdef END_KNOWNSPLICING_SHORTCUT
				    cutoff_level,/*queryptr*/watsonp ? queryuc_ptr : queryrc,
				    watsonp ? query_compress_fwd : query_compress_rev,
#endif
				    /*queryseq_ptr*/queryuc_ptr,queryuc_ptr,querylength,/*skiplength*/0,
#ifdef EXTRACT_GENOMICSEG
				    /*query_subseq_offset*/0,
#else
				    /*query_subseq_offset*/0,
#endif
				    chrnum,chroffset,chrhigh,
				    knownsplice_limit_low,knownsplice_limit_high,watsonp,genestrand,
				    /*jump_late_p*/watsonp ? false : true,
				    maxpeelback,maxpeelback_distalmedial,nullgap,
				    extramaterial_end,extramaterial_paired,
				    extraband_single,extraband_end,extraband_paired,
				    minendexon,pairpool,dynprogL,dynprogM,dynprogR,ngap,
				    /*diagnosticp*/false,/*checkp*/false,
				    /*do_final_p*/true,sense_try,/*sense_filter*/0,
				    oligoindices_minor,noligoindices_minor,diagpool,
				    sufflookback,nsufflookback,maxintronlen_bound,
				    /*close_indels_mode*/+1,paired_favor_mode,zero_offset)) == NULL) {
      debug13(printf("stage3 is NULL\n"));
      stored_hits = List_push(stored_hits,(void *) NULL);

    } else {
      debug13(printf("stage3 is not NULL\n"));

      debug13a(Pair_dump_array(pairarray,npairs,true));

      if (0 && Stage3_short_alignment_p(pairarray,npairs,querylength) == true) {
	/* Very bad alignment */
	debug13(printf("Very bad alignment\n"));
	stored_hits = List_push(stored_hits,(void *) NULL);
	FREE_OUT(pairarray);

      } else {
#if 0
	Pair_print_gsnap(stdout,pairarray,npairs,invertedp,chrnum,chroffset,chrhigh,
			 querylength,watsonp,cdna_direction,chromosome_iit);
#endif

	nsegments = Pair_gsnap_nsegments(&nmismatches_whole,&nindels,&nintrons,&nindelbreaks,
					 pairarray,npairs);
	if (watsonp == true) {
	  start = subtract_bounded(chroffset + Pair_genomepos(&(pairarray[0])),
				   /*minusterm*/Pair_querypos(&(pairarray[0])),chroffset);
	  end = add_bounded(chroffset + Pair_genomepos(&(pairarray[npairs-1])),
			    /*plusterm*/querylength - 1 - Pair_querypos(&(pairarray[npairs-1])),chrhigh);
	  if ((hit = Stage3end_new_gmap(nmismatches_whole,nmatches_posttrim,max_match_length,
					ambig_end_length_5,ambig_end_length_3,
					ambig_splicetype_5,ambig_splicetype_3,min_splice_prob,
					pairarray,npairs,nsegments,nintrons,nindelbreaks,
					/*left*/start,/*genomiclength*/end - start + 1,
					/*plusp*/watsonp,genestrand,querylength,chrnum,chroffset,chrhigh,chrlength,
					cdna_direction,sensedir)) == NULL) {
	    debug13(printf("Stage3end_new_gmap returns NULL\n"));
	    stored_hits = List_push(stored_hits,(void *) NULL);
	    FREE_OUT(pairarray);

#if 0
	  } else if (Stage3end_bad_stretch_p(hit,query_compress_fwd,query_compress_rev) == true) {
	    debug13(printf("Stage3end_new_gmap has a bad stretch\n"));
	    Stage3end_free(&hit);
	    stored_hits = List_push(stored_hits,(void *) NULL);
	    /* FREE_OUT(pairarray); */
#endif
	    
	  } else {
	    if (Stage3end_trim_left(hit) < GOOD_GMAP_END) {
	      *good_start_p = true;
	    }
	    if (Stage3end_trim_right(hit) < GOOD_GMAP_END) {
	      *good_end_p = true;
	    }
	    debug13(printf("Trim at start: %d, trim at end: %d\n",
			   Stage3end_trim_left(hit),Stage3end_trim_right(hit)));
	    if (terminal_threshold > user_maxlevel &&
		(Stage3end_trim_left_raw(hit) >= GOOD_GMAP_END || Stage3end_trim_right_raw(hit) >= GOOD_GMAP_END)) {
	      debug13(printf("terminal_threshold %d > user_maxlevel %d, so freeing this GMAP hit\n",
			     terminal_threshold,user_maxlevel));
	      stored_hits = List_push(stored_hits,(void *) NULL);
	      Stage3end_free(&hit);
	    } else {
	      stored_hits = List_push(stored_hits,(void *) Stage3end_copy(hit));
	      hits = List_push(hits,(void *) hit);
	    }
	  }
	} else {
	  start = add_bounded(chroffset + Pair_genomepos(&(pairarray[0])),
			      /*plusterm*/Pair_querypos(&(pairarray[0])),chrhigh);
	  end = subtract_bounded(chroffset + Pair_genomepos(&(pairarray[npairs-1])),
				 /*minusterm*/querylength - 1 - Pair_querypos(&(pairarray[npairs-1])),chroffset);
	  if ((hit = Stage3end_new_gmap(nmismatches_whole,nmatches_posttrim,max_match_length,
					ambig_end_length_5,ambig_end_length_3,
					ambig_splicetype_5,ambig_splicetype_3,min_splice_prob,
					pairarray,npairs,nsegments,nintrons,nindelbreaks,
					/*left*/end,/*genomiclength*/start - end + 1,
					/*plusp*/watsonp,genestrand,querylength,chrnum,chroffset,chrhigh,chrlength,
					cdna_direction,sensedir)) == NULL) {
	    debug13(printf("Stage3end_new_gmap returns NULL\n"));
	    stored_hits = List_push(stored_hits,(void *) NULL);
	    FREE_OUT(pairarray);

#if 0
	  } else if (Stage3end_bad_stretch_p(hit,query_compress_fwd,query_compress_rev) == true) {
	    debug13(printf("Stage3end_new_gmap has a bad stretch\n"));
	    stored_hits = List_push(stored_hits,(void *) NULL);
	    Stage3end_free(&hit);
	    /* FREE_OUT(pairarray); */
#endif

	  } else {
	    if (Stage3end_trim_right(hit) < GOOD_GMAP_END) {
	      *good_start_p = true;
	    }
	    if (Stage3end_trim_left(hit) < GOOD_GMAP_END) {
	      *good_end_p = true;
	    }
	    debug13(printf("Trim at start: %d, trim at end: %d (raw %d and %d)\n",
			   Stage3end_trim_right(hit),Stage3end_trim_left(hit),
			   Stage3end_trim_right_raw(hit),Stage3end_trim_left_raw(hit)));
	    if (terminal_threshold > user_maxlevel &&
		(Stage3end_trim_left_raw(hit) >= GOOD_GMAP_END || Stage3end_trim_right_raw(hit) >= GOOD_GMAP_END)) {
	      debug13(printf("terminal_threshold %d > user_maxlevel %d, so freeing this GMAP hit\n",
			     terminal_threshold,user_maxlevel));
	      stored_hits = List_push(stored_hits,(void *) NULL);
	      Stage3end_free(&hit);
	    } else {
	      stored_hits = List_push(stored_hits,(void *) Stage3end_copy(hit));
	      hits = List_push(hits,(void *) hit);
	    }
	  }
	}
	/* Don't free pairarray */
      }
    }

    Stage2_free(&stage2);
  }
  List_free(&all_stage2results);


#ifdef EXTRACT_GENOMICSEG
  FREE(genomicseg_alloc);
#endif

  debug13(printf(" => Got good_start_p %d, good_end_p %d\n",*good_start_p,*good_end_p));
  debug13(printf("Storing history for interval at %u..%u (sense_try %d)\n",
		 mappingstart,mappingend,sense_try));
  History_put(gmap_history,interval,stored_hits);

  return hits;
}


static List_T
align_single_hit_with_gmap (History_T gmap_history, Stage3end_T hit,
			    bool extend_left_p, bool extend_right_p,
			    char *queryuc_ptr, int querylength, int query_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
			    char *queryrc, bool invertedp,
#endif
			    Compress_T query_compress_fwd, Compress_T query_compress_rev,
			    struct Segment_T *plus_segments, int plus_nsegments,
			    struct Segment_T *minus_segments, int minus_nsegments,
			    Oligoindex_T *oligoindices_major, int noligoindices_major,
			    Oligoindex_T *oligoindices_minor, int noligoindices_minor,
			    Pairpool_T pairpool, Diagpool_T diagpool,
			    Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
			    int user_maxlevel, int genestrand) {
  List_T hits = NULL;

  Univcoord_T segmentstart, segmentend;
  Univcoord_T mappingstart, mappingend, chroffset, chrhigh, mappingpos;
  Univcoord_T origlow, orighigh;
  Univcoord_T close_mappingstart_greedy, close_mappingend_greedy,
    close_mappingstart_last, close_mappingend_last;
  Univcoord_T middle_mappingstart_greedy, middle_mappingend_greedy,
    middle_mappingstart_last, middle_mappingend_last;
  Univcoord_T knownsplice_limit_low, knownsplice_limit_high;
  Univcoord_T close_knownsplice_limit_low, close_knownsplice_limit_high;
  Chrpos_T chrlength;
  Chrnum_T chrnum;
  bool close_mappingstart_p = false, close_mappingend_p = false;
  bool middle_mappingstart_p = false, middle_mappingend_p = false;
  bool fallback_mappingstart_p, fallback_mappingend_p;
  bool good_start_p, good_end_p, watsonp, favor_right_p;

  int starti, endi, i;


  if ((chrnum = Stage3end_chrnum(hit)) == 0) {
    return (List_T) NULL;
  } else {
    chroffset = Stage3end_chroffset(hit);
    chrhigh = Stage3end_chrhigh(hit);
    chrlength = Stage3end_chrlength(hit);
  }

  if ((watsonp = Stage3end_plusp(hit)) == true) {
    origlow = Stage3end_genomicstart(hit);
    orighigh = Stage3end_genomicend(hit);

    if (extend_left_p == true) {
      knownsplice_limit_low = subtract_bounded(origlow,shortsplicedist,chroffset);
#ifdef LONG_ENDSPLICES
      mappingstart = segmentstart = subtract_bounded(origlow,shortsplicedist,chroffset);
#else
      mappingstart = segmentstart = subtract_bounded(origlow,shortsplicedist_novelend,chroffset);
#endif

    } else {
      knownsplice_limit_low = mappingstart = segmentstart = origlow;
    }
    debug13(printf("Original bounds: knownsplice_limit_low %u, mappingstart %u\n",
		   knownsplice_limit_low - chroffset,mappingstart - chroffset));

    if (extend_right_p == true) {
      knownsplice_limit_high = add_bounded(orighigh,shortsplicedist,chrhigh);
#ifdef LONG_ENDSPLICES
      mappingend = segmentend =	add_bounded(orighigh,shortsplicedist,chrhigh);
#else
      mappingend = segmentend =	add_bounded(orighigh,shortsplicedist_novelend,chrhigh);
#endif
    } else {
      knownsplice_limit_high = mappingend = segmentend = orighigh;
    }
    debug13(printf("Original bounds: knownsplice_limit_high %u, mappingend %u\n",
		   knownsplice_limit_high - chroffset,mappingend - chroffset));

    debug13(printf("plus hit %u..%u (extend_left_p %d, extend_right_p %d) (sensedir %d) => segment bounds %u..%u\n",
		   Stage3end_genomicstart(hit) - chroffset,Stage3end_genomicend(hit) - chroffset,
		   extend_left_p,extend_right_p,Stage3end_sensedir(hit),segmentstart-chroffset,segmentend-chroffset));

    close_mappingstart_last = middle_mappingstart_last = origlow;
    close_mappingend_last = middle_mappingend_last = orighigh;
    close_mappingstart_greedy = middle_mappingstart_greedy = segmentstart;
    close_mappingend_greedy = middle_mappingend_greedy = segmentend;

    if (plus_nsegments > 0) {
      /* Use segments to bound */
      debug13(printf("Finding segments from segmentstart %u to segmentend %u (plus_nsegments %d)\n",
		     segmentstart - chroffset,segmentend - chroffset,plus_nsegments));
      starti = endi = -1;
      i = binary_search_segments(0,plus_nsegments-1,plus_segments,segmentend);
      while (i >= 0 && plus_segments[i].diagonal >= segmentend) {
	i--;
      }
      starti = i;
      while (i >= 0 && plus_segments[i].diagonal > segmentstart) {
	if (plus_segments[i].diagonal < (Univcoord_T) -1) {
	  endi = i;
	}
	i--;
      }
      if (extend_left_p == true && starti >= 0 && endi >= 0) {
	debug13(printf("starti = %d, endi = %d\n",starti,endi));
	assert(starti >= endi);
	for (i = starti; i >= endi; i--) {
	  debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			 (Chrpos_T) (plus_segments[i].diagonal - chroffset),plus_segments[i].diagonal,
			 plus_segments[i].querypos5,plus_segments[i].querypos3));
	  if (plus_segments[i].querypos5 >= STAGE2_MIN_OLIGO + index1interval) {
	    /* Case 3. Missing start of query, so there could be a middle splice */
	    debug13b(printf("  querypos5 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			    plus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = subtract_bounded(plus_segments[i].diagonal,querylength + shortsplicedist_novelend,chroffset)) > middle_mappingstart_greedy &&
		mappingpos < origlow) {
	      middle_mappingstart_greedy = mappingpos;
	      middle_mappingstart_p = true;
	      debug13(printf("  Redefining middle mappingstart greedy to %u\n",middle_mappingstart_greedy - chroffset));
	    }
#ifdef LONG_ENDSPLICES
	    if ((mappingpos = subtract_bounded(plus_segments[i].diagonal,querylength + shortsplicedist,chroffset)) < middle_mappingstart_last) {
	      /* Use < for NOT_GREEDY */
	      middle_mappingstart_last = mappingpos;
	      middle_mappingstart_p = true;
	      debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	    }
#else
	    if (mappingpos < middle_mappingstart_last) {
	      /* Use < for NOT_GREEDY */
	      middle_mappingstart_last = mappingpos;
	      middle_mappingstart_p = true;
	      debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	    }
#endif

	  } else {
	    debug13b(printf("  querypos5 %d < %d + %d, so using this diagonal\n",
			    plus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = subtract_bounded(plus_segments[i].diagonal,querylength,chroffset)) > close_mappingstart_greedy &&
		mappingpos < origlow) {
	      close_mappingstart_greedy = mappingpos;
	      close_mappingstart_p = true;
	      debug13(printf("  Redefining close mappingstart last to %u\n",close_mappingstart_greedy - chroffset));
	    }
	    if (mappingpos < close_mappingstart_last) {
	      /* Use < for NOT_GREEDY */
	      close_mappingstart_last = mappingpos;
	      close_mappingstart_p = true;
	      debug13(printf("  Redefining close mappingstart last to %u\n",close_mappingstart_last - chroffset));
	    }
	  }
	}

	if (close_mappingstart_p == true) {
	  close_knownsplice_limit_low = subtract_bounded(close_mappingstart_greedy,shortsplicedist,chroffset);
	} else if (middle_mappingstart_p == true) {
	  debug13(printf("Using middle mappingstart\n"));
	  close_knownsplice_limit_low = middle_mappingstart_greedy;
	  close_mappingstart_greedy = middle_mappingstart_greedy;
	  close_mappingstart_p = true;
	}
	if (middle_mappingstart_p == true && middle_mappingstart_last < close_mappingstart_greedy) {
	  knownsplice_limit_low = middle_mappingstart_last;
	  mappingstart = middle_mappingstart_last;
	} else if (close_mappingstart_p == true && close_mappingstart_last != close_mappingstart_greedy) {
	  knownsplice_limit_low = subtract_bounded(close_mappingstart_last,shortsplicedist,chroffset);
	  mappingstart = close_mappingstart_last;
	}
	if (close_mappingstart_p == false) {
	  fallback_mappingstart_p = false;
	} else if (mappingstart >= close_mappingstart_greedy) {
	  fallback_mappingstart_p = false;
	} else {
	  debug13(printf("Fallback mappingstart = %u\n",mappingstart - chroffset));
	  fallback_mappingstart_p = true;
	}
      }

      if (extend_right_p == true && starti >= 0 && endi >= 0) {
	debug13(printf("starti = %d, endi = %d\n",starti,endi));
	assert(starti >= endi);
	for (i = starti; i >= endi; i--) {
	  debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			 (Chrpos_T) (plus_segments[i].diagonal - chroffset),plus_segments[i].diagonal,
			 plus_segments[i].querypos5,plus_segments[i].querypos3));
	  if (query_lastpos - plus_segments[i].querypos3 >= STAGE2_MIN_OLIGO + index1interval) {
	    /* Case 1. Missing end of query, so there could be a middle splice */
	    debug13b(printf("  query_lastpos %d - querypos3 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			    query_lastpos,plus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = add_bounded(plus_segments[i].diagonal,shortsplicedist_novelend,chrhigh)) < middle_mappingend_greedy &&
		mappingpos > orighigh) {
	      middle_mappingend_greedy = mappingpos;
	      middle_mappingend_p = true;
	      debug13(printf("  Redefining middle mappingend greedy to %u\n",middle_mappingend_greedy - chroffset));
	    }
#ifdef LONG_ENDSPLICES
	    if ((mappingpos = add_bounded(plus_segments[i].diagonal,shortsplicedist,chrhigh)) > middle_mappingend_last) {
	      /* Use > for NOT_GREEDY */
	      middle_mappingend_last = mappingpos;
	      middle_mappingend_p = true;
	      debug13(printf("  Redefining middle mappingend last to %u\n",middle_mappingend_last - chroffset));
	    }
#else
	    if (mappingpos > middle_mappingend_last) {
	      /* Use > for NOT_GREEDY */
	      middle_mappingend_last = mappingpos;
	      middle_mappingend_p = true;
	      debug13(printf("  Redefining middle mappingend last to %u\n",middle_mappingend_last - chroffset));
	    }
#endif

	  } else {
	    debug13b(printf("  query_lastpos %d - querypos3 %d < %d + %d, so using this diagonal\n",
			    query_lastpos,plus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = plus_segments[i].diagonal) < close_mappingend_greedy &&
		mappingpos > orighigh) {
	      close_mappingend_greedy = mappingpos;
	      close_mappingend_p = true;
	      debug13(printf("  Redefining close mappingend greedy to %u\n",close_mappingend_greedy - chroffset));
	    }
	    if (mappingpos > close_mappingend_last) {
	      /* Use > for NOT_GREEDY */
	      close_mappingend_last = mappingpos;
	      close_mappingend_p = true;
	      debug13(printf("  Redefining close mappingend last to %u\n",close_mappingend_last - chroffset));
	    }
	  }
	}

	if (close_mappingend_p == true) {
	  close_knownsplice_limit_high = add_bounded(close_mappingend_greedy,shortsplicedist,chrhigh);
	} else if (middle_mappingend_p == true) {
	  close_knownsplice_limit_high = middle_mappingend_greedy;
	  close_mappingend_greedy = middle_mappingend_greedy;
	  close_mappingend_p = true;
	  debug13(printf("Using middle mappingend => close_mappingend %u\n",close_mappingend_greedy));
	}
	if (middle_mappingend_p == true && middle_mappingend_last > close_mappingend_greedy) {
	  knownsplice_limit_high = middle_mappingend_last;
	  mappingend = middle_mappingend_last;
	} else if (close_mappingend_p == true && close_mappingend_last != close_mappingend_greedy) {
	  knownsplice_limit_high = add_bounded(close_mappingend_last,shortsplicedist,chrhigh);
	  mappingend = close_mappingend_last;
	}
	if (close_mappingend_p == false) {
	  fallback_mappingend_p = false;
	} else if (mappingend <= close_mappingend_greedy) {
	  fallback_mappingend_p = false;
	} else {
	  debug13(printf("Fallback mappingend = %u\n",mappingend - chroffset));
	  fallback_mappingend_p = true;
	}
      }
    }

    favor_right_p = false;

  } else {
    origlow = Stage3end_genomicend(hit);
    orighigh = Stage3end_genomicstart(hit);
    
    if (extend_right_p == true) {
      knownsplice_limit_low = subtract_bounded(origlow,shortsplicedist,chroffset);
#ifdef LONG_ENDSPLICES
      mappingstart = segmentstart = subtract_bounded(origlow,shortsplicedist,chroffset);
#else
      mappingstart = segmentstart = subtract_bounded(origlow,shortsplicedist_novelend,chroffset);
#endif
    } else {
      knownsplice_limit_low = mappingstart = segmentstart = origlow;
    }
    debug13(printf("Original bounds: knownsplice_limit_low %u, mappingstart %u\n",
		   knownsplice_limit_low - chroffset,mappingstart - chroffset));

    if (extend_left_p == true) {
      knownsplice_limit_high = add_bounded(orighigh,shortsplicedist,chrhigh);
#ifdef LONG_ENDSPLICES
      mappingend = segmentend =	add_bounded(orighigh,shortsplicedist,chrhigh);
#else
      mappingend = segmentend =	add_bounded(orighigh,shortsplicedist_novelend,chrhigh);
#endif
    } else {
      knownsplice_limit_high = mappingend = segmentend = orighigh;
    }
    debug13(printf("Original bounds: knownsplice_limit_high %u, mappingend %u\n",
		   knownsplice_limit_high - chroffset,mappingend - chroffset));

    debug13(printf("minus hit %u..%u (extend_left_p %d, extend_right_p %d) (sensedir %d), => segmentbounds %u..%u\n",
		   Stage3end_genomicstart(hit) - chroffset,Stage3end_genomicend(hit) - chroffset,
		   extend_left_p,extend_right_p,Stage3end_sensedir(hit),segmentstart - chroffset,segmentend - chroffset));

    close_mappingstart_last = middle_mappingstart_last = origlow;
    close_mappingend_last = middle_mappingend_last = orighigh;
    close_mappingstart_greedy = middle_mappingstart_greedy = segmentstart;
    close_mappingend_greedy = middle_mappingend_greedy = segmentend;

    if (minus_nsegments > 0) {
      /* Use segments to bound */
      debug13(printf("Finding segments from segmentstart %u to segmentend %u, (minus_nsegments %d)\n",
		     segmentstart - chroffset,segmentend - chroffset,minus_nsegments));
      starti = endi = -1;
      i = binary_search_segments(0,minus_nsegments-1,minus_segments,segmentend);
      while (i >= 0 && minus_segments[i].diagonal >= segmentend) {
	i--;
      }
      starti = i;
      while (i >= 0 && minus_segments[i].diagonal > segmentstart) {
	if (minus_segments[i].diagonal < (Univcoord_T) -1) {
	  endi = i;
	}
	i--;
      }
      if (extend_right_p == true && starti >= 0 && endi >= 0) {
	debug13(printf("starti = %d, endi = %d\n",starti,endi));
	assert(starti >= endi);
	for (i = starti; i >= endi; i--) {
	  debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			 (Chrpos_T) (minus_segments[i].diagonal - chroffset),minus_segments[i].diagonal,
			 minus_segments[i].querypos5,minus_segments[i].querypos3));
	  if (query_lastpos - minus_segments[i].querypos3 >= STAGE2_MIN_OLIGO + index1interval) {
	    /* Case 2. Missing end of query, so there could be a middle splice */
	    debug13b(printf("  query_lastpos %d - querypos3 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			    query_lastpos,minus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = subtract_bounded(minus_segments[i].diagonal,querylength + shortsplicedist_novelend,chroffset)) > middle_mappingstart_greedy &&
		mappingpos < origlow) {
	      middle_mappingstart_greedy = mappingpos;
	      middle_mappingstart_p = true;
	      debug13(printf("  Redefining middle mappingstart greedy to %u\n",middle_mappingstart_greedy - chroffset));
	    }
#ifdef LONG_ENDSPLICES
	    if ((mappingpos = subtract_bounded(minus_segments[i].diagonal,querylength + shortsplicedist,chroffset)) < middle_mappingstart_last) {
	      /* Use < for NOT_GREEDY */
	      middle_mappingstart_last = mappingpos;
	      middle_mappingstart_p = true;
	      debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	    }
#else
	    if (mappingpos < middle_mappingstart_last) {
	      /* Use < for NOT_GREEDY */
	      middle_mappingstart_last = mappingpos;
	      middle_mappingstart_p = true;
	      debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	    }
#endif

	  } else {
	    debug13b(printf("  query_lastpos %d - querypos3 %d < %d + %d, so using this diagonal\n",
			    query_lastpos,minus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = subtract_bounded(minus_segments[i].diagonal,querylength,chroffset)) > close_mappingstart_greedy &&
		mappingpos < origlow) {
	      close_mappingstart_greedy = mappingpos;
	      close_mappingstart_p = true;
	      debug13(printf("  Redefining close mappingstart greedy to %u\n",close_mappingstart_greedy - chroffset));
	    }
	    if (mappingpos < close_mappingstart_last) {
	      /* Use < for NOT_GREEDY */
	      close_mappingstart_last = mappingpos;
	      close_mappingstart_p = true;
	      debug13(printf("  Redefining close mappingstart last to %u\n",close_mappingstart_last - chroffset));
	    }
	  }
	}

	if (close_mappingstart_p == true) {
	  close_knownsplice_limit_low = subtract_bounded(close_mappingstart_greedy,shortsplicedist,chroffset);
	} else if (middle_mappingstart_p == true) {
	  debug13(printf("Using middle mappingstart\n"));
	  close_knownsplice_limit_low = middle_mappingstart_greedy;
	  close_mappingstart_greedy = middle_mappingstart_greedy;
	  close_mappingstart_p = true;
	}
	if (middle_mappingstart_p == true && middle_mappingstart_last < close_mappingstart_greedy) {
	  knownsplice_limit_low = middle_mappingstart_last;
	  mappingstart = middle_mappingstart_last;
	} else if (close_mappingstart_p == true && close_mappingstart_last != close_mappingstart_greedy) {
	  knownsplice_limit_low = subtract_bounded(close_mappingstart_last,shortsplicedist,chroffset);
	  mappingstart = close_mappingstart_last;
	}
	if (close_mappingstart_p == false) {
	  fallback_mappingstart_p = false;
	} else if (mappingstart >= close_mappingstart_greedy) {
	  fallback_mappingstart_p = false;
	} else {
	  debug13(printf("Fallback mappingstart = %u\n",mappingstart - chroffset));
	  fallback_mappingstart_p = true;
	}
      }

      if (extend_left_p == true && starti >= 0 && endi >= 0) {
	debug13(printf("starti = %d, endi = %d\n",starti,endi));
	assert(starti >= endi);
	for (i = starti; i >= endi; i--) {
	  debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			 (Chrpos_T) (minus_segments[i].diagonal - chroffset),minus_segments[i].diagonal,
			 minus_segments[i].querypos5,minus_segments[i].querypos3));
	  if (minus_segments[i].querypos5 >= STAGE2_MIN_OLIGO + index1interval) {
	    /* Case 4. Missing start of query, so there could be a middle splice */
	    debug13b(printf("  querypos5 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			    minus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = add_bounded(minus_segments[i].diagonal,shortsplicedist_novelend,chrhigh)) < middle_mappingend_greedy &&
		mappingpos > orighigh) {
	      middle_mappingend_greedy = mappingpos;
	      middle_mappingend_p = true;
	      debug13(printf("  Redefining middle mappingend greedy to %u\n",middle_mappingend_greedy - chroffset));
	    }
#ifdef LONG_ENDSPLICES
	    if ((mappingpos = add_bounded(minus_segments[i].diagonal,shortsplicedist,chrhigh)) > middle_mappingend_last) {
	      /* Use > for NOT_GREEDY */
	      middle_mappingend_last = mappingpos;
	      middle_mappingend_p = true;
	      debug13(printf("  Redefining middle mappingend last to %u\n",middle_mappingend_last - chroffset));
	    }
#else
	    if (mappingpos > middle_mappingend_last) {
	      /* Use > for NOT_GREEDY */
	      middle_mappingend_last = mappingpos;
	      middle_mappingend_p = true;
	      debug13(printf("  Redefining middle mappingend last to %u\n",middle_mappingend_last - chroffset));
	    }
#endif

	  } else {
	    debug13b(printf("  querypos5 %d < %d + %d, so using this diagonal\n",
			    minus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	    if ((mappingpos = minus_segments[i].diagonal) < close_mappingend_greedy &&
		mappingpos > orighigh) {
	      close_mappingend_greedy = mappingpos;
	      close_mappingend_p = true;
	      debug13(printf("  Redefining close mappingend greedy to %u\n",close_mappingend_greedy - chroffset));
	    }
	    if (mappingpos > close_mappingend_last) {
	      /* Use > for NOT_GREEDY */
	      close_mappingend_last = mappingpos;
	      close_mappingend_p = true;
	      debug13(printf("  Redefining close mappingend last to %u\n",close_mappingend_last - chroffset));
	    }
	  }
	}

	if (close_mappingend_p == true) {
	  close_knownsplice_limit_high = add_bounded(close_mappingend_greedy,shortsplicedist,chrhigh);
	} else if (middle_mappingend_p == true) {
	  debug13(printf("Using middle mappingend\n"));
	  close_knownsplice_limit_high = middle_mappingend_greedy;
	  close_mappingend_greedy = middle_mappingend_greedy;
	  close_mappingend_p = true;
	}
	if (middle_mappingend_p == true && middle_mappingend_last > close_mappingend_greedy) {
	  knownsplice_limit_high = middle_mappingend_last;
	  mappingend = middle_mappingend_last;
	} else if (close_mappingend_p == true && close_mappingstart_last != close_mappingstart_greedy) {
	  knownsplice_limit_high = add_bounded(close_mappingend_last,shortsplicedist,chrhigh);
	  mappingend = close_mappingend_last;
	}
	if (close_mappingend_p == false) {
	  fallback_mappingend_p = false;
	} else if (mappingend <= close_mappingend_greedy) {
	  fallback_mappingend_p = false;
	} else {
	  debug13(printf("Fallback mappingend = %u\n",mappingend - chroffset));
	  fallback_mappingend_p = true;
	}
      }
    }

    favor_right_p = true;
  }

  if (close_mappingstart_p == true && close_mappingend_p == true) {
    debug13(printf("Single hit: Running gmap with close mappingstart and close mappingend\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
		    /*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
		    query_compress_fwd,query_compress_rev,
		    close_mappingstart_greedy,close_mappingend_greedy,
		    close_knownsplice_limit_low,close_knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);

    if (good_start_p == true && good_end_p == true) {
      /* Success */
    } else if (gmap_rerun_p == false) {
      debug13(printf("Skipping re-run of gmap\n"));
    } else if (good_start_p == true) {
      if (fallback_mappingend_p == true) {
	debug13(printf("Single hit: Re-running gmap with close mappingstart only\n"));
	hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
			/*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
			query_compress_fwd,query_compress_rev,close_mappingstart_greedy,mappingend,
			close_knownsplice_limit_low,knownsplice_limit_high,
			watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
			oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
      }
    } else if (good_end_p == true) {
      if (fallback_mappingstart_p == true) {
	debug13(printf("Single hit: Re-running gmap with close mappingend only\n"));
	hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
			/*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
			query_compress_fwd,query_compress_rev,mappingstart,close_mappingend_greedy,
			knownsplice_limit_low,close_knownsplice_limit_high,
			watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
			oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
      }
    } else {
      if (fallback_mappingstart_p == true && fallback_mappingend_p == true) {
	debug13(printf("Single hit: Re-running gmap with far mappingstart and mappingend\n"));
	hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
			/*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
			query_compress_fwd,query_compress_rev,mappingstart,mappingend,
			knownsplice_limit_low,close_knownsplice_limit_high,
			watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
			oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
      }
    }

  } else if (close_mappingstart_p == true) {
    debug13(printf("Single hit: Running gmap with close mappingstart\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
		    /*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
		    query_compress_fwd,query_compress_rev,close_mappingstart_greedy,mappingend,
		    close_knownsplice_limit_low,knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
    if (good_start_p == true) {
      /* Success */
    } else if (gmap_rerun_p == false) {
      debug13(printf("Skipping re-run of gmap\n"));
    } else if (fallback_mappingstart_p == true) {
      debug13(printf("Single hit: Re-running gmap with far mappingstart\n"));
      hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
		      /*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
		      query_compress_fwd,query_compress_rev,mappingstart,mappingend,
		      knownsplice_limit_low,knownsplice_limit_high,
		      watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		      oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		      pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
    }

  } else if (close_mappingend_p == true) {
    debug13(printf("Single hit: Running gmap with close mappingend\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
		    /*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
		    query_compress_fwd,query_compress_rev,mappingstart,close_mappingend_greedy,
		    knownsplice_limit_low,close_knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
    if (good_end_p == true) {
      /* Success */
    } else if (gmap_rerun_p == false) {
      debug13(printf("Skipping re-run of gmap\n"));
    } else if (fallback_mappingend_p == true) {
      debug13(printf("Single hit: Re-running gmap with far mappingend\n"));
      hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
		      /*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
		      query_compress_fwd,query_compress_rev,mappingstart,mappingend,
		      knownsplice_limit_low,knownsplice_limit_high,
		      watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		      oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		      pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
    }

  } else {
    debug13(printf("Single hit: Running gmap with far mappingstart and mappingend\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,hits,queryuc_ptr,querylength,
		    /*sense_try*/0,favor_right_p,/*paired_favor_mode*/0,/*zero_offset*/0,
		    query_compress_fwd,query_compress_rev,mappingstart,mappingend,
		    knownsplice_limit_low,knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
  }

  return hits;
}



static List_T
align_singleend_with_gmap (History_T gmap_history, List_T result, T this,
			   Compress_T query_compress_fwd, Compress_T query_compress_rev,
			   char *queryuc_ptr, int querylength, int query_lastpos,
			   Oligoindex_T *oligoindices_major, int noligoindices_major,
			   Oligoindex_T *oligoindices_minor, int noligoindices_minor,
			   Pairpool_T pairpool, Diagpool_T diagpool,
			   Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
			   int user_maxlevel, int cutoff_level) {
  List_T new_result = NULL, gmap_hits = NULL;
  Stage3end_T hit, gmap;
  List_T p, a;
  int genestrand;
  int missing_hit, missing_gmap;
  int i;
  bool gmap_better_p;


  debug13(printf("Sorting hits by nmatches\n"));
  result = Stage3end_sort_bymatches(result);

  for (p = result, i = 0; p != NULL && i < max_gmap_improvement; p = p->rest, i++) {
    hit = (Stage3end_T) List_head(p);
    genestrand = Stage3end_genestrand(hit);

    debug13(printf("GMAP improvement: Entering align_singleend_with_gmap with hittype %s\n",
		   Stage3end_hittype_string(hit)));

    /* Was querylength5 - Stage3end_matches(hit5) > 5 */
    if (Stage3end_hittype(hit) == GMAP) {
      /* Skip */
      debug13(printf("Skipping hit of type GMAP\n"));
      new_result = List_push(new_result,(void *) hit);

    } else if (Stage3end_improved_by_gmap_p(hit) == true) {
      /* Skip */
      debug13(printf("Skipping hit already improved by GMAP\n"));
      new_result = List_push(new_result,(void *) hit);

#if 0
      /* Don't skip on final align_singleend_with_gmap */
    } else if (Stage3end_hittype(hit) == TERMINAL) {
      /* Skip */
      debug13(printf("Skipping hit of type TERMINAL\n"));
      new_result = List_push(new_result,(void *) hit);
#endif

    } else if (querylength - Stage3end_nmatches(hit) <= user_maxlevel) {
      /* Skip */
      debug13(printf("Skipping hit with nmismatches %d - %d <= user_maxlevel %d\n",
		     querylength,Stage3end_nmatches(hit),user_maxlevel));
      new_result = List_push(new_result,(void *) hit);

    } else if (Stage3end_terminal_trim(hit) <= GMAP_TERMINAL_TRIM
	       && Stage3end_contains_known_splicesite(hit) == false
	       ) {
      debug13(printf("Skipping good hit\n"));
      new_result = List_push(new_result,(void *) hit);

    } else {
      debug13(printf("To correct hit terminalp %d or known_splicesite %d, running GMAP on 5' to match with 3' end\n",
		     Stage3end_hittype(hit) == TERMINAL,
		     Stage3end_contains_known_splicesite(hit)));
    
      /* Want high quality because we already have a pretty good answer */
      gmap_hits = align_single_hit_with_gmap(gmap_history,hit,
					     /*extend_left_p*/true,/*extend_right_p*/true,
					     queryuc_ptr,querylength,query_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
					     queryrc,Shortread_invertedp(queryseq),
#endif
					     query_compress_fwd,query_compress_rev,
					     this->plus_segments,this->plus_nsegments,
					     this->minus_segments,this->minus_nsegments,
					     oligoindices_major,noligoindices_major,
					     oligoindices_minor,noligoindices_minor,
					     pairpool,diagpool,dynprogL,dynprogM,dynprogR,
					     user_maxlevel,genestrand);

      gmap_better_p = false;
      missing_hit = querylength - Stage3end_nmatches(hit);
      for (a = gmap_hits; a != NULL; a = List_next(a)) {
	gmap = (Stage3end_T) List_head(a);
	missing_gmap = querylength - Stage3end_nmatches(gmap);
	if (Stage3end_score(gmap) > cutoff_level + gmap_allowance) {
	  debug13(printf("Score is only %d vs cutoff level %d\n",Stage3end_score(gmap),cutoff_level));
	  Stage3end_free(&gmap);
	} else if (missing_gmap < missing_hit/2) {
	  debug13(printf("GMAP with %d matches, %d missing is significantly better than hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap),missing_gmap,Stage3end_nmatches(hit),missing_hit));
	  gmap_better_p = true;
	  new_result = List_push(new_result,(void *) gmap);
	  Stage3end_set_improved_by_gmap(hit);
	} else {
	  debug13(printf("GMAP with %d matches, %d missing is not significantly better than hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap),missing_gmap,Stage3end_nmatches(hit),missing_hit));
	  Stage3end_free(&gmap);
	}
      }
      List_free(&gmap_hits);

      if (gmap_better_p == false) {
	new_result = List_push(new_result,(void *) hit);
      } else {
	Stage3end_free(&hit);
      }
    }
  }

  for ( ; p != NULL; p = p->rest) {
    hit = (Stage3end_T) List_head(p);
    new_result = List_push(new_result,(void *) hit);
  }

  List_free(&result);
  return new_result;
}


/* done_level should probably be renamed final_level.  opt_level
   should probably be renamed found_level or opt_level. */
static List_T
align_end (int *cutoff_level, History_T gmap_history, T this,
	   Compress_T query_compress_fwd, Compress_T query_compress_rev,
	   char *queryuc_ptr, char *queryrc, int querylength, int query_lastpos,
	   Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev, int indexdb_size_threshold, Floors_T *floors_array,

	   Oligoindex_T *oligoindices_major, int noligoindices_major,
	   Oligoindex_T *oligoindices_minor, int noligoindices_minor,
	   Pairpool_T pairpool, Diagpool_T diagpool,
	   Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,

	   int user_maxlevel, int subopt_levels,
	   int indel_penalty_middle, int indel_penalty_end,
	   int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
	   bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
	   bool allvalidp, bool keep_floors_p, int genestrand) {
  List_T hits, subs = NULL, indels = NULL, new_indels,
    singlesplicing = NULL, doublesplicing = NULL, shortendsplicing = NULL,
    longsinglesplicing = NULL, distantsplicing = NULL, good_gmap_hits = NULL, terminals = NULL;
  List_T gmap_hits, p, a;
  Stage3end_T hit, gmap;
  int found_score, done_level, opt_level, fast_level, mismatch_level, nmismatches, max_mismatches_allowed;
  int max_splice_mismatches, i;
  int missing_hit, missing_gmap;
  int nhits = 0, nsplicepairs = 0;
  List_T *donors_plus, *antidonors_plus, *acceptors_plus, *antiacceptors_plus,
    *donors_minus, *antidonors_minus, *acceptors_minus, *antiacceptors_minus;
  bool any_omitted_p, ambiguousp, alloc_floors_p = false, floors_computed_p = false;
  Floors_T floors;
  bool segments_computed_p = false, gmap_better_p, extend_left_p, extend_right_p;

  found_score = querylength;
  fast_level = (querylength + index1interval - 1)/spansize - NREQUIRED_FAST;
  debug(printf("fast_level %d = (querylength %d + index1interval %d - 1)/spansize %d - nrequired_fast %d\n",
	       fast_level,querylength,index1interval,spansize,NREQUIRED_FAST));

#if 0
  /* This prevents complete_mm procedure, needed for short reads */
  if (fast_level < 1 && user_maxlevel < 0) {
    debug(printf("Changing fast_level to 0\n"));
    fast_level = 1;		/* Do at least 1 mismatch */
  }
#endif

  if (user_maxlevel >= 0) {
    *cutoff_level = user_maxlevel;
  } else if (fast_level >= 0) {
    *cutoff_level = fast_level;
  } else {
    *cutoff_level = 0;
  }
  debug(printf("cutoff_level = %d\n",*cutoff_level));

  if (user_maxlevel < 0) {
    if (fast_level >= 0) {
      user_maxlevel = fast_level;
    } else {
      user_maxlevel = 0;
    }
  }
  debug(printf("user_maxlevel = %d\n",user_maxlevel));

#if 0
  if (dibasep) {
    opt_level = querylength;	/* Allow extra because color errors may exceed nt errors */
  }
#endif
  opt_level = user_maxlevel;
  done_level = user_maxlevel /* + subopt_levels.  -- Initially the same */;
  debug(printf("0> opt_level %d, done_level %d\n",opt_level,done_level));

  nhits = 0;
  if (use_sarray_p == true) {
    /* Replaces spanning set */
    Sarray_search_greedy(&found_score,&subs,&indels,&singlesplicing,&doublesplicing,
			 queryuc_ptr,queryrc,querylength,
			 query_compress_fwd,query_compress_rev,/*nmisses_allowed*/fast_level,
			 /*first_read_p*/NOT_APPLICABLE);
    opt_level = (found_score < opt_level) ? found_score : opt_level;
    if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
      done_level = user_maxlevel;
    }

  } else {
    /* 1. Exact.  Requires compress if cmet or genomealt.  Creates and uses spanning set. */
    mismatch_level = 0;
    if (allvalidp == false) {
      debug(printf("Not all oligos are valid, so cannot perform spanning set\n"));
      fast_level = -1;
    } else {
      debug(printf("fast_level = %d\n",fast_level));
      debug(printf("*** Stage 1.  Exact ***\n"));
      subs = find_spanning_exact_matches(&found_score,&nhits,/*hits*/NULL,this,genestrand,
					 querylength,query_lastpos,indexdb_fwd,indexdb_rev,
					 query_compress_fwd,query_compress_rev);
      opt_level = (found_score < opt_level) ? found_score : opt_level;
      if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	done_level = user_maxlevel;
      }
      mismatch_level = 1;
      debug(printf("1> found_score = %d, opt_level %d, done_level %d\n",found_score,opt_level,done_level));
    }

    /* 2. One mismatch.  Requires spanning set and compress. */
    if (allvalidp && querylength >= one_miss_querylength && done_level >= 1) {
      debug(printf("*** Stage 2.  One miss ***\n"));
      subs = find_spanning_onemiss_matches(&found_score,&nhits,subs,this,genestrand,querylength,
					   query_compress_fwd,query_compress_rev);
      opt_level = (found_score < opt_level) ? found_score : opt_level;
      if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	done_level = user_maxlevel;
      }
      mismatch_level = 2;
      debug(printf("2> found_score = %d, opt_level %d, done_level %d\n",found_score,opt_level,done_level));
    }

    /* 3. Mismatches via spanning set.  Requires spanning set and compress. */
    if (allvalidp && done_level >= 2) {
      while (mismatch_level <= fast_level && mismatch_level <= done_level) {
	debug(printf("*** Stage 3 (level %d).  Spanning set mismatches ***\n",mismatch_level));
	subs = find_spanning_multimiss_matches(&found_score,&nhits,subs,this,genestrand,NREQUIRED_FAST,querylength,
					       query_compress_fwd,query_compress_rev,
					       /*nmisses_allowed*/mismatch_level);
	opt_level = (found_score < opt_level) ? found_score : opt_level;
	if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	  done_level = user_maxlevel;
	}
	mismatch_level++;
	debug(printf("3> found_score = %d, opt_level %d, done_level %d\n",found_score,opt_level,done_level));
      }
    }
  }

  /* 4, 5.  Complete set mismatches and indels, omitting frequent oligos */
  debug(printf("Testing done_level %d > fast_level %d\n",done_level,fast_level));
  if (use_sarray_p == true && (subs || indels || singlesplicing || doublesplicing)) {
      /* Skip.  Suffix array already found something.  Also, get memory errors if run both algorithms. */

  } else if (done_level > fast_level || done_level >= indel_penalty_middle || done_level >= indel_penalty_end) {
#if 1
    floors = compute_floors(&any_omitted_p,&alloc_floors_p,floors_array,this,querylength,query_lastpos,
			    indexdb_fwd,indexdb_rev,indexdb_size_threshold,max_end_insertions,
			    /*omit_frequent_p*/true,/*omit_repetitive_p*/true,keep_floors_p);
    floors_computed_p = true;
    complete_set_mm_indels(&found_score,&segments_computed_p,
			   &opt_level,&done_level,user_maxlevel,/*revise_levels_p*/true,
			   &nhits,&subs,&indels,this,query_compress_fwd,query_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			   queryuc_ptr,queryrc,
#endif
			   querylength,query_lastpos,floors,subopt_levels,
			   indel_penalty_middle,indel_penalty_end,
			   allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			   fast_level,genestrand);
#else
    /* Using obsolete masktype */
    if (masktype == MASK_NONE) {
      debug(printf("*** Stage 4,5.  Complete mm/indels with no masking with done_level %d ***\n",done_level));
      complete_set_mm_indels(&found_score,&segments_computed_p,
			     &any_omitted_p,&opt_level,&done_level,user_maxlevel,/*revise_levels_p*/true,
			     &nhits,&subs,&indels,this,query_compress_fwd,query_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			     queryuc_ptr,queryrc,
#endif
			     querylength,query_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     floors_array,subopt_levels,
			     indel_penalty_middle,indel_penalty_end,
			     allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			     fast_level,/*omit_frequent_p*/false,/*omit_repetitive_p*/false,keep_floors_p,
			     genestrand);
    } else {
      debug(printf("*** Stage 4,5.  Complete mm/indels masking frequent oligos with done_level %d ***\n",done_level));
      complete_set_mm_indels(&found_score,&segments_computed_p,
			     &any_omitted_p,&opt_level,&done_level,user_maxlevel,/*revise_levels_p*/true,
			     &nhits,&subs,&indels,this,query_compress_fwd,query_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			     queryuc_ptr,queryrc,
#endif
			     querylength,query_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     floors_array,subopt_levels,
			     indel_penalty_middle,indel_penalty_end,
			     allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			     fast_level,/*omit_frequent_p*/true,
			     /*omit_repetitive_p*/(masktype == MASK_REPETITIVE || masktype == MASK_GREEDY_REPETITIVE) ? true : false,
			     keep_floors_p,genestrand);
      if ((masktype == MASK_GREEDY_FREQUENT || masktype == MASK_GREEDY_REPETITIVE) && subs == NULL && indels == NULL && any_omitted_p == true) {
	FREE(this->minus_segments);
	FREE(this->plus_segments);

	debug(printf("*** Stage 4,5.  Complete mm/indels with no masking with done_level %d ***\n",done_level));
	complete_set_mm_indels(&found_score,&segments_computed_p,
			       &any_omitted_p,&opt_level,&done_level,user_maxlevel,/*revise_levels_p*/true,
			       &subs,&indels,this,query_compress_fwd,query_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			       queryuc_ptr,queryrc,
#endif
			       querylength,query_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			       floors_array,subopt_levels,
			       indel_penalty_middle,indel_penalty_end,
			       allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			       fast_level,/*omit_frequent_p*/false,/*omit_repetitive_p*/false,keep_floors_p,
			       genestrand);
      }
    }
#endif
  }

  /* 6, 7, 8, 9.  Splicing.  Requires compress and all positions fetched */
  if (use_sarray_p == true && (subs || indels || singlesplicing || doublesplicing)) {
      /* Skip.  Suffix array already found something.  Also, get memory errors if run both algorithms.  */

  } else if (knownsplicingp || novelsplicingp) {
    /* 6.  Single splicing */
    debug(printf("Deciding whether to do singlesplicing: done_level %d >=? localsplicing_penalty %d\n",
		 done_level,localsplicing_penalty));
    if (done_level >= localsplicing_penalty) {
      debug(printf("*** Stage 6.  Single splicing masking frequent oligos with done_level %d ***\n",done_level));
      /* Always mask frequent oligos for splicing, which must be transcriptional */
      if (floors_computed_p == false) {
	floors = compute_floors(&any_omitted_p,&alloc_floors_p,floors_array,this,querylength,query_lastpos,
				indexdb_fwd,indexdb_rev,indexdb_size_threshold,max_end_insertions,
				/*omit_frequent_p*/true,/*omit_repetitive_p*/true,keep_floors_p);
	floors_computed_p = true;
      }

      if (segments_computed_p == false) {
	this->plus_segments = identify_all_segments(&this->plus_nsegments,&this->plus_spliceable,&this->plus_nspliceable,
						    this->plus_positions,this->plus_npositions,
						    this->omitted,querylength,query_lastpos,floors,/*plusp*/true);
	this->minus_segments = identify_all_segments(&this->minus_nsegments,&this->minus_spliceable,&this->minus_nspliceable,
						     this->minus_positions,this->minus_npositions,
						     this->omitted,querylength,query_lastpos,floors,/*plusp*/false);
	segments_computed_p = true;
      }

      singlesplicing = complete_set_singlesplicing(&found_score,singlesplicing,floors,this,
						   query_compress_fwd,query_compress_rev,
						   querylength,query_lastpos,
						   localsplicing_penalty,
						   /*max_mismatches_allowed*/done_level - localsplicing_penalty,
						   /*first_read_p*/NOT_APPLICABLE,genestrand,
						   /*subs_or_indels_p*/(subs != NULL || indels != NULL) ? true : false);

#if 0
      /* Mark ambiguous splices only for single-end reads */
      singlesplicing = Stage3end_mark_ambiguous_splices(&ambiguousp,singlesplicing);
#endif
      singlesplicing = Stage3end_optimal_score(singlesplicing,/*cutoff_level*/opt_level,subopt_levels,
					       query_compress_fwd,query_compress_rev,/*keep_gmap_p*/true,
					       /*finalp*/false);

      if (singlesplicing) {
	opt_level = (found_score < opt_level) ? found_score : opt_level;
	if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	  done_level = user_maxlevel;
	}
      }
    }

    /* 7.  Double splicing */
    debug(printf("Deciding whether to do doublesplicing: done_level %d >=? localsplicing_penalty %d\n",
		 done_level,localsplicing_penalty));
    if (done_level >= localsplicing_penalty) {
      debug(printf("*** Stage 7.  Double splicing masking frequent oligos with done_level %d ***\n",done_level));
      if (floors_computed_p == false) {
	floors = compute_floors(&any_omitted_p,&alloc_floors_p,floors_array,this,querylength,query_lastpos,
				indexdb_fwd,indexdb_rev,indexdb_size_threshold,max_end_insertions,
				/*omit_frequent_p*/true,/*omit_repetitive_p*/true,keep_floors_p);
	floors_computed_p = true;
      }
      doublesplicing = complete_set_doublesplicing(&found_score,doublesplicing,floors,this,
						   query_compress_fwd,query_compress_rev,
						   queryuc_ptr,queryrc,querylength,query_lastpos,
						   localsplicing_penalty,min_shortend,
						   /*max_mismatches_allowed*/done_level - localsplicing_penalty,
						   /*pairedp*/false,/*first_read_p*/NOT_APPLICABLE,genestrand,
						   /*subs_or_indels_p*/(subs != NULL || indels != NULL) ? true : false);
      
#if 0
      /* Mark ambiguous splices only for single-end reads */
      doublesplicing = Stage3end_mark_ambiguous_splices(&ambiguousp,doublesplicing);
#endif
      doublesplicing = Stage3end_optimal_score(doublesplicing,/*cutoff_level*/opt_level,subopt_levels,
					       query_compress_fwd,query_compress_rev,/*keep_gmap_p*/true,
					       /*finalp*/false);

      if (doublesplicing) {
	opt_level = (found_score < opt_level) ? found_score : opt_level;
	if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	  done_level = user_maxlevel;
	}
      }
    }

    if (knownsplicingp == true && done_level >= localsplicing_penalty) {
      /* Want >= and not > to give better results.  Negligible effect on speed. */
      /* 8.  Shortend splicing */

      max_splice_mismatches = done_level - localsplicing_penalty;
      debug(printf("*** Stage 8.  Short-end splicing, allowing %d mismatches ***\n",max_splice_mismatches));

      donors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antidonors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      acceptors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antiacceptors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      donors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antidonors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      acceptors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antiacceptors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));

      debug(printf("Starting find_spliceends (plus)\n"));
      find_spliceends_shortend(&donors_plus,&antidonors_plus,&acceptors_plus,&antiacceptors_plus,
			       this->plus_segments,this->plus_nsegments,
#ifdef DEBUG4E
			       /*queryptr*/queryuc_ptr,
#endif
			       floors,querylength,query_lastpos,/*query_compress*/query_compress_fwd,
			       max_splice_mismatches,/*plusp*/true,genestrand);
      debug(printf("Finished find_spliceends (plus)\n"));

      debug(printf("Starting find_spliceends (minus)\n"));
      find_spliceends_shortend(&antidonors_minus,&donors_minus,&antiacceptors_minus,&acceptors_minus,
			       this->minus_segments,this->minus_nsegments,
#ifdef DEBUG4E
			       /*queryptr*/queryrc,
#endif
			       floors,querylength,query_lastpos,/*query_compress*/query_compress_rev,
			       max_splice_mismatches,/*plusp*/false,genestrand);
      debug(printf("Finished find_spliceends (minus)\n"));

      shortendsplicing = find_splicepairs_shortend(&found_score,shortendsplicing,
						   donors_plus,antidonors_plus,
						   acceptors_plus,antiacceptors_plus,
						   donors_minus,antidonors_minus,
						   acceptors_minus,antiacceptors_minus,
						   query_compress_fwd,query_compress_rev,
						   queryuc_ptr,queryrc,min_shortend,
						   localsplicing_penalty,
						   /*max_mismatches_allowed*/max_splice_mismatches,querylength,
						   /*pairedp*/false,/*first_read_p*/NOT_APPLICABLE,genestrand);
      opt_level = (found_score < opt_level) ? found_score : opt_level;
      if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	done_level = user_maxlevel;
      }
      debug(printf("8> found_score = %d, opt_level %d, done_level %d\n",found_score,opt_level,done_level));

      for (i = 0; i <= max_splice_mismatches; i++) {
	substringlist_gc(&(donors_plus[i]));
	substringlist_gc(&(antidonors_plus[i]));
	substringlist_gc(&(acceptors_plus[i]));
	substringlist_gc(&(antiacceptors_plus[i]));
	substringlist_gc(&(donors_minus[i]));
	substringlist_gc(&(antidonors_minus[i]));
	substringlist_gc(&(acceptors_minus[i]));
	substringlist_gc(&(antiacceptors_minus[i]));
      }
      FREE(donors_plus);
      FREE(antidonors_plus);
      FREE(acceptors_plus);
      FREE(antiacceptors_plus);
      FREE(donors_minus);
      FREE(antidonors_minus);
      FREE(acceptors_minus);
      FREE(antiacceptors_minus);
    }


    if (subs || indels || singlesplicing || doublesplicing || shortendsplicing) {
      /* Don't find distant splicing */

    } else if (knownsplicingp == false && novelsplicingp == false) {
      /* Don't find distant splicing */

    } else if (done_level < distantsplicing_penalty) {
      /* Want < and not <=, because otherwise distant splicing does not work on 50-bp reads */
      /* Want <= and not <, because distant splicing needs to be better than other alternatives */
      /* Don't find distant splicing */

    } else {
      /* 9.  Find distant splicing iteratively using both known and novel splice sites */
      max_splice_mismatches = done_level - distantsplicing_penalty;
      debug(printf("*** Stage 9.  Distant splice ends, allowing %d mismatches ***\n",max_splice_mismatches));

      donors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antidonors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      acceptors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antiacceptors_plus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      donors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antidonors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      acceptors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));
      antiacceptors_minus = (List_T *) CALLOC(max_splice_mismatches+1,sizeof(List_T));

      debug(printf("Starting find_spliceends (plus)\n"));
      find_spliceends_distant(&donors_plus,&antidonors_plus,&acceptors_plus,&antiacceptors_plus,
			      this->plus_segments,this->plus_nsegments,
#ifdef DEBUG4E
			      /*queryptr*/queryuc_ptr,
#endif
			      floors,querylength,query_lastpos,/*query_compress*/query_compress_fwd,
			      max_splice_mismatches,/*plusp*/true,genestrand);
      debug(printf("Finished find_spliceends (plus)\n"));


      debug(printf("Starting find_spliceends (minus)\n"));
      find_spliceends_distant(&antidonors_minus,&donors_minus,&antiacceptors_minus,&acceptors_minus,
			      this->minus_segments,this->minus_nsegments,
#ifdef DEBUG4E
			      /*queryptr*/queryrc,
#endif
			      floors,querylength,query_lastpos,/*query_compress*/query_compress_rev,
			      max_splice_mismatches,/*plusp*/false,genestrand);
      debug(printf("Finished find_spliceends (minus)\n"));


      nmismatches = 0;
      ambiguousp = false;
      while (longsinglesplicing == NULL &&
	     nmismatches <= done_level - distantsplicing_penalty &&
	     nsplicepairs < MAXCHIMERAPATHS && ambiguousp == false) {
	debug(printf("*** Stage 9.  Distant splicing, allowing %d mismatches ***\n",nmismatches));

	debug4e(printf("Sorting splice ends\n"));
	donors_plus[nmismatches] = Substring_sort_chimera_halves(donors_plus[nmismatches],/*ascendingp*/true);
	acceptors_plus[nmismatches] = Substring_sort_chimera_halves(acceptors_plus[nmismatches],/*ascendingp*/true);

	antidonors_plus[nmismatches] = Substring_sort_chimera_halves(antidonors_plus[nmismatches],/*ascendingp*/false);
	antiacceptors_plus[nmismatches] = Substring_sort_chimera_halves(antiacceptors_plus[nmismatches],/*ascendingp*/false);

	donors_minus[nmismatches] = Substring_sort_chimera_halves(donors_minus[nmismatches],/*ascendingp*/false);
	acceptors_minus[nmismatches] = Substring_sort_chimera_halves(acceptors_minus[nmismatches],/*ascendingp*/false);

	antidonors_minus[nmismatches] = Substring_sort_chimera_halves(antidonors_minus[nmismatches],/*ascendingp*/true);
	antiacceptors_minus[nmismatches] = Substring_sort_chimera_halves(antiacceptors_minus[nmismatches],/*ascendingp*/true);

	debug4e(printf("Splice ends at %d nmismatches: +donors/acceptors %d/%d, +antidonors/antiacceptors %d/%d, -donors/acceptors %d/%d, -antidonors/antiacceptors %d/%d\n",
		       nmismatches,
		       List_length(donors_plus[nmismatches]),List_length(acceptors_plus[nmismatches]),
		       List_length(antidonors_plus[nmismatches]),List_length(antiacceptors_plus[nmismatches]),
		       List_length(donors_minus[nmismatches]),List_length(acceptors_minus[nmismatches]),
		       List_length(antidonors_minus[nmismatches]),List_length(antiacceptors_minus[nmismatches])));

	distantsplicing = find_splicepairs_distant(&found_score,&nsplicepairs,&longsinglesplicing,distantsplicing,
						   donors_plus,antidonors_plus,acceptors_plus,antiacceptors_plus,
						   donors_minus,antidonors_minus,acceptors_minus,antiacceptors_minus,
						   localsplicing_penalty,distantsplicing_penalty,
						   querylength,nmismatches,/*first_read_p*/NOT_APPLICABLE);
#if 0
	assert(List_length(distantsplicing) <= 1);
#endif

#if 0
	/* Mark ambiguous splices only for single-end reads */
	distantsplicing = Stage3end_mark_ambiguous_splices(&ambiguousp,distantsplicing);
#endif


	/* Excess distant splicing should be freed already in find_splicepairs_distant */
	debug(printf("Entering Stage3end_optimal_score with %d hits\n",List_length(distantsplicing)));
	distantsplicing = Stage3end_optimal_score(distantsplicing,opt_level,subopt_levels,
						  query_compress_fwd,query_compress_rev,/*keep_gmap_p*/true,
						  /*finalp*/false);
	debug(printf("Exiting Stage3end_optimal_score with %d hits\n",List_length(distantsplicing)));

	if (distantsplicing) {
	  opt_level = (found_score < opt_level) ? found_score : opt_level;
	  if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	    done_level = user_maxlevel;
	  }
	  debug(printf("9> found_score = %d, opt_level %d, done_level %d\n",found_score,opt_level,done_level));
	}
	nmismatches++;

      }

      if (longsinglesplicing != NULL) {
	debug(printf("Entering Stage3end_optimal_score with %d longsinglesplicing hits\n",List_length(longsinglesplicing)));
	longsinglesplicing = Stage3end_optimal_score(longsinglesplicing,opt_level,subopt_levels,
						     query_compress_fwd,query_compress_rev,/*keep_gmap_p*/true,
						     /*finalp*/false);
	debug(printf("Exiting Stage3end_optimal_score with %d hits\n",List_length(longsinglesplicing)));

	opt_level = (found_score < opt_level) ? found_score : opt_level;
	if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
	  done_level = user_maxlevel;
	}
	debug(printf("9> found_score = %d, opt_level %d, done_level %d\n",found_score,opt_level,done_level));
      }

      for (i = 0; i <= max_splice_mismatches; i++) {
	substringlist_gc(&(donors_plus[i]));
	substringlist_gc(&(antidonors_plus[i]));
	substringlist_gc(&(acceptors_plus[i]));
	substringlist_gc(&(antiacceptors_plus[i]));
	substringlist_gc(&(donors_minus[i]));
	substringlist_gc(&(antidonors_minus[i]));
	substringlist_gc(&(acceptors_minus[i]));
	substringlist_gc(&(antiacceptors_minus[i]));
      }
      FREE(donors_plus);
      FREE(antidonors_plus);
      FREE(acceptors_plus);
      FREE(antiacceptors_plus);
      FREE(donors_minus);
      FREE(antidonors_minus);
      FREE(acceptors_minus);
      FREE(antiacceptors_minus);
    }
    debug(printf("%d single splices, %d double splices, %d short-end splices, %d long single splices, %d distant splices\n",
		 List_length(singlesplicing),List_length(doublesplicing),
		 List_length(shortendsplicing),List_length(longsinglesplicing),
		 List_length(distantsplicing)));
  }

  debug(printf("Before terminals:\n"));
  debug(printf("  subs: %d\n",List_length(subs)));
  debug(printf("  indels: %d\n",List_length(indels)));
  debug(printf("  singlesplicing %d\n",List_length(singlesplicing)));
  debug(printf("  doublesplicing %d\n",List_length(doublesplicing)));
  debug(printf("  shortendsplicing: %d\n",List_length(shortendsplicing)));
  debug(printf("  longsinglesplicing %d\n",List_length(longsinglesplicing)));
  debug(printf("  distantsplicing: %d\n",List_length(distantsplicing)));
  debug(printf("  done_level: %d\n",done_level));

  /* 9.  GMAP indels_plus_knownsplicing */
  if (indels != NULL && gmap_indel_knownsplice_p == true) {
    debug13(printf("GMAP indels plus known splicing: %d indels\n",List_length(indels)));
    new_indels = (List_T) NULL;
    for (p = indels; p != NULL; p = List_next(p)) {
      hit = (Stage3end_T) List_head(p);
      if (Stage3end_indel_contains_known_splicesite(&extend_left_p,&extend_right_p,hit) == false) {
	new_indels = List_push(new_indels,(void *) hit);
      } else {
	gmap_hits = align_single_hit_with_gmap(gmap_history,hit,extend_left_p,extend_right_p,
					       queryuc_ptr,querylength,query_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
					       queryrc,Shortread_invertedp(queryseq),
#endif
					       query_compress_fwd,query_compress_rev,
					       this->plus_segments,this->plus_nsegments,this->minus_segments,this->minus_nsegments,
					       oligoindices_major,noligoindices_major,
					       oligoindices_minor,noligoindices_minor,
					       pairpool,diagpool,dynprogL,dynprogM,dynprogR,
					       user_maxlevel,genestrand);

	gmap_better_p = false;
	missing_hit = querylength - Stage3end_nmatches(hit);
	for (a = gmap_hits; a != NULL; a = List_next(a)) {
	  gmap = (Stage3end_T) List_head(a);
	  missing_gmap = querylength - Stage3end_nmatches(gmap);
	  if (Stage3end_score(gmap) > opt_level + gmap_allowance) {
	    debug13(printf("Score is only %d vs opt_level %d\n",Stage3end_score(gmap),opt_level));
	    Stage3end_free(&gmap);
	  } else if (missing_gmap < missing_hit/2) {
	    debug13(printf("GMAP with %d matches, %d missing is significantly better than indel with %d matches, %d missing\n",
			   Stage3end_nmatches(gmap),missing_gmap,Stage3end_nmatches(hit),missing_hit));
	    good_gmap_hits = List_push(good_gmap_hits,(void *) gmap);
	    gmap_better_p = true;
	    Stage3end_set_improved_by_gmap(hit);
	  } else {
	    debug13(printf("GMAP with %d matches, %d missing is not significantly better than indel with %d matches, %d missing\n",
			   Stage3end_nmatches(gmap),missing_gmap,Stage3end_nmatches(hit),missing_hit));
	    Stage3end_free(&gmap);
	  }
	}
	List_free(&gmap_hits);

	if (gmap_better_p == true) {
	  Stage3end_free(&hit);
	} else {
	  new_indels = List_push(new_indels,(void *) hit);
	}
      }
    }
    List_free(&indels);
    indels = new_indels;
  }


  /* 10.  Terminals */
  /* Previously did not find terminals if (subs || indels || good_gmap_hits || singlesplicing || doublesplicing || shortendsplicing || distantsplicing) */

  if (done_level >= terminal_threshold) {
    max_mismatches_allowed = done_level;
    debug(printf("*** Stage 10.  Terminals up to %d mismatches ***\n",max_mismatches_allowed));
    if (floors_computed_p == false) {
      floors = compute_floors(&any_omitted_p,&alloc_floors_p,floors_array,this,querylength,query_lastpos,
			      indexdb_fwd,indexdb_rev,indexdb_size_threshold,max_end_insertions,
			      /*omit_frequent_p*/true,/*omit_repetitive_p*/true,keep_floors_p);
    }

    if (segments_computed_p == false) {
      this->plus_segments = identify_all_segments_for_terminals(&this->plus_nsegments,this->plus_positions,this->plus_npositions,
								this->omitted,querylength,query_lastpos,
								floors,max_mismatches_allowed,
								/*plusp*/true);
      this->minus_segments = identify_all_segments_for_terminals(&this->minus_nsegments,this->minus_positions,this->minus_npositions,
								 this->omitted,querylength,query_lastpos,
								 floors,max_mismatches_allowed,
								 /*plusp*/false);
    }

    terminals = find_terminals(this->plus_segments,this->plus_nsegments,this->minus_segments,this->minus_nsegments,
#ifdef DEBUG4T
			       queryuc_ptr,queryrc,
#endif
			       floors,querylength,query_lastpos,
			       query_compress_fwd,query_compress_rev,
			       max_mismatches_allowed,/*max_terminal_length*/end_miss_one,genestrand);
#if 0
    opt_level = (found_score < opt_level) ? found_score : opt_level;
    if ((done_level = opt_level + subopt_levels) > user_maxlevel) {
      done_level = user_maxlevel;
    }
    debug(printf("10> found_score = %d, opt_level %d, done_level %d\n",found_score,opt_level,done_level));
#endif
  }


  /* This step does make sense, in contrast with paired-end alignment,
     where terminals should get improved by GMAP improvement */
  debug13(printf("%d terminals (vs max_gmap_terminal %d)\n",List_length(terminals),max_gmap_terminal));
  if (terminals != NULL && gmap_terminal_p == true) {
    /* 11.  GMAP terminal */

#if 0
    /* This is done for paired-ends, but should not be necessary for single-end */
    debug13(printf("Before remove overlaps at cutoff level %d: %d hits\n",opt_level,List_length(terminals)));
    terminals = Stage3end_sort_bymatches(Stage3end_remove_overlaps(terminals,/*finalp*/false));
    debug13(printf("After remove overlaps: %d\n",List_length(terminals)));
#endif

    if (List_length(terminals) <= max_gmap_terminal) {
      debug13(printf("%d hits\n",List_length(terminals)));
      debug13(printf("For terminals, running GMAP on single end to match with terminal\n"));

      for (p = terminals; p != NULL; p = List_next(p)) {
	hit = (Stage3end_T) List_head(p);
	/* Was align_single_terminal_with_gmap() */
	gmap_hits = align_single_hit_with_gmap(gmap_history,hit,
					       /*extend_left_p*/Stage3end_start_endtype(hit) == TERM,
					       /*extend_right_p*/Stage3end_end_endtype(hit) == TERM,
					       queryuc_ptr,querylength,query_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
					       queryrc,Shortread_invertedp(queryseq),
#endif
					       query_compress_fwd,query_compress_rev,
					       this->plus_segments,this->plus_nsegments,this->minus_segments,this->minus_nsegments,
					       oligoindices_major,noligoindices_major,
					       oligoindices_minor,noligoindices_minor,
					       pairpool,diagpool,dynprogL,dynprogM,dynprogR,
					       user_maxlevel,genestrand);

	missing_hit = querylength - Stage3end_nmatches(hit);
	for (a = gmap_hits; a != NULL; a = List_next(a)) {
	  gmap = (Stage3end_T) List_head(a);
	  missing_gmap = querylength - Stage3end_nmatches(gmap);
	  if (Stage3end_score(gmap) > opt_level + gmap_allowance) {
	    debug13(printf("Score is only %d vs opt_level %d\n",Stage3end_score(gmap),opt_level));
	    Stage3end_free(&gmap);
	  } else if (missing_gmap < missing_hit/2) {
	    debug13(printf("GMAP with %d matches, %d missing is significantly better than terminal with %d matches, %d missing\n",
			   Stage3end_nmatches(gmap),missing_gmap,Stage3end_nmatches(hit),missing_hit));
	    good_gmap_hits = List_push(good_gmap_hits,(void *) gmap);
	    Stage3end_set_improved_by_gmap(hit);
	  } else {
	    debug13(printf("GMAP with %d matches, %d missing is not significantly better than terminal with %d matches, %d missing\n",
			   Stage3end_nmatches(gmap),missing_gmap,Stage3end_nmatches(hit),missing_hit));
	    Stage3end_free(&gmap);
	  }
	}
	List_free(&gmap_hits);
      }
    }
  }
  debug13(printf("Have %d good GMAP hits\n",List_length(good_gmap_hits)));


  if (alloc_floors_p == true) {
    Floors_free(&floors);
  }

  /* Keep good_gmap_hits found in step 10 */
  hits = List_append(subs,
		     List_append(indels,
				 List_append(singlesplicing,
					     List_append(longsinglesplicing,
							 List_append(doublesplicing,
								     List_append(shortendsplicing,
										 List_append(distantsplicing,
											     List_append(good_gmap_hits,terminals))))))));

  if (gmap_improvement_p == false) {
    debug(printf("No GMAP improvement: Before remove_overlaps at cutoff level %d: %d\n",*cutoff_level,List_length(hits)));
    hits = Stage3end_optimal_score(hits,*cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/true,/*finalp*/true);
    hits = Stage3end_filter_terminals(hits);
    hits = Stage3end_remove_overlaps(hits,/*finalp*/true);
    hits = Stage3end_optimal_score(hits,*cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/false,/*finalp*/true);
    hits = Stage3end_resolve_multimapping(hits);
    debug(printf("After remove_overlaps: %d\n",List_length(hits)));
  
  } else {
    debug(printf("GMAP improvement: Before remove_overlaps at cutoff level %d: %d\n",*cutoff_level,List_length(hits)));
    hits = Stage3end_optimal_score(hits,*cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/true,/*finalp*/false);
    hits = Stage3end_filter_terminals(hits);
    hits = Stage3end_remove_overlaps(hits,/*finalp*/false);
    hits = Stage3end_optimal_score(hits,*cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/false,/*finalp*/false);
    hits = Stage3end_resolve_multimapping(hits);
    debug(printf("After remove_overlaps: %d\n",List_length(hits)));

    hits = align_singleend_with_gmap(gmap_history,hits,this,query_compress_fwd,query_compress_rev,
				     queryuc_ptr,querylength,query_lastpos,
				     oligoindices_major,noligoindices_major,
				     oligoindices_minor,noligoindices_minor,
				     pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel,*cutoff_level);
    hits = Stage3end_optimal_score(hits,*cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/true,/*finalp*/true);
    hits = Stage3end_filter_terminals(hits);
    hits = Stage3end_remove_overlaps(hits,/*finalp*/true);
    hits = Stage3end_optimal_score(hits,*cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/false,/*finalp*/true);
    hits = Stage3end_resolve_multimapping(hits);
  }

  hits = Stage3end_remove_circular_alias(hits);

  return hits;
}


static Stage3end_T *
single_read (int *npaths, int *first_absmq, int *second_absmq,
	     Shortread_T queryseq, Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev,
	     int indexdb_size_threshold, Genome_T genome, Floors_T *floors_array,
	     double user_maxlevel_float, int subopt_levels,
	     int indel_penalty_middle, int indel_penalty_end,
	     bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
	     int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
	     Oligoindex_T *oligoindices_major, int noligoindices_major,
	     Oligoindex_T *oligoindices_minor, int noligoindices_minor,
	     Pairpool_T pairpool, Diagpool_T diagpool,
	     Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
	     bool keep_floors_p) {
  Stage3end_T *stage3array;
  History_T gmap_history;
  List_T hits = NULL;
  T this = NULL;
  int user_maxlevel;
  int querylength, query_lastpos, cutoff_level;
  char *queryuc_ptr, *quality_string;
  char queryrc[MAX_READLENGTH+1];
  Compress_T query_compress_fwd = NULL, query_compress_rev = NULL;
  bool allvalidp;

  if ((querylength = Shortread_fulllength(queryseq)) < min_readlength) {
    fprintf(stderr,"GSNAP cannot handle reads shorter than %d bp with this k-mer = %d genomic index.\n",
	    min_readlength,index1part);
    fprintf(stderr,"You may want to build a genomic index with a smaller k-mer value using the -k flag to gmap_build\n");
    *npaths = 0;
    return (Stage3end_T *) NULL;

  } else if (querylength > MAX_READLENGTH) {
    fprintf(stderr,"GSNAP cannot handle reads longer than %d bp.  Either run configure and make again with a higher value of MAX_READLENGTH, or consider using GMAP instead.\n",
	    MAX_READLENGTH);
    *npaths = 0;
    return (Stage3end_T *) NULL;

  } else {
    if (user_maxlevel_float < 0.0) {
      user_maxlevel = -1;
    } else if (user_maxlevel_float > 0.0 && user_maxlevel_float < 1.0) {
      user_maxlevel = (int) rint(user_maxlevel_float * (double) querylength);
    } else {
      user_maxlevel = (int) user_maxlevel_float;
    }

    /* Limit search on repetitive sequences */
    queryuc_ptr = Shortread_fullpointer_uc(queryseq);
    quality_string = Shortread_quality_string(queryseq);
    if (check_dinucleotides(queryuc_ptr,querylength) == false) {
      user_maxlevel = 0;
    }

    query_compress_fwd = Compress_new_fwd(queryuc_ptr,querylength);
    query_compress_rev = Compress_new_rev(queryuc_ptr,querylength);
    make_complement_buffered(queryrc,queryuc_ptr,querylength);

    this = Stage1_new(querylength);
    query_lastpos = querylength - index1part;

    if (read_oligos(&allvalidp,this,queryuc_ptr,querylength,query_lastpos,/*genestrand*/0) == 0) {
      debug(printf("Aborting because no hits found anywhere\n"));
      *npaths = 0;
      Compress_free(&query_compress_fwd);
      Compress_free(&query_compress_rev);
      Stage1_free(&this,querylength);
      return (Stage3end_T *) NULL;

    } else {

      gmap_history = History_new();
      hits = align_end(&cutoff_level,gmap_history,this,
		       query_compress_fwd,query_compress_rev,
		       queryuc_ptr,queryrc,querylength,query_lastpos,
		       indexdb_fwd,indexdb_rev,indexdb_size_threshold,floors_array,
		       oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		       pairpool,diagpool,dynprogL,dynprogM,dynprogR,
		       user_maxlevel,subopt_levels,
		       indel_penalty_middle,indel_penalty_end,
		       localsplicing_penalty,distantsplicing_penalty,min_shortend,
		       allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
		       allvalidp,keep_floors_p,/*genestrand*/0);

      if ((*npaths = List_length(hits)) == 0) {
	stage3array = (Stage3end_T *) NULL;
      } else {
	stage3array = (Stage3end_T *) List_to_array_out(hits,NULL); List_free(&hits);
	stage3array = Stage3end_eval_and_sort(&(*npaths),&(*first_absmq),&(*second_absmq),
					      stage3array,maxpaths_search,queryseq,
					      query_compress_fwd,query_compress_rev,
					      genome,quality_string,/*displayp*/true);
      }

      History_free(&gmap_history);
      Compress_free(&query_compress_fwd);
      Compress_free(&query_compress_rev);
      Stage1_free(&this,querylength); 
      return stage3array;
    }
  }
}


static Stage3end_T *
single_read_tolerant_nonstranded (int *npaths, int *first_absmq, int *second_absmq,
				  Shortread_T queryseq, Indexdb_T indexdb_geneplus, Indexdb_T indexdb_geneminus,
				  int indexdb_size_threshold, Genome_T genome, Floors_T *floors_array,
				  double user_maxlevel_float, int subopt_levels,
				  int indel_penalty_middle, int indel_penalty_end,
				  bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
				  int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
				  Oligoindex_T *oligoindices_major, int noligoindices_major,
				  Oligoindex_T *oligoindices_minor, int noligoindices_minor,
				  Pairpool_T pairpool, Diagpool_T diagpool,
				  Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
				  bool keep_floors_p) {
  Stage3end_T *stage3array;
  History_T gmap_history;
  List_T hits, hits_geneplus = NULL, hits_geneminus = NULL;
  T this_geneplus = NULL, this_geneminus = NULL;
  int user_maxlevel;
  int querylength, query_lastpos, cutoff_level;
  char *queryuc_ptr, *quality_string;
  char queryrc[MAX_READLENGTH+1];
  Compress_T query_compress_fwd = NULL, query_compress_rev = NULL;
  bool allvalidp;

  if ((querylength = Shortread_fulllength(queryseq)) < min_readlength) {
    fprintf(stderr,"GSNAP cannot handle reads shorter than %d bp with this k-mer = %d genomic index.\n",
	    min_readlength,index1part);
    fprintf(stderr,"You may want to build a genomic index with a smaller k-mer value using the -k flag to gmap_build\n");
    *npaths = 0;
    return (Stage3end_T *) NULL;

  } else if (querylength > MAX_READLENGTH) {
    fprintf(stderr,"GSNAP cannot handle reads longer than %d bp.  Either run configure and make again with a higher value of MAX_READLENGTH, or consider using GMAP instead.\n",
	    MAX_READLENGTH);
    *npaths = 0;
    return (Stage3end_T *) NULL;

  } else {
    if (user_maxlevel_float < 0.0) {
      user_maxlevel = -1;
    } else if (user_maxlevel_float > 0.0 && user_maxlevel_float < 1.0) {
      user_maxlevel = (int) rint(user_maxlevel_float * (double) querylength);
    } else {
      user_maxlevel = (int) user_maxlevel_float;
    }

    this_geneplus = Stage1_new(querylength);
    this_geneminus = Stage1_new(querylength);

    queryuc_ptr = Shortread_fullpointer_uc(queryseq);
    quality_string = Shortread_quality_string(queryseq);
    query_lastpos = querylength - index1part;

    /* Limit search on repetitive sequences */
    if (check_dinucleotides(queryuc_ptr,querylength) == false) {
      user_maxlevel = 0;
    }

    query_compress_fwd = Compress_new_fwd(queryuc_ptr,querylength);
    query_compress_rev = Compress_new_rev(queryuc_ptr,querylength);
    gmap_history = History_new();
    make_complement_buffered(queryrc,queryuc_ptr,querylength);

    if (read_oligos(&allvalidp,this_geneplus,queryuc_ptr,querylength,query_lastpos,/*genestrand*/+1) > 0) {
      hits_geneplus = align_end(&cutoff_level,gmap_history,this_geneplus,
				query_compress_fwd,query_compress_rev,
				queryuc_ptr,queryrc,querylength,query_lastpos,
				indexdb_geneplus,indexdb_geneplus,indexdb_size_threshold,floors_array,
				oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
				pairpool,diagpool,dynprogL,dynprogM,dynprogR,
				user_maxlevel,subopt_levels,
				indel_penalty_middle,indel_penalty_end,
				localsplicing_penalty,distantsplicing_penalty,min_shortend,
				allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
				allvalidp,keep_floors_p,/*genestrand*/+1);
    }

    if (read_oligos(&allvalidp,this_geneminus,queryuc_ptr,querylength,query_lastpos,/*genestrand*/+2) > 0) {
      hits_geneminus = align_end(&cutoff_level,gmap_history,this_geneminus,
				 query_compress_fwd,query_compress_rev,
				 queryuc_ptr,queryrc,querylength,query_lastpos,
				 indexdb_geneminus,indexdb_geneminus,indexdb_size_threshold,floors_array,
				 oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
				 pairpool,diagpool,dynprogL,dynprogM,dynprogR,
				 user_maxlevel,subopt_levels,
				 indel_penalty_middle,indel_penalty_end,
				 localsplicing_penalty,distantsplicing_penalty,min_shortend,
				 allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
				 allvalidp,keep_floors_p,/*genestrand*/+2);
    }

    hits = List_append(hits_geneplus,hits_geneminus);
    hits = Stage3end_optimal_score(hits,cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/true,/*finalp*/true);
    hits = Stage3end_filter_terminals(hits);
    hits = Stage3end_remove_overlaps(hits,/*finalp*/true);
    hits = Stage3end_optimal_score(hits,cutoff_level,subopt_levels,query_compress_fwd,query_compress_rev,
				   /*keep_gmap_p*/false,/*finalp*/true);
    hits = Stage3end_resolve_multimapping(hits);

    if ((*npaths = List_length(hits)) == 0) {
      stage3array = (Stage3end_T *) NULL;
    } else {
      stage3array = (Stage3end_T *) List_to_array_out(hits,NULL); List_free(&hits);
      stage3array = Stage3end_eval_and_sort(&(*npaths),&(*first_absmq),&(*second_absmq),
					    stage3array,maxpaths_search,queryseq,
					    query_compress_fwd,query_compress_rev,
					    genome,quality_string,/*displayp*/true);
    }
    
    History_free(&gmap_history);
    Compress_free(&query_compress_fwd);
    Compress_free(&query_compress_rev);
    Stage1_free(&this_geneminus,querylength); 
    Stage1_free(&this_geneplus,querylength); 
    return stage3array;
  }
}


Stage3end_T *
Stage1_single_read (int *npaths, int *first_absmq, int *second_absmq,
		    Shortread_T queryseq, Indexdb_T indexdb, Indexdb_T indexdb2,
		    int indexdb_size_threshold, Genome_T genome, Floors_T *floors_array,
		    double user_maxlevel_float, int subopt_levels,
		    int indel_penalty_middle, int indel_penalty_end,
		    bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
		    int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
		    Oligoindex_T *oligoindices_major, int noligoindices_major,
		    Oligoindex_T *oligoindices_minor, int noligoindices_minor,
		    Pairpool_T pairpool, Diagpool_T diagpool,
		    Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		    bool keep_floors_p) {

  if (mode == STANDARD || mode == CMET_STRANDED || mode == ATOI_STRANDED) {
    return single_read(&(*npaths),&(*first_absmq),&(*second_absmq),
		       queryseq,/*indexdb_fwd*/indexdb,/*indexdb_rev*/indexdb2,
		       indexdb_size_threshold,genome,floors_array,user_maxlevel_float,subopt_levels,
		       indel_penalty_middle,indel_penalty_end,
		       allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
		       localsplicing_penalty,distantsplicing_penalty,min_shortend,
		       oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		       pairpool,diagpool,dynprogL,dynprogM,dynprogR,keep_floors_p);
  } else if (mode == CMET_NONSTRANDED || mode == ATOI_NONSTRANDED) {
    return single_read_tolerant_nonstranded(&(*npaths),&(*first_absmq),&(*second_absmq),queryseq,/*indexdb_geneplus*/indexdb,/*indexdb_geneminus*/indexdb2,
					    indexdb_size_threshold,genome,floors_array,user_maxlevel_float,subopt_levels,
					    indel_penalty_middle,indel_penalty_end,
					    allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
					    localsplicing_penalty,distantsplicing_penalty,min_shortend,
					    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
					    pairpool,diagpool,dynprogL,dynprogM,dynprogR,keep_floors_p);
  } else {
    fprintf(stderr,"Do not recognize mode %d\n",mode);
    abort();
  }
}



/* #define HITARRAY_SHORTENDSPLICING 4 */
/* #define HITARRAY_DISTANTSPLICING 4 */


static List_T
align_halfmapping_with_gmap (History_T gmap_history, Stage3end_T hit5, Stage3end_T hit3, 
			     Shortread_T queryseq5, Shortread_T queryseq3,
			     char *queryuc_ptr, int querylength, int query_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
			     char *queryrc, bool invertedp,
#endif
			     Compress_T query_compress_fwd, Compress_T query_compress_rev,
			     struct Segment_T *plus_segments, int plus_nsegments,
			     struct Segment_T *minus_segments, int minus_nsegments,
			     Oligoindex_T *oligoindices_major, int noligoindices_major,
			     Oligoindex_T *oligoindices_minor, int noligoindices_minor,
			     Pairpool_T pairpool, Diagpool_T diagpool,
			     Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
			     int pairmax, Chrpos_T shortsplicedist, int user_maxlevel,
			     int genestrand) {
  List_T hits = NULL;
  int sensedir, sense_try;
  int overlap;

  int zero_offset = 0;
  Univcoord_T segmentstart, segmentend;
  Univcoord_T genomicbound, genomicbound2, mappingstart, mappingend,
    chroffset, chrhigh, mappingpos;
  Univcoord_T close_mappingstart_greedy, close_mappingend_greedy, close_mappingstart_last, close_mappingend_last;
  Univcoord_T middle_mappingstart_greedy, middle_mappingend_greedy, middle_mappingstart_last, middle_mappingend_last;
  Univcoord_T knownsplice_limit_low, knownsplice_limit_high;
  Univcoord_T close_knownsplice_limit_low, close_knownsplice_limit_high;
  Chrpos_T chrlength;
  Chrnum_T chrnum;
  bool close_mappingstart_p = false, close_mappingend_p = false;
  bool middle_mappingstart_p = false, middle_mappingend_p = false;
  bool fallback_mappingstart_p, fallback_mappingend_p;
  bool good_start_p, good_end_p, watsonp, favor_right_p;

  int starti, endi, i;


  if (hit3 == NULL) {
    if ((chrnum = Stage3end_chrnum(hit5)) == 0) {
      /* Translocation */
      return (List_T) NULL;

    } else if ((watsonp = Stage3end_plusp(hit5)) == true) {
      chroffset = Stage3end_chroffset(hit5);
      chrhigh = Stage3end_chrhigh(hit5);
      chrlength = Stage3end_chrlength(hit5);

      if (Shortread_find_primers(queryseq5,queryseq3) == true) {
	/* Go from genomicstart */
	debug13(printf("Found primers\n"));
	genomicbound = subtract_bounded(Stage3end_genomicstart(hit5),querylength,chroffset);

      } else {
	genomicbound = subtract_bounded(Stage3end_genomicend(hit5),querylength,chroffset);

	/* TODO: Previously called Shortread_find_overlap.  Now with Shortread_max_overlap, can optimize this code */
	if ((overlap = Shortread_max_overlap(queryseq5,queryseq3)) > 0 &&
	    Stage3end_genomicbound_from_end(&genomicbound2,hit5,overlap,chroffset) == true) {
	  debug13(printf("Found overlap of %d\n",overlap));
	  if (genomicbound2 < genomicbound) {
	    zero_offset = genomicbound - genomicbound2;
	    genomicbound = genomicbound2;
	  }
	}
      }

      debug13(printf("Case 1: hit5 plus %s %u..%u (sensedir %d) => genomicbound %u\n",
		     Stage3end_hittype_string(hit5),
		     Stage3end_genomicstart(hit5) - chroffset,Stage3end_genomicend(hit5) - chroffset,
		     Stage3end_sensedir(hit5),genomicbound - chroffset));

      knownsplice_limit_low = mappingstart = segmentstart = genomicbound;
      knownsplice_limit_high =  add_bounded(Stage3end_genomicend(hit5),pairmax + shortsplicedist,chrhigh);
      segmentend = add_bounded(Stage3end_genomicend(hit5),pairmax,chrhigh);
#ifdef LONG_ENDSPLICES
      mappingend = add_bounded(Stage3end_genomicend(hit5),pairmax + shortsplicedist,chrhigh);
#else
      mappingend = add_bounded(Stage3end_genomicend(hit5),pairmax + shortsplicedist_novelend,chrhigh);
#endif
      debug13(printf("Original bounds: knownsplice_limit_low %u, knownsplice_limit_high %u, mappingend %u\n",
		     knownsplice_limit_low - chroffset,knownsplice_limit_high - chroffset,mappingend - chroffset));

      close_mappingend_last = middle_mappingend_last = Stage3end_genomicend(hit5);
      close_mappingend_greedy = middle_mappingend_greedy = segmentend;

      if (plus_nsegments > 0) {
	/* Use segments to bound */
	debug13(printf("Finding segments from segmentstart %u to segmentend %u (plus_nsegments %d)\n",
		       segmentstart - chroffset,segmentend - chroffset,plus_nsegments));
	starti = endi = -1;
	i = binary_search_segments(0,plus_nsegments-1,plus_segments,segmentstart);
	while (i < plus_nsegments - 1 && plus_segments[i].diagonal == (Univcoord_T) -1) {
	  i++;
	}
	starti = i;
	while (plus_segments[i].diagonal < segmentend) {
	  endi = i;
	  i++;
	}
	if (starti >= 0 && endi >= 0) {
	  debug13(printf("starti = %d, endi = %d\n",starti,endi));
	  assert(starti <= endi);
	  for (i = starti; i <= endi; i++) {
	    debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			   (Chrpos_T) (plus_segments[i].diagonal - chroffset),plus_segments[i].diagonal,
			   plus_segments[i].querypos5,plus_segments[i].querypos3));
	    if (query_lastpos - plus_segments[i].querypos3 >= STAGE2_MIN_OLIGO + index1interval) {
	      /* Case 1. Missing end of query, so there could be a middle splice */
	      debug13b(printf("  query_lastpos %d - querypos3 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			      query_lastpos,plus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = add_bounded(plus_segments[i].diagonal,shortsplicedist_novelend,chrhigh)) < middle_mappingend_greedy &&
		  mappingpos > genomicbound) {
		middle_mappingend_greedy = mappingpos;
		middle_mappingend_p = true;
		debug13(printf("  Redefining middle mappingend greedy to %u\n",middle_mappingend_greedy - chroffset));
	      }
#ifdef LONG_ENDSPLICES
	      if ((mappingpos = add_bounded(plus_segments[i].diagonal,shortsplicedist,chrhigh)) > middle_mappingend_last) {
		/* Use > for NOT_GREEDY */
		middle_mappingend_last = mappingpos;
		middle_mappingend_p = true;
		debug13(printf("  Redefining middle mappingend last to %u\n",middle_mappingend_last - chroffset));
	      }
#else
	      if (mappingpos > middle_mappingend_last) {
		/* Use > for NOT_GREEDY */
		middle_mappingend_last = mappingpos;
		middle_mappingend_p = true;
		debug13(printf("  Redefining middle mappingend last to %u\n",middle_mappingend_last - chroffset));
	      }
#endif

	    } else {
	      debug13b(printf("  query_lastpos %d - querypos3 %d < %d + %d, so using this diagonal\n",
			      query_lastpos,plus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = plus_segments[i].diagonal) < close_mappingend_greedy &&
		  mappingpos > genomicbound) {
		close_mappingend_greedy = mappingpos;
		close_mappingend_p = true;
		debug13(printf("  Redefining close mappingend greedy to %u\n",close_mappingend_greedy - chroffset));
	      }
	      if (mappingpos > close_mappingend_last) {
		/* Use > for NOT_GREEDY */
		close_mappingend_last = mappingpos;
		close_mappingend_p = true;
		debug13(printf("  Redefining close mappingend last to %u\n",close_mappingend_last - chroffset));
	      }
	    }
	  }

	  if (close_mappingend_p == true) {
	    close_knownsplice_limit_high = add_bounded(close_mappingend_greedy,shortsplicedist,chrhigh);
	  } else if (middle_mappingend_p == true) {
	    debug13(printf("Using middle mappingend\n"));
	    close_knownsplice_limit_high = middle_mappingend_greedy;
	    close_mappingend_greedy = middle_mappingend_greedy;
	    close_mappingend_p = true;
	  }
	  if (middle_mappingend_p == true && middle_mappingend_last > close_mappingend_greedy) {
	    knownsplice_limit_high = middle_mappingend_last;
	    mappingend = middle_mappingend_last;
	  } else if (close_mappingend_p == true && close_mappingend_last != close_mappingend_greedy) {
	    knownsplice_limit_high = add_bounded(close_mappingend_last,shortsplicedist,chrhigh);
	    mappingend = close_mappingend_last;
	  }
	  if (close_mappingend_p == false) {
	    fallback_mappingend_p = false;
	  } else if (mappingend <= close_mappingend_greedy) {
	    fallback_mappingend_p = false;
	  } else {
	    debug13(printf("Fallback mappingend = %u\n",mappingend - chroffset));
	    fallback_mappingend_p = true;
	  }
	}
      }

      favor_right_p = false;

    } else {
      chroffset = Stage3end_chroffset(hit5);
      chrhigh = Stage3end_chrhigh(hit5);
      chrlength = Stage3end_chrlength(hit5);

      if (Shortread_find_primers(queryseq5,queryseq3) == true) {
	/* Go from genomicstart */
	debug13(printf("Found primers\n"));
	genomicbound = add_bounded(Stage3end_genomicstart(hit5),querylength,chrhigh);

      } else {
	genomicbound = add_bounded(Stage3end_genomicend(hit5),querylength,chrhigh);
	
	/* TODO: Previously called Shortread_find_overlap.  Now with Shortread_max_overlap, can optimize this code */
	if ((overlap = Shortread_max_overlap(queryseq5,queryseq3)) > 0 &&
	    Stage3end_genomicbound_from_end(&genomicbound2,hit5,overlap,chroffset) == true) {
	  debug13(printf("Found overlap of %d\n",overlap));
	  if (genomicbound2 > genomicbound) {
	    zero_offset = genomicbound2 - genomicbound;
	    genomicbound = genomicbound2;
	  }
	}
      }

      debug13(printf("Case 2: hit5 minus %s %u..%u (sensedir %d) => genomicbound %u\n",
		     Stage3end_hittype_string(hit5),
		     Stage3end_genomicstart(hit5) - chroffset,Stage3end_genomicend(hit5) - chroffset,
		     Stage3end_sensedir(hit5),genomicbound - chroffset));

      knownsplice_limit_high = mappingend = segmentend = genomicbound;
      knownsplice_limit_low = subtract_bounded(Stage3end_genomicend(hit5),pairmax + shortsplicedist,chroffset);
      segmentstart = subtract_bounded(Stage3end_genomicend(hit5),pairmax,chroffset);
#ifdef LONG_ENDSPLICES
      mappingstart = subtract_bounded(Stage3end_genomicend(hit5),pairmax + shortsplicedist,chroffset);
#else
      mappingstart = subtract_bounded(Stage3end_genomicend(hit5),pairmax + shortsplicedist_novelend,chroffset);
#endif
      debug13(printf("Original bounds: knownsplice_limit_low %u, knownsplice_limit_high %u, mappingstart %u\n",
		     knownsplice_limit_low - chroffset,knownsplice_limit_high - chroffset,mappingstart - chroffset));

      close_mappingstart_last = middle_mappingstart_last = Stage3end_genomicend(hit5);
      close_mappingstart_greedy = middle_mappingstart_greedy = segmentstart;

      if (minus_nsegments > 0) {
	/* Use segments to bound */
	debug13(printf("Finding segments from segmentstart %u to segmentend %u (minus_nsegments %d)\n",
		       segmentstart - chroffset,segmentend - chroffset,minus_nsegments));
	starti = endi = -1;
	i = binary_search_segments(0,minus_nsegments-1,minus_segments,segmentend);
	while (i >= 0 && minus_segments[i].diagonal >= segmentend) {
	  i--;
	}
	starti = i;
	while (i >= 0 && minus_segments[i].diagonal > segmentstart) {
	  if (minus_segments[i].diagonal < (Univcoord_T) -1) {
	    endi = i;
	  }
	  i--;
	}
	if (starti >= 0 && endi >= 0) {
	  debug13(printf("starti = %d, endi = %d\n",starti,endi));
	  assert(starti >= endi);
	  for (i = starti; i >= endi; i--) {
	    debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			   (Chrpos_T) (minus_segments[i].diagonal - chroffset),minus_segments[i].diagonal,
			   minus_segments[i].querypos5,minus_segments[i].querypos3));
	    if (query_lastpos - minus_segments[i].querypos3 >= STAGE2_MIN_OLIGO + index1interval) {
	      /* Case 2. Missing end of query, so there could be a middle splice */
	      debug13b(printf("  query_lastpos %d - querypos3 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			      query_lastpos,minus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = subtract_bounded(minus_segments[i].diagonal,querylength + shortsplicedist_novelend,chroffset)) > middle_mappingstart_greedy &&
		  mappingpos < genomicbound) {
		middle_mappingstart_greedy = mappingpos;
		middle_mappingstart_p = true;
		debug13(printf("  Redefining middle mappingstart greedy to %u\n",middle_mappingstart_greedy - chroffset));
	      }
#ifdef LONG_ENDSPLICES
	      if ((mappingpos = subtract_bounded(minus_segments[i].diagonal,querylength + shortsplicedist,chroffset)) < middle_mappingstart_last) {
		/* Use < for NOT_GREEDY */
		middle_mappingstart_last = mappingpos;
		middle_mappingstart_p = true;
		debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	      }
#else
	      if (mappingpos < middle_mappingstart_last) {
		/* Use < for NOT_GREEDY */
		middle_mappingstart_last = mappingpos;
		middle_mappingstart_p = true;
		debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	      }
#endif

	    } else {
	      debug13b(printf("  query_lastpos %d - querypos3 %d < %d + %d, so using this diagonal\n",
			      query_lastpos,minus_segments[i].querypos3,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = subtract_bounded(minus_segments[i].diagonal,querylength,chroffset)) > close_mappingstart_greedy &&
		  mappingpos < genomicbound) {
		close_mappingstart_greedy = mappingpos;
		close_mappingstart_p = true;
		debug13(printf("  Redefining close mappingstart greedy to %u\n",close_mappingstart_greedy - chroffset));
	      }
	      if (mappingpos > close_mappingstart_last) {
		/* Use < for NOT_GREEDY */
		close_mappingstart_last = mappingpos;
		close_mappingstart_p = true;
		debug13(printf("  Redefining close mappingstart last to %u\n",close_mappingstart_last - chroffset));
	      }
	    }
	  }

	  if (close_mappingstart_p == true) {
	    close_knownsplice_limit_low = subtract_bounded(close_mappingstart_greedy,shortsplicedist,chroffset);
	  } else if (middle_mappingstart_p == true) {
	    debug13(printf("Using middle mappingstart\n"));
	    close_knownsplice_limit_low = middle_mappingstart_greedy;
	    close_mappingstart_greedy = middle_mappingstart_greedy;
	    close_mappingstart_p = true;
	  }
	  if (middle_mappingstart_p == true && middle_mappingstart_last < close_mappingstart_greedy) {
	    knownsplice_limit_low = middle_mappingstart_last;
	    mappingstart = middle_mappingstart_last;
	  } else if (close_mappingstart_p == true && close_mappingstart_last != close_mappingstart_greedy) {
	    knownsplice_limit_low = subtract_bounded(close_mappingstart_last,shortsplicedist,chroffset);
	    mappingstart = close_mappingstart_last;
	  }
	  if (close_mappingstart_p == false) {
	    fallback_mappingstart_p = false;
	  } else if (mappingstart >= close_mappingstart_greedy) {
	    fallback_mappingstart_p = false;
	  } else {
	    debug13(printf("Fallback mappingstart = %u\n",mappingstart - chroffset));
	    fallback_mappingstart_p = true;
	  }
	}
      }

      favor_right_p = false;
    }

    if ((sensedir = Stage3end_sensedir_nonamb(hit5)) == SENSE_FORWARD) {
      sense_try = +1;
    } else if (sensedir == SENSE_ANTI) {
      sense_try = -1;
    } else {
      sense_try = 0;
    }

  } else if (hit5 == NULL) {
    if ((chrnum = Stage3end_chrnum(hit3)) == 0) {
      /* Translocation */
      return (List_T) NULL;

    } else if ((watsonp = Stage3end_plusp(hit3)) == true) {
      chroffset = Stage3end_chroffset(hit3);
      chrhigh = Stage3end_chrhigh(hit3);
      chrlength = Stage3end_chrlength(hit3);

      if (Shortread_find_primers(queryseq5,queryseq3) == true) {
	/* Go from genomicend */
	debug13(printf("Found primers\n"));
	genomicbound = add_bounded(Stage3end_genomicend(hit3),querylength,chrhigh);

      } else {
	genomicbound = add_bounded(Stage3end_genomicstart(hit3),querylength,chrhigh);

	/* TODO: Previously called Shortread_find_overlap.  Now with Shortread_max_overlap, can optimize this code */
	if ((overlap = Shortread_max_overlap(queryseq5,queryseq3)) > 0 &&
	    Stage3end_genomicbound_from_start(&genomicbound2,hit3,overlap,chroffset) == true) {
	  debug13(printf("Found overlap of %d\n",overlap));
	  if (genomicbound2 > genomicbound) {
	    zero_offset = genomicbound2 - genomicbound;
	    genomicbound = genomicbound2;
	  }
	}
      }

      debug13(printf("Case 3: hit3 plus %s %u..%u (sensedir %d) => genomicbound %u\n",
		     Stage3end_hittype_string(hit3),
		     Stage3end_genomicstart(hit3) - chroffset,Stage3end_genomicend(hit3) - chroffset,
		     Stage3end_sensedir(hit3),genomicbound - chroffset));

      knownsplice_limit_high = mappingend = segmentend = genomicbound;
      knownsplice_limit_low = subtract_bounded(Stage3end_genomicstart(hit3),pairmax + shortsplicedist,chroffset);
      segmentstart = subtract_bounded(Stage3end_genomicstart(hit3),pairmax,chroffset);
#ifdef LONG_ENDSPLICES
      mappingstart = subtract_bounded(Stage3end_genomicstart(hit3),pairmax + shortsplicedist,chroffset);
#else
      mappingstart = subtract_bounded(Stage3end_genomicstart(hit3),pairmax + shortsplicedist_novelend,chroffset);
#endif

      close_mappingstart_last = middle_mappingstart_last = Stage3end_genomicstart(hit3);
      close_mappingstart_greedy = middle_mappingstart_greedy = segmentstart;

      if (plus_nsegments > 0) {
	/* Use segments to bound */
	debug13(printf("Finding segments from segmentstart %u to segmentend %u (plus_nsegments %d)\n",
		       segmentstart - chroffset,segmentend - chroffset,plus_nsegments));
	starti = endi = -1;
	i = binary_search_segments(0,plus_nsegments-1,plus_segments,segmentend);
	while (i >= 0 && plus_segments[i].diagonal >= segmentend) {
	  i--;
	}
	starti = i;
	while (i >= 0 && plus_segments[i].diagonal > segmentstart) {
	  if (plus_segments[i].diagonal < (Univcoord_T) -1) {
	    endi = i;
	  }
	  i--;
	}
	if (starti >= 0 && endi >= 0) {
	  debug13(printf("starti = %d, endi = %d\n",starti,endi));
	  assert(starti >= endi);
	  for (i = starti; i >= endi; i--) {
	    debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			   (Chrpos_T) (plus_segments[i].diagonal - chroffset),plus_segments[i].diagonal,
			   plus_segments[i].querypos5,plus_segments[i].querypos3));
	    if (plus_segments[i].querypos5 >= STAGE2_MIN_OLIGO + index1interval) {
	      /* Case 3. Missing start of query, so there could be a middle splice */
	      debug13b(printf("  querypos5 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			      plus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = subtract_bounded(plus_segments[i].diagonal,querylength + shortsplicedist_novelend,chroffset)) > middle_mappingstart_greedy &&
		  mappingpos < genomicbound) {
		middle_mappingstart_greedy = mappingpos;
		middle_mappingstart_p = true;
		debug13(printf("  Redefining middle mappingstart greedy to %u\n",middle_mappingstart_greedy - chroffset));
	      }
#ifdef LONG_ENDSPLICES
	      if ((mappingpos = subtract_bounded(plus_segments[i].diagonal,querylength + shortsplicedist,chroffset)) < middle_mappingstart_last) {
		/* Use < for NOT_GREEDY */
		middle_mappingstart_last = mappingpos;
		middle_mappingstart_p = true;
		debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	      }
#else
	      if (mappingpos < middle_mappingstart_last) {
		/* Use < for NOT_GREEDY */
		middle_mappingstart_last = mappingpos;
		middle_mappingstart_p = true;
		debug13(printf("  Redefining middle mappingstart last to %u\n",middle_mappingstart_last - chroffset));
	      }
#endif

	    } else {
	      debug13b(printf("  querypos5 %d < %d + %d, so using this diagonal\n",
			      plus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = subtract_bounded(plus_segments[i].diagonal,querylength,chroffset)) > close_mappingstart_greedy &&
		  mappingpos < genomicbound) {
		close_mappingstart_greedy = mappingpos;
		close_mappingstart_p = true;
		debug13(printf("  Redefining close mappingstart greedy to %u\n",close_mappingstart_greedy - chroffset));
	      }
	      if (mappingpos < close_mappingstart_last) {
		/* Use < for NOT_GREEDY */
		close_mappingstart_last = mappingpos;
		close_mappingstart_p = true;
		debug13(printf("  Redefining close mappingstart last to %u\n",close_mappingstart_last - chroffset));
	      }
	    }
	  }

	  if (close_mappingstart_p == true) {
	    close_knownsplice_limit_low = subtract_bounded(close_mappingstart_greedy,shortsplicedist,chroffset);
	  } else if (middle_mappingstart_p == true) {
	    debug13(printf("Using middle mappingstart\n"));
	    close_knownsplice_limit_low = middle_mappingstart_greedy;
	    close_mappingstart_greedy = middle_mappingstart_greedy;
	    close_mappingstart_p = true;
	  }
	  if (middle_mappingstart_p == true && middle_mappingstart_last < close_mappingstart_greedy) {
	    knownsplice_limit_low = middle_mappingstart_last;
	    mappingstart = middle_mappingstart_last;
	  } else if (close_mappingstart_p == true && close_mappingstart_last != close_mappingstart_greedy) {
	    knownsplice_limit_low = subtract_bounded(close_mappingstart_last,shortsplicedist,chroffset);
	    mappingstart = close_mappingstart_last;
	  }
	  if (close_mappingstart_p == false) {
	    fallback_mappingstart_p = false;
	  } else if (mappingstart >= close_mappingstart_greedy) {
	    fallback_mappingstart_p = false;
	  } else {
	    debug13(printf("Fallback mappingstart = %u\n",mappingstart - chroffset));
	    fallback_mappingstart_p = true;
	  }
	}
      }

      favor_right_p = true;

    } else {
      chroffset = Stage3end_chroffset(hit3);
      chrhigh = Stage3end_chrhigh(hit3);
      chrlength = Stage3end_chrlength(hit3);

      if (Shortread_find_primers(queryseq5,queryseq3) == true) {
	/* Go from genomicend */
	debug13(printf("Found primers\n"));
	genomicbound = subtract_bounded(Stage3end_genomicend(hit3),querylength,chroffset);

      } else {
	genomicbound = subtract_bounded(Stage3end_genomicstart(hit3),querylength,chroffset);

	/* TODO: Previously called Shortread_find_overlap.  Now with Shortread_max_overlap, can optimize this code */
	if ((overlap = Shortread_max_overlap(queryseq5,queryseq3)) > 0 &&
	    Stage3end_genomicbound_from_start(&genomicbound2,hit3,overlap,chroffset) == true) {
	  debug13(printf("Found overlap of %d\n",overlap));
	  if (genomicbound2 < genomicbound) {
	    zero_offset = genomicbound - genomicbound2;
	    genomicbound = genomicbound2;
	  }
	}
      }

      debug13(printf("Case 4: hit3 minus %s %u..%u (sensedir %d) => genomicbound %u\n",
		     Stage3end_hittype_string(hit3),
		     Stage3end_genomicstart(hit3) - chroffset,Stage3end_genomicend(hit3) - chroffset,
		     Stage3end_sensedir(hit3),genomicbound - chroffset));

      knownsplice_limit_low = mappingstart = segmentstart = genomicbound;
      knownsplice_limit_high = add_bounded(Stage3end_genomicstart(hit3),pairmax + shortsplicedist,chrhigh);
      segmentend = add_bounded(Stage3end_genomicstart(hit3),pairmax,chrhigh);
#ifdef LONG_ENDSPLICES
      mappingend = add_bounded(Stage3end_genomicstart(hit3),pairmax + shortsplicedist,chrhigh);
#else
      mappingend = add_bounded(Stage3end_genomicstart(hit3),pairmax + shortsplicedist_novelend,chrhigh);
#endif

      close_mappingend_last = middle_mappingend_last = Stage3end_genomicstart(hit3);
      close_mappingend_greedy = middle_mappingend_greedy = segmentend;

      if (minus_nsegments > 0) {
	/* Use segments to bound */
	debug13(printf("Finding segments from segmentstart %u to segmentend %u (minus_nsegments %d)\n",
		       segmentstart - chroffset,segmentend - chroffset,minus_nsegments));
	starti = endi = -1;
	i = binary_search_segments(0,minus_nsegments-1,minus_segments,segmentstart);
	while (i < minus_nsegments - 1 && minus_segments[i].diagonal == (Univcoord_T) -1) {
	  i++;
	}
	starti = i;
	while (minus_segments[i].diagonal < segmentend) {
	  endi = i;
	  i++;
	}
	if (starti >= 0 && endi >= 0) {
	  debug13(printf("starti = %d, endi = %d\n",starti,endi));
	  assert(starti <= endi);
	  for (i = starti; i <= endi; i++) {
	    debug13(printf("diagonal %u (%lu), querypos %d..%d\n",
			   (Chrpos_T) (minus_segments[i].diagonal - chroffset),minus_segments[i].diagonal,
			   minus_segments[i].querypos5,minus_segments[i].querypos3));
	    if (minus_segments[i].querypos5 >= STAGE2_MIN_OLIGO + index1interval) {
	      /* Case 4. Missing start of query, so there could be a middle splice */
	      debug13b(printf("  querypos5 %d >= %d + %d, so using this diagonal plus shortsplicedist\n",
			      minus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = add_bounded(minus_segments[i].diagonal,shortsplicedist_novelend,chrhigh)) < middle_mappingend_greedy &&
		  mappingpos > genomicbound) {
		middle_mappingend_greedy = mappingpos;
		middle_mappingend_p = true;
		debug13(printf("  Redefining middle mappingend greedy to %u\n",middle_mappingend_greedy - chroffset));
	      }
#ifdef LONG_ENDSPLICES
	      if ((mappingpos = add_bounded(minus_segments[i].diagonal,shortsplicedist,chrhigh)) > middle_mappingend_last) {
		/* Use > for NOT_GREEDY */
		middle_mappingend_last = mappingpos;
		middle_mappingend_p = true;
		debug13(printf("  Redefining middle mappingend to %u\n",middle_mappingend_last - chroffset));
	      }
#else
	      if (mappingpos > middle_mappingend_last) {
		/* Use > for NOT_GREEDY */
		middle_mappingend_last = mappingpos;
		middle_mappingend_p = true;
		debug13(printf("  Redefining middle mappingend to %u\n",middle_mappingend_last - chroffset));
	      }
#endif

	    } else {
	      debug13b(printf("  querypos5 %d < %d + %d, so using this diagonal\n",
			      minus_segments[i].querypos5,STAGE2_MIN_OLIGO,index1interval));
	      if ((mappingpos = minus_segments[i].diagonal) < close_mappingend_greedy &&
		  mappingpos > genomicbound) {
		close_mappingend_greedy = mappingpos;
		close_mappingend_p = true;
		debug13(printf("  Redefining close mappingend greedy to %u\n",close_mappingend_greedy - chroffset));
	      }
	      if (mappingpos > close_mappingend_last) {
		/* Use > for NOT_GREEDY */
		close_mappingend_last = mappingpos;
		close_mappingend_p = true;
		debug13(printf("  Redefining close mappingend last to %u\n",close_mappingend_last - chroffset));
	      }
	    }
	  }

	  if (close_mappingend_p == true) {
	    close_knownsplice_limit_high = add_bounded(close_mappingend_greedy,shortsplicedist,chrhigh);
	  } else if (middle_mappingend_p == true) {
	    debug13(printf("Using middle mappingend\n"));
	    close_knownsplice_limit_high = middle_mappingend_greedy;
	    close_mappingend_greedy = middle_mappingend_greedy;
	    close_mappingend_p = true;
	  }
	  if (middle_mappingend_p == true && middle_mappingend_last > close_mappingend_greedy) {
	    knownsplice_limit_high = middle_mappingend_last;
	    mappingend = middle_mappingend_last;
	  } else if (close_mappingend_p == true && close_mappingend_last != close_mappingend_greedy) {
	    knownsplice_limit_high = add_bounded(close_mappingend_last,shortsplicedist,chrhigh);
	    mappingend = close_mappingend_last;
	  }
	  if (close_mappingend_p == false) {
	    fallback_mappingend_p = false;
	  } else if (mappingend <= close_mappingend_greedy) {
	    fallback_mappingend_p = false;
	  } else {
	    debug13(printf("Fallback mappingend = %u\n",mappingend - chroffset));
	    fallback_mappingend_p = true;
	  }
	}
      }

      favor_right_p = true;
    }

    if ((sensedir = Stage3end_sensedir_nonamb(hit3)) == SENSE_FORWARD) {
      sense_try = +1;
    } else if (sensedir == SENSE_ANTI) {
      sense_try = -1;
    } else {
      sense_try = 0;
    }

  } else {
    abort();
  }

#ifdef OLD_GENOMICBOUND
  knownsplice_limit_low = genomicstart + querylength;
  knownsplice_limit_high = genomicend - querylength;
#endif

  if (close_mappingstart_p == true && close_mappingend_p == true) {
    debug13(printf("Halfmapping: Running gmap with close mappingstart and close mappingend\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
		    hits,queryuc_ptr,querylength,sense_try,favor_right_p,
		    /*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
		    query_compress_fwd,query_compress_rev,close_mappingstart_greedy,close_mappingend_greedy,
		    close_knownsplice_limit_low,close_knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);

    if (good_start_p == true && good_end_p == true) {
      /* Success */
    } else if (gmap_rerun_p == false) {
      debug13(printf("Skipping re-run of gmap\n"));
    } else if (/* require both ends to be good */ 0 && good_start_p == true) {
      if (fallback_mappingend_p == true) {
	debug13(printf("Halfmapping: Re-running gmap with close mappingstart only\n"));
	hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
			hits,queryuc_ptr,querylength,sense_try,favor_right_p,
			/*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
			query_compress_fwd,query_compress_rev,close_mappingstart_greedy,mappingend,
			close_knownsplice_limit_low,knownsplice_limit_high,
			watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
			oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
      }

    } else if (/* require both ends to be good */ 0 && good_end_p == true) {
      if (fallback_mappingstart_p == true) {
	debug13(printf("Halfmapping: Re-running gmap with close mappingend only\n"));
	hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
			hits,queryuc_ptr,querylength,sense_try,favor_right_p,
			/*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
			query_compress_fwd,query_compress_rev,mappingstart,close_mappingend_greedy,
			knownsplice_limit_low,close_knownsplice_limit_high,
			watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
			oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
      }
    } else {
      if (fallback_mappingstart_p == true && fallback_mappingend_p == true) {
	debug13(printf("Halfmapping: Re-running gmap with far mappingstart and mappingend\n"));
	hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
			hits,queryuc_ptr,querylength,sense_try,favor_right_p,
			/*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
			query_compress_fwd,query_compress_rev,mappingstart,mappingend,
			knownsplice_limit_low,knownsplice_limit_high,
			watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
			oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
      }
    }

  } else if (close_mappingstart_p == true) {
    debug13(printf("Halfmapping: Running gmap with close mappingstart\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
		    hits,queryuc_ptr,querylength,sense_try,favor_right_p,
		    /*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
		    query_compress_fwd,query_compress_rev,close_mappingstart_greedy,mappingend,
		    close_knownsplice_limit_low,knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);

    if (good_start_p == true && /* require both ends to be good */ good_end_p == true) {
      /* Success */
    } else if (gmap_rerun_p == false) {
      debug13(printf("Skipping re-run of gmap\n"));
    } else if (fallback_mappingstart_p == true) {
      debug13(printf("Halfmapping: Re-running gmap with far mappingstart\n"));
      hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
		      hits,queryuc_ptr,querylength,sense_try,favor_right_p,
		      /*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
		      query_compress_fwd,query_compress_rev,mappingstart,mappingend,
		      knownsplice_limit_low,knownsplice_limit_high,
		      watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		      oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		      pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
    }

  } else if (close_mappingend_p == true) {
    debug13(printf("Halfmapping: Running gmap with close mappingend\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
		    hits,queryuc_ptr,querylength,sense_try,favor_right_p,
		    /*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
		    query_compress_fwd,query_compress_rev,mappingstart,close_mappingend_greedy,
		    knownsplice_limit_low,close_knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);

    if (good_end_p == true && /* require both ends to be good */ good_start_p == true) {
      /* Success */
    } else if (gmap_rerun_p == false) {
      debug13(printf("Skipping re-run of gmap\n"));
    } else if (fallback_mappingend_p == true) {
      debug13(printf("Halfmapping: Re-running gmap with far mappingend\n"));
      hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
		      hits,queryuc_ptr,querylength,sense_try,favor_right_p,
		      /*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
		      query_compress_fwd,query_compress_rev,mappingstart,mappingend,
		      knownsplice_limit_low,knownsplice_limit_high,
		      watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		      oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		      pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
    }

  } else {
    debug13(printf("Halfmapping: Running gmap with far mappingstart and mappingend\n"));
    hits = run_gmap(&good_start_p,&good_end_p,gmap_history,
		    hits,queryuc_ptr,querylength,sense_try,favor_right_p,
		    /*paired_favor_mode*/favor_right_p == true ? +1 : -1,zero_offset,
		    query_compress_fwd,query_compress_rev,mappingstart,mappingend,
		    knownsplice_limit_low,knownsplice_limit_high,
		    watsonp,genestrand,chrnum,chroffset,chrhigh,chrlength,
		    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
		    pairpool,diagpool,dynprogL,dynprogM,dynprogR,user_maxlevel);
  }

  return hits;
}


static List_T
align_pair_with_gmap (Pairtype_T *final_pairtype, List_T result,
		      History_T gmap_history_5, History_T gmap_history_3,
		      Compress_T query5_compress_fwd, Compress_T query5_compress_rev,
		      Compress_T query3_compress_fwd, Compress_T query3_compress_rev,
		      struct Segment_T **plus_segments_genestrand_5, int *plus_nsegments_genestrand_5,
		      struct Segment_T **minus_segments_genestrand_5, int *minus_nsegments_genestrand_5,
		      struct Segment_T **plus_segments_genestrand_3, int *plus_nsegments_genestrand_3,
		      struct Segment_T **minus_segments_genestrand_3, int *minus_nsegments_genestrand_3,
		      Shortread_T queryseq5, Shortread_T queryseq3,
		      char *queryuc_ptr_5, char *queryuc_ptr_3,
		      int querylength5, int querylength3, int query5_lastpos, int query3_lastpos,
		      int localsplicing_penalty,
		      Oligoindex_T *oligoindices_major, int noligoindices_major,
		      Oligoindex_T *oligoindices_minor, int noligoindices_minor,
		      Pairpool_T pairpool, Diagpool_T diagpool,
		      Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		      int pairmax, int user_maxlevel_5, int user_maxlevel_3,
		      int cutoff_level_5, int cutoff_level_3,
		      Pairtype_T pairtype, bool expect_concordant_p, bool redo_for_sense_p) {
  Stage3pair_T newpair, stage3pair;
  List_T gmap5_hits = NULL, gmap3_hits = NULL, good_gmap5_hits = NULL, good_gmap3_hits = NULL;
  Stage3end_T hit5, hit3, gmap5, gmap3;
  List_T p, a, b, rest;
  int genestrand;
  int missing_hit, missing_gmap;
  int i;
  bool replacedp;


  debug13(printf("Sorting hitpairs by nmatches\n"));
  result = Stage3pair_sort_bymatches(result);

  for (p = result, i = 0; p != NULL && i < max_gmap_improvement; p = p->rest, i++) {
    stage3pair = (Stage3pair_T) List_head(p);
    genestrand = Stage3pair_genestrand(stage3pair);
    hit5 = Stage3pair_hit5(stage3pair);
    hit3 = Stage3pair_hit3(stage3pair);
    gmap5 = gmap3 = (Stage3end_T) NULL;

    debug13(printf("GMAP improvement #%d: Entering align_pair_with_gmap with hittypes %s and %s\n",
		   i,Stage3end_hittype_string(hit5),Stage3end_hittype_string(hit3)));

    /* Was querylength5 - Stage3end_matches(hit5) > 5 */
    debug13(printf("**Looking at hit5\n"));
    if (Stage3end_sarrayp(hit5) == true && redo_for_sense_p == false) {
      /* Skip */
      debug13(printf("Skipping hit5 from sarray search\n"));

    } else if (Stage3end_hittype(hit5) == GMAP && redo_for_sense_p == false) {
      /* Skip */
      debug13(printf("Skipping hit5 of type GMAP\n"));

#if 0
      /* Don't skip on final align_concordant_with_gmap */
    } else if (Stage3end_hittype(hit5) == TERMINAL) {
      /* Skip */
      debug13(printf("Skipping hit5 of type TERMINAL\n"));
#endif

    } else if (querylength5 - Stage3end_nmatches(hit5) <= user_maxlevel_5) {
      /* Skip */
      debug13(printf("Skipping hit with nmismatches %d - %d <= user_maxlevel %d\n",
		     querylength5,Stage3end_nmatches(hit5),user_maxlevel_5));

    } else if (expect_concordant_p == false) {
      debug13(printf("expect_concordant_p is false, so running GMAP single end on 5'\n"));
      gmap5_hits = align_single_hit_with_gmap(gmap_history_5,hit5,
					      /*extend_left_p*/true,/*extend_right_p*/true,
					      queryuc_ptr_5,querylength5,query5_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
					      queryrc5,Shortread_invertedp(queryseq5),
#endif
					      query5_compress_fwd,query5_compress_rev,
					      plus_segments_genestrand_5[genestrand],
					      plus_nsegments_genestrand_5[genestrand],
					      minus_segments_genestrand_5[genestrand],
					      minus_nsegments_genestrand_5[genestrand],
					      oligoindices_major,noligoindices_major,
					      oligoindices_minor,noligoindices_minor,
					      pairpool,diagpool,dynprogL,dynprogM,dynprogR,
					      user_maxlevel_5,genestrand);

      missing_hit = querylength5 - Stage3end_nmatches(hit5);
      for (b = gmap5_hits; b != NULL; b = List_next(b)) {
	gmap5 = (Stage3end_T) List_head(b);
	missing_gmap = querylength5 - Stage3end_nmatches(gmap5);
	if (Stage3end_score(gmap5) > cutoff_level_5 + gmap_allowance) {
	  debug13(printf("Score is only %d vs cutoff_level_5 %d\n",Stage3end_score(gmap5),cutoff_level_5));
	  Stage3end_free(&gmap5);
	} else if (missing_gmap < missing_hit/2) {
	  debug13(printf("GMAP with %d matches, %d missing is significantly better than 5' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap5),missing_gmap,Stage3end_nmatches(hit5),missing_hit));
	  good_gmap5_hits = List_push(good_gmap5_hits,(void *) gmap5);
	  Stage3end_set_improved_by_gmap(hit5);
	} else {
	  debug13(printf("GMAP with %d matches, %d missing is not significantly better than 5' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap5),missing_gmap,Stage3end_nmatches(hit5),missing_hit));
	  Stage3end_free(&gmap5);
	}
      }
      List_free(&gmap5_hits);

    } else if ((redo_for_sense_p == true && Stage3end_sensedir(hit5) == 0) ||
	       Stage3end_terminal_trim(hit5) > GMAP_TERMINAL_TRIM ||
	       Stage3end_contains_known_splicesite(hit5) == true) {
      debug13(printf("To correct hit5 terminalp %d or known_splicesite %d, running GMAP on 5' to match with 3' end\n",
		     Stage3end_hittype(hit5) == TERMINAL,
		     Stage3end_contains_known_splicesite(hit5)));
    
      /* Want high quality because we already have a pretty good answer */
      gmap5_hits = align_halfmapping_with_gmap(gmap_history_5,/*hit5*/NULL,hit3,
					       queryseq5,queryseq3,
					       queryuc_ptr_5,/*querylength*/querylength5,query5_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
					       queryrc5,Shortread_invertedp(queryseq5),
#endif
					       query5_compress_fwd,query5_compress_rev,
					       plus_segments_genestrand_5[genestrand],
					       plus_nsegments_genestrand_5[genestrand],
					       minus_segments_genestrand_5[genestrand],
					       minus_nsegments_genestrand_5[genestrand],
					       oligoindices_major,noligoindices_major,
					       oligoindices_minor,noligoindices_minor,
					       pairpool,diagpool,dynprogL,dynprogM,dynprogR,
					       pairmax,shortsplicedist,user_maxlevel_5,genestrand);

      missing_hit = querylength5 - Stage3end_nmatches(hit5);
      for (a = gmap5_hits; a != NULL; a = List_next(a)) {
	gmap5 = (Stage3end_T) List_head(a);
	missing_gmap = querylength5 - Stage3end_nmatches(gmap5);
	if (Stage3end_score(gmap5) > cutoff_level_5 + gmap_allowance) {
	  debug13(printf("Score is only %d vs cutoff_level_5 %d\n",Stage3end_score(gmap5),cutoff_level_5));
	  Stage3end_free(&gmap5);
	} else if (redo_for_sense_p == true && Stage3end_sensedir(hit5) == 0) {
	  debug13(printf("redo_for_sense, so using this one\n"));
	  good_gmap5_hits = List_push(good_gmap5_hits,(void *) gmap5);
	} else if (missing_gmap < missing_hit/2) {
	  debug13(printf("GMAP with %d matches, %d missing is significantly better than 5' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap5),missing_gmap,Stage3end_nmatches(hit5),missing_hit));
	  good_gmap5_hits = List_push(good_gmap5_hits,(void *) gmap5);
	  Stage3end_set_improved_by_gmap(hit5);
	} else {
	  debug13(printf("GMAP with %d matches, %d missing is not significantly better than 5' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap5),missing_gmap,Stage3end_nmatches(hit5),missing_hit));
	  Stage3end_free(&gmap5);
	}
      }
      List_free(&gmap5_hits);
    }

    debug13(printf("**Looking at hit3\n"));
    if (Stage3end_sarrayp(hit3) == true && redo_for_sense_p == false) {
      /* Skip */
      debug13(printf("Skipping hit3 from sarray search\n"));

    } else if (Stage3end_hittype(hit3) == GMAP && redo_for_sense_p == false) {
      /* Skip */
      debug13(printf("Skipping hit3 of type GMAP\n"));

#if 0
      /* Don't skip on final align_concordant_with_gmap */
    } else if (Stage3end_hittype(hit3) == TERMINAL) {
      /* Skip */
      debug13(printf("Skipping hit3 of type TERMINAL\n"));
#endif

    } else if (querylength3 - Stage3end_nmatches(hit3) <= user_maxlevel_3) {
      /* Skip */
      debug13(printf("Skipping hit3 with nmismatches %d - %d <= user_maxlevel %d\n",
		     querylength3,Stage3end_nmatches(hit3),user_maxlevel_3));

    } else if (expect_concordant_p == false) {
      debug13(printf("expect_concordant_p is false, so running GMAP single end on 3'\n"));
      gmap3_hits = align_single_hit_with_gmap(gmap_history_3,hit3,
					      /*extend_left_p*/true,/*extend_right_p*/true,
					      queryuc_ptr_3,querylength3,query3_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
					      queryrc3,Shortread_invertedp(queryseq3),
#endif
					      query3_compress_fwd,query3_compress_rev,
					      plus_segments_genestrand_3[genestrand],
					      plus_nsegments_genestrand_3[genestrand],
					      minus_segments_genestrand_3[genestrand],
					      minus_nsegments_genestrand_3[genestrand],
					      oligoindices_major,noligoindices_major,
					      oligoindices_minor,noligoindices_minor,
					      pairpool,diagpool,dynprogL,dynprogM,dynprogR,
					      user_maxlevel_3,genestrand);

      missing_hit = querylength3 - Stage3end_nmatches(hit3);
      for (b = gmap3_hits; b != NULL; b = List_next(b)) {
	gmap3 = (Stage3end_T) List_head(b);
	missing_gmap = querylength3 - Stage3end_nmatches(gmap3);
	if (Stage3end_score(gmap3) > cutoff_level_3 + gmap_allowance) {
	  debug13(printf("Score is only %d vs cutoff_level_3 %d\n",Stage3end_score(gmap3),cutoff_level_3));
	  Stage3end_free(&gmap3);
	} else if (missing_gmap < missing_hit/2) {
	  debug13(printf("GMAP with %d matches, %d missing is significantly better than 3' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap3),missing_gmap,Stage3end_nmatches(hit3),missing_hit));
	  good_gmap3_hits = List_push(good_gmap3_hits,(void *) gmap3);
	  Stage3end_set_improved_by_gmap(hit3);
	} else {
	  debug13(printf("GMAP with %d matches, %d missing is not significantly better than 3' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap3),missing_gmap,Stage3end_nmatches(hit3),missing_hit));
	  Stage3end_free(&gmap3);
	}
      }
      List_free(&gmap3_hits);

    } else if ((redo_for_sense_p == true && Stage3end_sensedir(hit3) == 0) ||
	       Stage3end_terminal_trim(hit3) > GMAP_TERMINAL_TRIM ||
	       Stage3end_contains_known_splicesite(hit3) == true) {
      debug13(printf("To correct hit3 terminal %d or known_splicesite %d, running GMAP on 3' to match with 5' end\n",
		     Stage3end_hittype(hit3) == TERMINAL,
		     Stage3end_contains_known_splicesite(hit3)));

      /* Want high quality because we already have a pretty good answer */
      gmap3_hits = align_halfmapping_with_gmap(gmap_history_3,hit5,/*hit3*/NULL,
					       queryseq5,queryseq3,
					       queryuc_ptr_3,/*querylength*/querylength3,query3_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
					       queryrc3,Shortread_invertedp(queryseq3),
#endif
					       query3_compress_fwd,query3_compress_rev,
					       plus_segments_genestrand_3[genestrand],
					       plus_nsegments_genestrand_3[genestrand],
					       minus_segments_genestrand_3[genestrand],
					       minus_nsegments_genestrand_3[genestrand],
					       oligoindices_major,noligoindices_major,
					       oligoindices_minor,noligoindices_minor,
					       pairpool,diagpool,dynprogL,dynprogM,dynprogR,
					       pairmax,shortsplicedist,user_maxlevel_3,genestrand);

      missing_hit = querylength3 - Stage3end_nmatches(hit3);
      for (b = gmap3_hits; b != NULL; b = List_next(b)) {
	gmap3 = (Stage3end_T) List_head(b);
	missing_gmap = querylength3 - Stage3end_nmatches(gmap3);
	if (Stage3end_score(gmap3) > cutoff_level_3 + gmap_allowance) {
	  debug13(printf("Score is only %d vs cutoff_level_3 %d\n",Stage3end_score(gmap3),cutoff_level_3));
	  Stage3end_free(&gmap3);
	} else if (redo_for_sense_p == true && Stage3end_sensedir(hit3) == 0) {
	  debug13(printf("redo_for_sense, so using this one\n"));
	  good_gmap3_hits = List_push(good_gmap3_hits,(void *) gmap3);
	} else if (missing_gmap < missing_hit/2) {
	  debug13(printf("GMAP with %d matches, %d missing is significantly better than 3' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap3),missing_gmap,Stage3end_nmatches(hit3),missing_hit));
	  good_gmap3_hits = List_push(good_gmap3_hits,(void *) gmap3);
	  Stage3end_set_improved_by_gmap(hit3);
	} else {
	  debug13(printf("GMAP with %d matches, %d missing is not significantly better than 3' hit with %d matches, %d missing\n",
			 Stage3end_nmatches(gmap3),missing_gmap,Stage3end_nmatches(hit3),missing_hit));
	  Stage3end_free(&gmap3);
	}
      }
      List_free(&gmap3_hits);
    }

    if (good_gmap5_hits != NULL && good_gmap3_hits != NULL) {
      replacedp = false;
      for (a = good_gmap5_hits; a != NULL; a = List_next(a)) {
	gmap5 = (Stage3end_T) List_head(a);

	for (b = good_gmap3_hits; b != NULL; b = List_next(b)) {
	  gmap3 = (Stage3end_T) List_head(b);

	  debug13(printf("Imperfect concordant uniq: Double GMAP on hit5 and hit3"));
	  if ((newpair = Stage3pair_new(Stage3end_copy(gmap5),Stage3end_copy(gmap3),splicesites,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,genestrand,
					/*pairtype*/UNSPECIFIED,localsplicing_penalty,
					/*private5p*/true,/*private3p*/true,expect_concordant_p)) == NULL) {
	    /* Stage3end_free(&gmap3); -- done by Stage3pair_new */
	    /* Stage3end_free(&gmap5); -- done by Stage3pair_new */
	    debug13(printf("  => NULL\n"));

	  } else if (replacedp == false) {
	    /* Convert to gmap-gmap */
	    debug13(printf("  => replacement\n"));
	    if (Stage3pair_pairtype(newpair) == CONCORDANT) {
	      *final_pairtype = CONCORDANT;
	    }
	    List_head_set(p,(void *) newpair);
	    replacedp = true;

	  } else {
	    debug13(printf("  => addition\n"));
	    if (Stage3pair_pairtype(newpair) == CONCORDANT) {
	      *final_pairtype = CONCORDANT;
	    }
	    rest = List_push(List_next(p),(void *) newpair);
	    List_tail_set(p,rest);
	    p = rest;
	  }
	}
      }

      if (replacedp == true) {
	Stage3pair_free(&stage3pair); /* Also frees hit5 and hit3 */
      }
      for (a = good_gmap5_hits; a != NULL; a = List_next(a)) {
	gmap5 = (Stage3end_T) List_head(a);
	Stage3end_free(&gmap5);
      }
      for (b = good_gmap3_hits; b != NULL; b = List_next(b)) {
	gmap3 = (Stage3end_T) List_head(b);
	Stage3end_free(&gmap3);
      }

      List_free(&good_gmap3_hits);
      List_free(&good_gmap5_hits);

    } else {
      debug13(printf("Have %d GMAP 5' hits and %d GMAP 3' hits\n",
		     List_length(good_gmap5_hits),List_length(good_gmap3_hits)));

      /* Handle gmap5 hits */
      replacedp = false;
      for (a = good_gmap5_hits; a != NULL; a = List_next(a)) {
	gmap5 = (Stage3end_T) List_head(a);

	debug13(printf("Imperfect concordant uniq: Single GMAP on hit5"));
	if ((newpair = Stage3pair_new(gmap5,Stage3end_copy(hit3),splicesites,
				      query5_compress_fwd,query5_compress_rev,
				      query3_compress_fwd,query3_compress_rev,genestrand,
				      /*pairtype*/UNSPECIFIED,localsplicing_penalty,
				      /*private5p*/true,/*private3p*/true,expect_concordant_p)) == NULL) {
	  /* Stage3end_free(&gmap5); -- done by Stage3pair_new */
	  debug13(printf(" => NULL\n"));

	} else if (replacedp == false) {
	  /* Convert to gmap-xx */
	  debug13(printf(" => replacement\n"));
	  if (Stage3pair_pairtype(newpair) == CONCORDANT) {
	    *final_pairtype = CONCORDANT;
	  }
	  List_head_set(p,(void *) newpair);
	  replacedp = true;

	} else {
	  debug13(printf(" => addition\n"));
	  if (Stage3pair_pairtype(newpair) == CONCORDANT) {
	    *final_pairtype = CONCORDANT;
	  }
	  rest = List_push(List_next(p),(void *) newpair);
	  List_tail_set(p,rest);
	  p = rest;
	}
      }

      if (replacedp == true) {
	Stage3pair_free(&stage3pair);
      }
      /* Do not free gmap5 objects, since not copied */
      List_free(&good_gmap5_hits);


      /* Handle gmap3 hits */
      replacedp = false;
      for (b = good_gmap3_hits; b != NULL; b = List_next(b)) {
	gmap3 = (Stage3end_T) List_head(b);

	debug13(printf("Imperfect concordant uniq: Single GMAP on hit3"));
	if ((newpair = Stage3pair_new(Stage3end_copy(hit5),gmap3,splicesites,
				      query5_compress_fwd,query5_compress_rev,
				      query3_compress_fwd,query3_compress_rev,genestrand,
				      /*pairtype*/UNSPECIFIED,localsplicing_penalty,
				      /*private5p*/true,/*private3p*/true,expect_concordant_p)) == NULL) {
	  /* Stage3end_free(&gmap3); -- done by Stage3pair_new */
	  debug13(printf(" => NULL\n"));

	} else if (replacedp == false) {
	  /* Convert to xx-gmap */
	  debug13(printf(" => replacement\n"));
	  if (Stage3pair_pairtype(newpair) == CONCORDANT) {
	    *final_pairtype = CONCORDANT;
	  }
	  List_head_set(p,(void *) newpair);
	  replacedp = true;

	} else {
	  debug13(printf(" => addition\n"));
	  if (Stage3pair_pairtype(newpair) == CONCORDANT) {
	    *final_pairtype = CONCORDANT;
	  }
	  rest = List_push(List_next(p),(void *) newpair);
	  List_tail_set(p,rest);
	  p = rest;
	}
      }

      if (replacedp == true) {
	Stage3pair_free(&stage3pair);
      }
      /* Do not free gmap3 objects, since not copied */
      List_free(&good_gmap3_hits);

    }
  }

  debug13(printf("End of align_pair_with_gmap\n"));

  return result;
}


static bool
better_free_end_exists_p (List_T subs, List_T indels, List_T singlesplicing, List_T doublesplicing,
			  int querylength) {
  List_T p;
  Stage3end_T hit;
  int best_concordant_score = querylength, score;

  if ((score = Stage3end_best_score_paired(subs)) < best_concordant_score) {
    best_concordant_score = score;
  }
  if ((score = Stage3end_best_score_paired(indels)) < best_concordant_score) {
    best_concordant_score = score;
  }
  if ((score = Stage3end_best_score_paired(singlesplicing)) < best_concordant_score) {
    best_concordant_score = score;
  }
  if ((score = Stage3end_best_score_paired(doublesplicing)) < best_concordant_score) {
    best_concordant_score = score;
  }
  debug(printf("Best concordant score = %d\n",best_concordant_score));

  if (Stage3end_equiv_score_unpaired_p(subs,best_concordant_score) == true) {
    debug(printf("Better or equivalent score found in subs\n"));
    return true;
  } else if (Stage3end_equiv_score_unpaired_p(indels,best_concordant_score) == true) {
    debug(printf("Better or equivalent score found in indels\n"));
    return true;
  } else if (Stage3end_equiv_score_unpaired_p(singlesplicing,best_concordant_score) == true) {
    debug(printf("Better or equivalent score found in singlesplicing\n"));
    return true;
  } else if (Stage3end_equiv_score_unpaired_p(doublesplicing,best_concordant_score) == true) {
    debug(printf("Better or equivalent score found in doublesplicing\n"));
    return true;
  } else {
    return false;
  }
}



#define HITARRAY_SUBS 0
#define HITARRAY_INDELS 1
#define HITARRAY_SINGLESPLICING 2
#define HITARRAY_DOUBLESPLICING 3
#define HITARRAY_N 4

static List_T
align_pair (bool *abort_pairing_p, int *found_score, int *cutoff_level_5, int *cutoff_level_3,
	    List_T *samechr, List_T *conc_transloc, List_T *with_terminal,
	    History_T gmap_history_5, History_T gmap_history_3, List_T *hits5, List_T *hits3, T this5, T this3,
	    Compress_T query5_compress_fwd, Compress_T query5_compress_rev,
	    Compress_T query3_compress_fwd, Compress_T query3_compress_rev,
	    char *queryuc_ptr_5, char *queryuc_ptr_3, char *queryrc5, char *queryrc3,
	    int querylength5, int querylength3, int query5_lastpos, int query3_lastpos,
	    Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev, int indexdb_size_threshold, Floors_T *floors_array,

	    Oligoindex_T *oligoindices_major, int noligoindices_major,
	    Oligoindex_T *oligoindices_minor, int noligoindices_minor,
	    Pairpool_T pairpool, Diagpool_T diagpool,
	    Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,

	    int user_maxlevel_5, int user_maxlevel_3, int subopt_levels,
	    int indel_penalty_middle, int indel_penalty_end,
	    int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
	    bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
	    bool allvalidp5, bool allvalidp3, Chrpos_T pairmax,
	    int maxpairedpaths, bool keep_floors_p, Shortread_T queryseq5, Shortread_T queryseq3,
	    int genestrand) {

  List_T hitpairs = NULL, p;
  Stage3pair_T newpair;
  List_T gmap5_hits, gmap3_hits, a;
  Stage3end_T hit5, hit3, gmap5, gmap3;
  List_T hitarray5[HITARRAY_N], hitarray3[HITARRAY_N];
  List_T subs5 = NULL, indels5 = NULL, singlesplicing5 = NULL, doublesplicing5 = NULL, terminals5 = NULL;
  List_T subs3 = NULL, indels3 = NULL, singlesplicing3 = NULL, doublesplicing3 = NULL, terminals3 = NULL;
  List_T longsinglesplicing5 = NULL, longsinglesplicing3 = NULL;
  int ignore_found_score, done_level_5, done_level_3, opt_level, fast_level_5, fast_level_3,
    mismatch_level_5, mismatch_level_3, nmismatches, max_mismatches_allowed;
  int max_splice_mismatches_5 = -1, max_splice_mismatches_3 = -1, i;
  int nhits5 = 0, nhits3 = 0, nsplicepairs5 = 0, nsplicepairs3 = 0;
  List_T *donors_plus_5, *antidonors_plus_5, *acceptors_plus_5, *antiacceptors_plus_5,
    *donors_minus_5, *antidonors_minus_5, *acceptors_minus_5, *antiacceptors_minus_5;
  List_T *donors_plus_3, *antidonors_plus_3, *acceptors_plus_3, *antiacceptors_plus_3,
    *donors_minus_3, *antidonors_minus_3, *acceptors_minus_3, *antiacceptors_minus_3;

  bool spanningset5p, spanningset3p, completeset5p, completeset3p;
  bool did_alignment_p, did_singlesplicing5_p, did_singlesplicing3_p;
  bool any_omitted_p_5, any_omitted_p_3;
  Floors_T floors5, floors3;
  bool alloc_floors_p_5 = false, alloc_floors_p_3 = false, floors5_computed_p = false, floors3_computed_p = false,
    segments5_computed_p = false, segments3_computed_p = false, alloc5p, alloc3p;
  int best_score_paired;
  bool found_terminals_p = false;
  int nconcordant = 0, nsamechr = 0;


  *samechr = (List_T) NULL;
  *conc_transloc = (List_T) NULL;
  *with_terminal = (List_T) NULL;

  /* For paired-end alignment, ignore found_scores from single-end
     alignments.  Use only the found_score from
     Stage3_pair_up_concordant. */
  *found_score = querylength5 + querylength3;
  ignore_found_score = querylength5 + querylength3;

  fast_level_5 = (querylength5 + index1interval - 1)/spansize - NREQUIRED_FAST;
  fast_level_3 = (querylength3 + index1interval - 1)/spansize - NREQUIRED_FAST;

  debug(printf("fast_level_5 %d = (querylength %d + index1interval %d - 1)/spansize %d - nrequired_fast %d\n",
	       fast_level_5,querylength5,index1interval,spansize,NREQUIRED_FAST));
  debug(printf("fast_level_3 %d = (querylength %d + index1interval %d - 1)/spansize %d - nrequired_fast %d\n",
	       fast_level_3,querylength3,index1interval,spansize,NREQUIRED_FAST));

#if 0
  /* This prevents complete_mm procedure, needed for short reads */
  if (fast_level_5 < 1 && user_maxlevel_5 < 0) {
    fast_level_5 = 1;		/* Do at least 1 mismatch */
  }
  if (fast_level_3 < 1 && user_maxlevel_3 < 0) {
    fast_level_3 = 1;		/* Do at least 1 mismatch */
  }
#endif

  if (user_maxlevel_5 >= 0) {
    *cutoff_level_5 = user_maxlevel_5;
  } else if (fast_level_5 >= 0) {
    *cutoff_level_5 = fast_level_5;
  } else {
    *cutoff_level_5 = 0;
  }

  if (user_maxlevel_3 >= 0) {
    *cutoff_level_3 = user_maxlevel_3;
  } else if (fast_level_3 >= 0) {
    *cutoff_level_3 = fast_level_3;
  } else {
    *cutoff_level_3 = 0;
  }

  if (user_maxlevel_5 < 0) {
    if (fast_level_5 >= 0) {
      user_maxlevel_5 = fast_level_5;
    } else {
      user_maxlevel_5 = 0;
    }
  }

  if (user_maxlevel_3 < 0) {
    if (fast_level_3 >= 0) {
      user_maxlevel_3 = fast_level_3;
    } else {
      user_maxlevel_3 = 0;
    }
  }

#if 0
  if (dibasep) {
    opt_level = querylength5 + querylength3;
    done_level_5 = querylength5;
    done_level_3 = querylength3;
  }
#endif
  opt_level = user_maxlevel_5 + user_maxlevel_3;
  done_level_5 = user_maxlevel_5 /* + subopt_levels */;
  done_level_3 = user_maxlevel_3 /* + subopt_levels */;
  debug(printf("0> opt_level %d, done_level %d,%d\n",opt_level,done_level_5,done_level_3));

  for (i = 0; i < HITARRAY_N; i++) {
    hitarray5[i] = hitarray3[i] = (List_T) NULL;
  }

  nhits5 = nhits3 = 0;
  if (use_sarray_p == true) {
    /* Replaces spanning set */
    Sarray_search_greedy(&ignore_found_score,&subs5,&indels5,&singlesplicing5,&doublesplicing5,
			 queryuc_ptr_5,queryrc5,querylength5,
			 query5_compress_fwd,query5_compress_rev,/*nmisses_allowed*/fast_level_5,
			 /*first_read_p*/true);
    Sarray_search_greedy(&ignore_found_score,&subs3,&indels3,&singlesplicing3,&doublesplicing3,
			 queryuc_ptr_3,queryrc3,querylength3,
			 query3_compress_fwd,query3_compress_rev,/*nmisses_allowed*/fast_level_3,
			 /*first_read_p*/false);
    hitarray5[HITARRAY_SUBS] = subs5 = Stage3end_remove_duplicates(subs5,queryseq5,queryseq3);
    hitarray3[HITARRAY_SUBS] = subs3 = Stage3end_remove_duplicates(subs3,queryseq5,queryseq3);
    hitarray5[HITARRAY_INDELS] = indels5 = Stage3end_remove_duplicates(indels5,queryseq5,queryseq3);
    hitarray3[HITARRAY_INDELS] = indels3 = Stage3end_remove_duplicates(indels3,queryseq5,queryseq3);
    hitarray5[HITARRAY_SINGLESPLICING] = singlesplicing5;
    hitarray3[HITARRAY_SINGLESPLICING] = singlesplicing3;
    hitarray5[HITARRAY_DOUBLESPLICING] = doublesplicing5;
    hitarray3[HITARRAY_DOUBLESPLICING] = doublesplicing3;
    debug(printf("sarray: 5' end has %d subs, %d indels, %d single splices, %d double splices\n",
		 List_length(subs5),List_length(indels5),List_length(singlesplicing5),List_length(doublesplicing5)));
    debug(printf("sarray: 3' end has %d subs, %d indels, %d single splices, %d double splices\n",
		 List_length(subs3),List_length(indels3),List_length(singlesplicing3),List_length(doublesplicing3)));

    hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					 &(*samechr),&(*conc_transloc),&(*with_terminal),
					 hitpairs,hitarray5,/*narray5*/HITARRAY_DOUBLESPLICING+1,
					 hitarray3,/*narray3*/HITARRAY_DOUBLESPLICING+1,
					 /*terminals5*/NULL,/*terminals3*/NULL,
					 *cutoff_level_5,*cutoff_level_3,subopt_levels,
					 splicesites,query5_compress_fwd,query5_compress_rev,
					 query3_compress_fwd,query3_compress_rev,
					 querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					 genestrand);

    debug(printf("After pairing sarray, found %d concordant, %d samechr, found_score %d\n",
		 nconcordant,nsamechr,*found_score));
    if (*abort_pairing_p == true) {
      *hits5 = subs5;
      *hits3 = subs3;
      return Stage3pair_remove_circular_alias(hitpairs);
    } else {
      opt_level = (*found_score < opt_level) ? *found_score : opt_level;
      if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	done_level_5 = user_maxlevel_5;
      }
      if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	done_level_3 = user_maxlevel_3;
      }
      debug(printf("SA> found_score = %d, opt_level %d, done_level %d,%d\n",*found_score,opt_level,done_level_5,done_level_3));
    }
    nhits5 = List_length(subs5);
    nhits3 = List_length(subs3);
  }

  if (nconcordant == 0) {
    spanningset5p = spanningset3p = true;
  } else {
    spanningset5p = spanningset3p = false;
    if (better_free_end_exists_p(subs5,indels5,singlesplicing5,doublesplicing5,querylength5) == true) {
      spanningset3p = true;	/* Do search on other end */
    }
    if (better_free_end_exists_p(subs3,indels3,singlesplicing3,doublesplicing3,querylength3) == true) {
      spanningset5p = true;	/* Do search on other end */
    }
  }

  if (spanningset5p == true || spanningset3p == true) {
    /* 1A. Exact.  Requires compress if cmet or genomealt.  Creates and uses spanning set. */
    mismatch_level_5 = 0;
    if (allvalidp5 == false) {
      debug(printf("Not all oligos in 5' end are valid, so cannot perform spanning set\n"));
      fast_level_5 = -1;
    } else if (spanningset5p == true) {
      debug(printf("fast_level_5 = %d\n",fast_level_5));
      debug(printf("*** Stage 1.  Exact ***\n"));
      subs5 = find_spanning_exact_matches(&ignore_found_score,&nhits5,subs5,this5,genestrand,
					  querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,
					  query5_compress_fwd,query5_compress_rev);
      mismatch_level_5 = 1;
    }

    /* 1B. Exact.  Requires compress if cmet or genomealt.  Creates and uses spanning set. */
    mismatch_level_3 = 0;
    if (allvalidp3 == false) {
      debug(printf("Not all oligos in 3' end are valid, so cannot perform spanning set\n"));
      fast_level_3 = -1;
    } else if (spanningset3p == true) {
      debug(printf("fast_level_3 = %d\n",fast_level_3));
      debug(printf("*** Stage 1.  Exact ***\n"));
      subs3 = find_spanning_exact_matches(&ignore_found_score,&nhits3,subs3,this3,genestrand,
					  querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,
					  query3_compress_fwd,query3_compress_rev);
      mismatch_level_3 = 1;
    }

    /* 1. Pairing after exact */
    /* Should not have duplicates from the spanning set procedure */
    hitarray5[HITARRAY_SUBS] = subs5; /* = Stage3end_remove_duplicates(subs5) */;
    hitarray3[HITARRAY_SUBS] = subs3; /* = Stage3end_remove_duplicates(subs3) */;
    hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					 &(*samechr),&(*conc_transloc),&(*with_terminal),
					 hitpairs,hitarray5,/*narray5*/HITARRAY_SUBS+1,
					 hitarray3,/*narray3*/HITARRAY_SUBS+1,
					 /*terminals5*/NULL,/*terminals3*/NULL,
					 *cutoff_level_5,*cutoff_level_3,subopt_levels,
					 splicesites,query5_compress_fwd,query5_compress_rev,
					 query3_compress_fwd,query3_compress_rev,
					 querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					 genestrand);
    debug(printf("After pairing exact, found %d concordant, %d samechr, found_score %d\n",
		 nconcordant,nsamechr,*found_score));
    if (*abort_pairing_p == true) {
      *hits5 = subs5;
      *hits3 = subs3;
      return hitpairs;
    } else {
      opt_level = (*found_score < opt_level) ? *found_score : opt_level;
      if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	done_level_5 = user_maxlevel_5;
      }
      if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	done_level_3 = user_maxlevel_3;
      }
      debug(printf("1> found_score = %d, opt_level %d, done_level %d,%d\n",*found_score,opt_level,done_level_5,done_level_3));
    }

    did_alignment_p = false;

    /* 2A. One mismatch.  Requires spanning set and compress. */
    if (spanningset5p && allvalidp5 && querylength5 >= one_miss_querylength && done_level_5 >= 1) {
      debug(printf("*** Stage 2A.  One miss ***\n"));
      did_alignment_p = true;
      subs5 = find_spanning_onemiss_matches(&ignore_found_score,&nhits5,subs5,this5,genestrand,querylength5,
					    query5_compress_fwd,query5_compress_rev);
      mismatch_level_5 = 2;
    }

    /* 2B. One mismatch.  Requires spanning set and compress. */
    if (spanningset3p && allvalidp3 && querylength3 >= one_miss_querylength && done_level_3 >= 1) {
      debug(printf("*** Stage 2B.  One miss ***\n"));
      did_alignment_p = true;
      subs3 = find_spanning_onemiss_matches(&ignore_found_score,&nhits3,subs3,this3,genestrand,querylength3,
					    query3_compress_fwd,query3_compress_rev);
      mismatch_level_3 = 2;
    }

    if (did_alignment_p == true) {
      /* 2. Pairing after one mismatch */
      hitarray5[HITARRAY_SUBS] = subs5 /* = Stage3end_remove_duplicates(subs5,queryseq5,queryseq3) */;
      hitarray3[HITARRAY_SUBS] = subs3 /* = Stage3end_remove_duplicates(subs3,queryseq5,queryseq3) */;
      hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					   &(*samechr),&(*conc_transloc),&(*with_terminal),
					   hitpairs,hitarray5,/*narray5*/HITARRAY_SUBS+1,
					   hitarray3,/*narray3*/HITARRAY_SUBS+1,
					   /*terminals5*/NULL,/*terminals3*/NULL,
					   *cutoff_level_5,*cutoff_level_3,subopt_levels,
					   splicesites,query5_compress_fwd,query5_compress_rev,
					   query3_compress_fwd,query3_compress_rev,
					   querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					   genestrand);
      debug(printf("After pairing one mismatch, found %d concordant, %d samechr, found_score %d\n",
		   nconcordant,nsamechr,*found_score));
      if (*abort_pairing_p == true) {
	*hits5 = subs5;
	*hits3 = subs3;
	return Stage3pair_remove_circular_alias(hitpairs);
      } else {
	opt_level = (*found_score < opt_level) ? *found_score : opt_level;
	if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	  done_level_5 = user_maxlevel_5;
	}
	if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	  done_level_3 = user_maxlevel_3;
	}
	debug(printf("2> found_score = %d, opt_level %d, done_level %d,%d\n",*found_score,opt_level,done_level_5,done_level_3));
      }
    }


    did_alignment_p = false;

    /* 3A. Mismatches via spanning set.  Requires spanning set and compress. */
    if (spanningset5p && allvalidp5 && done_level_5 >= 2) {
      /* NOTE: Since done_level isn't updated, can do in one batch instead of iteratively */
      while (mismatch_level_5 <= fast_level_5 && mismatch_level_5 <= done_level_5) {
	debug(printf("*** Stage 3A (level %d).  Spanning set mismatches ***\n",mismatch_level_5));
	did_alignment_p = true;
	subs5 = find_spanning_multimiss_matches(&ignore_found_score,&nhits5,subs5,this5,genestrand,NREQUIRED_FAST,querylength5,
						query5_compress_fwd,query5_compress_rev,
						/*nmisses_allowed*/mismatch_level_5);
	mismatch_level_5++;
      }
    }

    /* 3B. Mismatches via spanning set.  Requires spanning set and compress. */
    if (spanningset3p && allvalidp3 && done_level_3 >= 2) {
      /* NOTE: Since done_level isn't updated, can do in one batch instead of iteratively */
      while (mismatch_level_3 <= fast_level_3 && mismatch_level_3 <= done_level_3) {
	debug(printf("*** Stage 3B (level %d).  Spanning set mismatches ***\n",mismatch_level_3));
	did_alignment_p = true;
	subs3 = find_spanning_multimiss_matches(&ignore_found_score,&nhits3,subs3,this3,genestrand,NREQUIRED_FAST,querylength3,
						query3_compress_fwd,query3_compress_rev,
						/*nmisses_allowed*/mismatch_level_3);
	mismatch_level_3++;
      }
    }

    if (did_alignment_p == true) {
      /* 3. Pairing after spanning set subs */
      hitarray5[HITARRAY_SUBS] = subs5 /* = Stage3end_remove_duplicates(subs5,queryseq5,queryseq3) */;
      hitarray3[HITARRAY_SUBS] = subs3 /* = Stage3end_remove_duplicates(subs3,queryseq5,queryseq3) */;
      hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					   &(*samechr),&(*conc_transloc),&(*with_terminal),
					   hitpairs,hitarray5,/*narray5*/HITARRAY_SUBS+1,
					   hitarray3,/*narray3*/HITARRAY_SUBS+1,
					   /*terminals5*/NULL,/*terminals3*/NULL,
					   *cutoff_level_5,*cutoff_level_3,subopt_levels,
					   splicesites,query5_compress_fwd,query5_compress_rev,
					   query3_compress_fwd,query3_compress_rev,
					   querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					   genestrand);
      debug(printf("After pairing spanning set, found %d concordant, %d samechr, found_score %d\n",
		   nconcordant,nsamechr,*found_score));
      if (*abort_pairing_p == true) {
	*hits5 = subs5;
	*hits3 = subs3;
	return Stage3pair_remove_circular_alias(hitpairs);
      } else {
	opt_level = (*found_score < opt_level) ? *found_score : opt_level;
	if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	  done_level_5 = user_maxlevel_5;
	}
	if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	  done_level_3 = user_maxlevel_3;
	}
	debug(printf("3> found_score = %d, opt_level %d, done_level %d,%d\n",*found_score,opt_level,done_level_5,done_level_3));
      }
    }
  }


  completeset5p = completeset3p = false;
  if (better_free_end_exists_p(subs5,indels5,singlesplicing5,doublesplicing5,querylength5) == true) {
    completeset3p = true;	/* Do search on other end */
  }
  if (better_free_end_exists_p(subs3,indels3,singlesplicing3,doublesplicing3,querylength3) == true) {
    completeset5p = true;	/* Do search on other end */
  }

  did_alignment_p = false;

  /* 4/5A.  Complete set mismatches and indels, omitting frequent oligos */
  if (use_sarray_p == true && completeset5p == false) {
    /* Skip.  Suffix array already found something */

  } else if (done_level_5 > fast_level_5 || done_level_5 >= indel_penalty_middle || done_level_5 >= indel_penalty_end) {
    did_alignment_p = true;

#if 1
    floors5 = compute_floors(&any_omitted_p_5,&alloc_floors_p_5,floors_array,this5,
			     querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
			     keep_floors_p);
    floors5_computed_p = true;
    complete_set_mm_indels(&ignore_found_score,&segments5_computed_p,
			   &opt_level,&done_level_5,user_maxlevel_5,/*revise_levels_p*/false,
			   &nhits5,&subs5,&indels5,this5,query5_compress_fwd,query5_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			   queryuc_ptr_5,queryrc5,
#endif
			   querylength5,query5_lastpos,floors5,subopt_levels,
			   indel_penalty_middle,indel_penalty_end,
			   allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			   fast_level_5,genestrand);

#else
    /* Using obsolete masktype */
    if (masktype == MASK_NONE) {
      debug(printf("*** Stage 4A,5A.  Complete mm/indels with no masking with done_level %d ***\n",done_level_5));
      complete_set_mm_indels(&ignore_found_score,&segments5_computed_p,
			     &any_omitted_p_5,&opt_level,&done_level_5,user_maxlevel_5,/*revise_levels_p*/false,
			     &nhits5,&subs5,&indels5,this5,query5_compress_fwd,query5_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			     queryuc_ptr_5,queryrc5,
#endif
			     querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     floors_array,subopt_levels,
			     indel_penalty_middle,indel_penalty_end,
			     allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			     fast_level_5,/*omit_frequent_p*/false,/*omit_repetitive_p*/false,keep_floors_p,
			     genestrand);
    } else {
      debug(printf("*** Stage 4A,5A.  Complete mm/indels masking frequent oligos with done_level %d ***\n",done_level_5));
      complete_set_mm_indels(&ignore_found_score,&segments5_computed_p,
			     &any_omitted_p_5,&opt_level,&done_level_5,user_maxlevel_5,/*revise_levels_p*/false,
			     &nhits5,&subs5,&indels5,this5,query5_compress_fwd,query5_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			     queryuc_ptr_5,queryrc5,
#endif
			     querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     floors_array,subopt_levels,
			     indel_penalty_middle,indel_penalty_end,
			     allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			     fast_level_5,/*omit_frequent_p*/true,
			     /*omit_repetitive_p*/(masktype == MASK_REPETITIVE || masktype == MASK_GREEDY_REPETITIVE) ? true : false,
			     keep_floors_p,genestrand);
      if ((masktype == MASK_GREEDY_FREQUENT || masktype == MASK_GREEDY_REPETITIVE) && subs5 == NULL && indels5 == NULL && any_omitted_p_5 == true) {
	FREE(this->minus_segments_5);
	FREE(this->plus_segments_5);

	/* 4/5A.  Complete set mismatches and indels, with all oligos */
	debug(printf("*** Stage 4A,5A.  Complete mm/indels with no masking with done_level %d ***\n",done_level_5));
	complete_set_mm_indels(&ignore_found_score,&segments5_computed_p,
			       &any_omitted_p_5,&opt_level,&done_level_5,user_maxlevel_5,/*revise_levels_p*/false,
			       &nhits5,&subs5,&indels5,this5,query5_compress_fwd,query5_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			       queryuc_ptr_5,queryrc5,
#endif
			       querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			       floors_array,subopt_levels,
			       indel_penalty_middle,indel_penalty_end,
			       allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			       fast_level_5,/*omit_frequent_p*/false,/*omit_repetitive_p*/false,keep_floors_p,
			       genestrand);
      }
    }
#endif
  }

  /* 4/5B.  Complete set mismatches and indels, omitting frequent oligos */
  if (use_sarray_p == true && completeset3p == false) {
    /* Skip.  Suffix array already found something */

  } else if (done_level_3 > fast_level_3 || done_level_3 >= indel_penalty_middle || done_level_3 >= indel_penalty_end) {
    did_alignment_p = true;

#if 1
    floors3 = compute_floors(&any_omitted_p_3,&alloc_floors_p_3,floors_array,this3,
			     querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
			     keep_floors_p);
    floors3_computed_p = true;

    complete_set_mm_indels(&ignore_found_score,&segments3_computed_p,
			   &opt_level,&done_level_3,user_maxlevel_3,/*revise_levels_p*/false,
			   &nhits3,&subs3,&indels3,this3,query3_compress_fwd,query3_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			   queryuc_ptr_3,queryrc3,
#endif
			   querylength3,query3_lastpos,floors3,subopt_levels,
			   indel_penalty_middle,indel_penalty_end,
			   allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			   fast_level_3,genestrand);

#else
    if (masktype == MASK_NONE) {
      debug(printf("*** Stage 4B,5B.  Complete mm/indels with no masking with done_level %d ***\n",done_level_3));
      complete_set_mm_indels(&ignore_found_score,&segments3_computed_p,
			     &any_omitted_p_3,&opt_level,&done_level_3,user_maxlevel_3,/*revise_levels_p*/false,
			     &nhits3,&subs3,&indels3,this3,query3_compress_fwd,query3_compress_rev,queryuc_ptr_3,
#if defined(DEBUG2) || defined(DEBUG2E)
			     queryrc3,
#endif
			     querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     floors_array,subopt_levels,
			     indel_penalty_middle,indel_penalty_end,
			     allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			     fast_level_3,/*omit_frequent_p*/false,/*omit_repetitive_p*/false,keep_floors_p,
			     genestrand);
    } else {
      debug(printf("*** Stage 4B,5B.  Complete mm/indels masking frequent oligos with done_level %d ***\n",done_level_3));
      complete_set_mm_indels(&ignore_found_score,&segments3_computed_p,
			     &any_omitted_p_3,&opt_level,&done_level_3,user_maxlevel_3,/*revise_levels_p*/false,
			     &nhits3,&subs3,&indels3,this3,query3_compress_fwd,query3_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			     queryuc_ptr_3,queryrc3,
#endif
			     querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			     floors_array,subopt_levels,
			     indel_penalty_middle,indel_penalty_end,
			     allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			     fast_level_3,/*omit_frequent_p*/true,
			     /*omit_repetitive_p*/(masktype == MASK_REPETITIVE || masktype == MASK_GREEDY_REPETITIVE) ? true : false,
			     keep_floors_p,genestrand);
      if ((masktype == MASK_GREEDY_FREQUENT || masktype == MASK_GREEDY_REPETITIVE) && subs3 == NULL && indels3 == NULL && any_omitted_p_3 == true) {
	FREE(this->minus_segments_3);
	FREE(this->plus_segments_3);

	/* 4/5B.  Complete set mismatches and indels, with all oligos */
	debug(printf("*** Stage 4B,5B.  Complete mm/indels with no masking with done_level %d ***\n",done_level_3));
	complete_set_mm_indels(&ignore_found_score,&segments3_computed_p,
			       &any_omitted_p_3,&opt_level,&done_level_3,user_maxlevel_3,/*revise_levels_p*/false,
			       &nhits3,&subs3,&indels3,this3,query3_compress_fwd,query3_compress_rev,
#if defined(DEBUG2) || defined(DEBUG2E)
			       queryuc_ptr_3,queryrc3,
#endif
			       querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
			       floors_array,subopt_levels,
			       indel_penalty_middle,indel_penalty_end,
			       allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			       fast_level_3,/*omit_frequent_p*/false,/*omit_repetitive_p*/false,keep_floors_p,
			       genestrand);
      }
    }
#endif
  }

  debug(printf("complete: 5' end has %d subs, %d indels, %d single splices, %d double splices\n",
	       List_length(subs5),List_length(indels5),List_length(singlesplicing5),List_length(doublesplicing5)));
  debug(printf("complete: 3' end has %d subs, %d indels, %d single splices, %d double splices\n",
	       List_length(subs3),List_length(indels3),List_length(singlesplicing3),List_length(doublesplicing3)));

  if (did_alignment_p == true) {
    /* 4/5. Pairing after complete set subs and indels */
    debug(printf("Starting pairing of 4 and 5\n"));
    hitarray5[HITARRAY_SUBS] = subs5 /* = Stage3end_remove_duplicates(subs5,queryseq5,queryseq3) */;
    hitarray5[HITARRAY_INDELS] = indels5 /* = Stage3end_remove_duplicates(indels5,queryseq5,queryseq3) */;
    hitarray3[HITARRAY_SUBS] = subs3 /* = Stage3end_remove_duplicates(subs3,queryseq5,queryseq3) */;
    hitarray3[HITARRAY_INDELS] = indels3 /* = Stage3end_remove_duplicates(indels3,queryseq5,queryseq3) */;
    hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					 &(*samechr),&(*conc_transloc),&(*with_terminal),
					 hitpairs,hitarray5,/*narray5*/HITARRAY_INDELS+1,
					 hitarray3,/*narray3*/HITARRAY_INDELS+1,
					 /*terminals5*/NULL,/*terminals3*/NULL,
					 *cutoff_level_5,*cutoff_level_3,subopt_levels,
					 splicesites,query5_compress_fwd,query5_compress_rev,
					 query3_compress_fwd,query3_compress_rev,
					 querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					 genestrand);
    debug(printf("After pairing complete set mismatches and indels, found %d concordant, %d nsamechr, found_score %d\n",
		 nconcordant,nsamechr,*found_score));
    if (*abort_pairing_p == true) {
      *hits5 = List_append(subs5,indels5);
      *hits3 = List_append(subs3,indels3);
      return Stage3pair_remove_circular_alias(hitpairs);
    } else {
      opt_level = (*found_score < opt_level) ? *found_score : opt_level;
      if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	done_level_5 = user_maxlevel_5;
      }
      if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	done_level_3 = user_maxlevel_3;
      }
      debug(printf("4/5> found_score = %d, opt_level %d, done_level %d,%d\n",*found_score,opt_level,done_level_5,done_level_3));
    }
  }


  completeset5p = completeset3p = false;
  if (better_free_end_exists_p(subs5,indels5,singlesplicing5,doublesplicing5,querylength5) == true) {
    completeset3p = true;	/* Do search on other end */
  }
  if (better_free_end_exists_p(subs3,indels3,singlesplicing3,doublesplicing3,querylength3) == true) {
    completeset5p = true;	/* Do search on other end */
  }

  /* 6/7/8. Local splicing.  Requires compress and all positions fetched. */
  /* Subtract 1 from done_level for previous hits */
  did_singlesplicing5_p = false;
  if (use_sarray_p == true && completeset5p == false) {
    /* Skip.  Suffix array already found something */

  } else if (knownsplicingp || novelsplicingp) {
    debug(printf("Deciding whether to do singlesplicing: done_level_5 %d >=? localsplicing_penalty %d\n",
		 done_level_5,localsplicing_penalty));

    if (done_level_5 >= localsplicing_penalty) {
      debug(printf("*** Stage 6A.  Single splicing masking frequent oligos with done_level %d ***\n",done_level_5));
      /* Always mask frequent oligos for splicing, which must be transcriptional */
      if (floors5_computed_p == false) {
	floors5 = compute_floors(&any_omitted_p_5,&alloc_floors_p_5,floors_array,this5,
				 querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
	floors5_computed_p = true;
      }

      if (segments5_computed_p == false) {
	this5->plus_segments = identify_all_segments(&this5->plus_nsegments,&this5->plus_spliceable,&this5->plus_nspliceable,
						     this5->plus_positions,this5->plus_npositions,
						     this5->omitted,querylength5,query5_lastpos,floors5,/*plusp*/true);
	this5->minus_segments = identify_all_segments(&this5->minus_nsegments,&this5->minus_spliceable,&this5->minus_nspliceable,
						      this5->minus_positions,this5->minus_npositions,
						      this5->omitted,querylength5,query5_lastpos,floors5,/*plusp*/false);
	segments5_computed_p = true;
      }

      did_singlesplicing5_p = true;
      singlesplicing5 = complete_set_singlesplicing(&ignore_found_score,singlesplicing5,floors5,this5,
						    query5_compress_fwd,query5_compress_rev,
						    querylength5,query5_lastpos,
						    localsplicing_penalty,
						    /*max_mismatches_allowed*/done_level_5 - localsplicing_penalty,
						    /*first_read_p*/true,genestrand,
						    /*subs_or_indels_p*/(subs5 != NULL || indels5 != NULL) ? true : false);
    }
  }

  did_singlesplicing3_p = false;
  if (use_sarray_p == true && completeset3p == false) {
    /* Skip.  Suffix array already found something */

  } else if (knownsplicingp || novelsplicingp) {
    debug(printf("Deciding whether to do singlesplicing: done_level_3 %d >=? localsplicing_penalty %d\n",
		 done_level_3,localsplicing_penalty));
    if (done_level_3 >= localsplicing_penalty) {
      debug(printf("*** Stage 6B.  Single splicing masking frequent oligos with done_level %d ***\n",done_level_3));
      /* Always mask frequent oligos for splicing, which must be transcriptional */
      if (floors3_computed_p == false) {
	floors3 = compute_floors(&any_omitted_p_3,&alloc_floors_p_3,floors_array,this3,
				 querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
	floors3_computed_p = true;
      }

      if (segments3_computed_p == false) {
	this3->plus_segments = identify_all_segments(&this3->plus_nsegments,&this3->plus_spliceable,&this3->plus_nspliceable,
						     this3->plus_positions,this3->plus_npositions,
						     this3->omitted,querylength3,query3_lastpos,floors3,/*plusp*/true);
	this3->minus_segments = identify_all_segments(&this3->minus_nsegments,&this3->minus_spliceable,&this3->minus_nspliceable,
						      this3->minus_positions,this3->minus_npositions,
						      this3->omitted,querylength3,query3_lastpos,floors3,/*plusp*/false);
	segments3_computed_p = true;
      }

      did_singlesplicing3_p = true;
      singlesplicing3 = complete_set_singlesplicing(&ignore_found_score,singlesplicing3,floors3,this3,
						    query3_compress_fwd,query3_compress_rev,
						    querylength3,query3_lastpos,
						    localsplicing_penalty,
						    /*max_mismatches_allowed*/done_level_3 - localsplicing_penalty,
						    /*first_read_p*/false,genestrand,
						    /*subs_or_indels_p*/(subs3 != NULL || indels3 != NULL) ? true : false);
    }
  }

  if (did_singlesplicing5_p == true || did_singlesplicing3_p == true) {
    /* 6.  Pairing after single splicing */
    /* Mark ambiguous splices only for single-end reads */
    hitarray5[HITARRAY_SINGLESPLICING] = singlesplicing5;
    hitarray3[HITARRAY_SINGLESPLICING] = singlesplicing3;

    hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					 &(*samechr),&(*conc_transloc),&(*with_terminal),
					 hitpairs,hitarray5,/*narray5*/HITARRAY_SINGLESPLICING+1,
					 hitarray3,/*narray3*/HITARRAY_SINGLESPLICING+1,
					 /*terminals5*/NULL,/*terminals3*/NULL,
					 *cutoff_level_5,*cutoff_level_3,subopt_levels,
					 splicesites,query5_compress_fwd,query5_compress_rev,
					 query3_compress_fwd,query3_compress_rev,
					 querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					 genestrand);
    debug(printf("After pairing single splicing, found %d concordant, %d nsamechr, found_score %d\n",
		 nconcordant,nsamechr,*found_score));
    if (*abort_pairing_p == true) {
      if (alloc_floors_p_5 == true) {
	Floors_free(&floors5);
      }
      if (alloc_floors_p_3 == true) {
	Floors_free(&floors3);
      }
      *hits5 = List_append(subs5,List_append(indels5,singlesplicing5));
      *hits3 = List_append(subs3,List_append(indels3,singlesplicing3));
      return Stage3pair_remove_circular_alias(hitpairs);

    } else {
      opt_level = (*found_score < opt_level) ? *found_score : opt_level;
      if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	done_level_5 = user_maxlevel_5;
      }
      if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	done_level_3 = user_maxlevel_3;
      }
      debug(printf("Pairing after 6A and 6B> found_score = %d, opt_level %d, done_level %d,%d\n",
		   *found_score,opt_level,done_level_5,done_level_3));
    }

    /* 7.  Double splicing */
    if (done_level_5 >= localsplicing_penalty) {
      debug(printf("*** Stage 7A.  Double splicing masking frequent oligos with done_level %d ***\n",done_level_5));
      if (floors5_computed_p == false) {
	floors5 = compute_floors(&any_omitted_p_5,&alloc_floors_p_5,floors_array,this5,
				 querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
	floors5_computed_p = true;
      }
      doublesplicing5 = complete_set_doublesplicing(&ignore_found_score,doublesplicing5,floors5,this5,
						    query5_compress_fwd,query5_compress_rev,
						    queryuc_ptr_5,queryrc5,querylength5,query5_lastpos,
						    localsplicing_penalty,min_shortend,
						    /*max_mismatches_allowed*/done_level_5 - localsplicing_penalty,
						    /*pairedp*/true,/*first_read_p*/true,genestrand,
						    /*subs_or_indels_p*/(subs5 != NULL || indels5 != NULL) ? true : false);
    }

    if (done_level_3 >= localsplicing_penalty) {
      debug(printf("*** Stage 7B.  Double splicing masking frequent oligos with done_level %d ***\n",done_level_3));
      if (floors3_computed_p == false) {
	floors3 = compute_floors(&any_omitted_p_3,&alloc_floors_p_3,floors_array,this3,
				 querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
	floors3_computed_p = true;
      }
      doublesplicing3 = complete_set_doublesplicing(&ignore_found_score,doublesplicing3,floors3,this3,
						    query3_compress_fwd,query3_compress_rev,
						    queryuc_ptr_3,queryrc3,querylength3,query3_lastpos,
						    localsplicing_penalty,min_shortend,
						    /*max_mismatches_allowed*/done_level_3 - localsplicing_penalty,
						    /*pairedp*/true,/*first_read_p*/false,genestrand,
						    /*subs_or_indels_p*/(subs3 != NULL || indels3 != NULL) ? true : false);
    }
    
    /* 7.  Pairing after double splicing */
    /* Mark ambiguous splices only for single-end reads */
    hitarray5[HITARRAY_DOUBLESPLICING] = doublesplicing5;
    hitarray3[HITARRAY_DOUBLESPLICING] = doublesplicing3;

    hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					 &(*samechr),&(*conc_transloc),&(*with_terminal),
					 hitpairs,hitarray5,/*narray5*/HITARRAY_DOUBLESPLICING+1,
					 hitarray3,/*narray3*/HITARRAY_DOUBLESPLICING+1,
					 /*terminals5*/NULL,/*terminals3*/NULL,
					 *cutoff_level_5,*cutoff_level_3,subopt_levels,
					 splicesites,query5_compress_fwd,query5_compress_rev,
					 query3_compress_fwd,query3_compress_rev,
					 querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					 genestrand);
    debug(printf("After pairing double splicing, found %d concordant, %d samechr, found_score %d\n",
		 nconcordant,nsamechr,*found_score));
    if (*abort_pairing_p == true) {
      if (alloc_floors_p_5 == true) {
	Floors_free(&floors5);
      }
      if (alloc_floors_p_3 == true) {
	Floors_free(&floors3);
      }
      *hits5 = List_append(subs5,List_append(indels5,List_append(singlesplicing5,doublesplicing5)));
      *hits3 = List_append(subs3,List_append(indels3,List_append(singlesplicing3,doublesplicing3)));
      return Stage3pair_remove_circular_alias(hitpairs);

    } else {
      opt_level = (*found_score < opt_level) ? *found_score : opt_level;
      if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	done_level_5 = user_maxlevel_5;
      }
      if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	done_level_3 = user_maxlevel_3;
      }
      debug(printf("Pairing after 7A and 7B> found_score = %d, opt_level %d, done_level %d,%d\n",
		   *found_score,opt_level,done_level_5,done_level_3));
    }


    alloc5p = false;
    if (knownsplicingp == true && done_level_5 >= localsplicing_penalty) {
      /* Want >= and not > to give better results.  Negligible effect on speed. */
      /* 8A.  Shortend splicing */
      max_splice_mismatches_5 = done_level_5 - localsplicing_penalty;

      alloc5p = true;
      donors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antidonors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      acceptors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antiacceptors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      donors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antidonors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      acceptors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antiacceptors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));

      find_spliceends_shortend(&donors_plus_5,&antidonors_plus_5,&acceptors_plus_5,&antiacceptors_plus_5,
			       this5->plus_segments,this5->plus_nsegments,
#ifdef DEBUG4E
			       queryuc_ptr_5,
#endif
			       floors5,querylength5,query5_lastpos,/*query_compress*/query5_compress_fwd,
			       /*max_mismatches_allowed*/max_splice_mismatches_5,/*plusp*/true,genestrand);

      find_spliceends_shortend(&antidonors_minus_5,&donors_minus_5,&antiacceptors_minus_5,&acceptors_minus_5,
			       this5->minus_segments,this5->minus_nsegments,
#ifdef DEBUG4E
			       /*queryptr*/queryrc5,
#endif
			       floors5,querylength5,query5_lastpos,/*query_compress*/query5_compress_rev,
			       /*max_mismatches_allowed*/max_splice_mismatches_5,/*plusp*/false,genestrand);

      singlesplicing5 = find_splicepairs_shortend(&ignore_found_score,/*hits*/singlesplicing5,
						  donors_plus_5,antidonors_plus_5,acceptors_plus_5,antiacceptors_plus_5,
						  donors_minus_5,antidonors_minus_5,acceptors_minus_5,antiacceptors_minus_5,
						  query5_compress_fwd,query5_compress_rev,
						  queryuc_ptr_5,queryrc5,min_shortend,localsplicing_penalty,
						  /*max_mismatches_allowed*/max_splice_mismatches_5,querylength5,
						  /*pairedp*/true,/*first_read_p*/true,genestrand);
    }


    alloc3p = false;
    if (knownsplicingp == true && done_level_3 >= localsplicing_penalty) {
      /* Want >= and not > to give better results.  Negligible effect on speed. */
      /* 8B.  Short-Overlap splicing */
      max_splice_mismatches_3 = done_level_3 - localsplicing_penalty;

      alloc3p = true;
      donors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antidonors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      acceptors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antiacceptors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      donors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antidonors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      acceptors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antiacceptors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));

      find_spliceends_shortend(&donors_plus_3,&antidonors_plus_3,&acceptors_plus_3,&antiacceptors_plus_3,
			       this3->plus_segments,this3->plus_nsegments,
#ifdef DEBUG4E
			       queryuc_ptr_3,
#endif
			       floors3,querylength3,query3_lastpos,/*query_compress*/query3_compress_fwd,
			       /*max_mismatches_allowed*/max_splice_mismatches_3,/*plusp*/true,genestrand);

      find_spliceends_shortend(&antidonors_minus_3,&donors_minus_3,&antiacceptors_minus_3,&acceptors_minus_3,
			       this3->minus_segments,this3->minus_nsegments,
#ifdef DEBUG4E
			       /*queryptr*/queryrc3,
#endif
			       floors3,querylength3,query3_lastpos,/*query_compress*/query3_compress_rev,
			       /*max_mismatches_allowed*/max_splice_mismatches_3,/*plusp*/false,genestrand);
      
      singlesplicing3 = find_splicepairs_shortend(&ignore_found_score,/*hits*/singlesplicing3,
						  donors_plus_3,antidonors_plus_3,acceptors_plus_3,antiacceptors_plus_3,
						  donors_minus_3,antidonors_minus_3,acceptors_minus_3,antiacceptors_minus_3,
						  query3_compress_fwd,query3_compress_rev,
						  queryuc_ptr_3,queryrc3,min_shortend,localsplicing_penalty,
						  /*max_mismatches_allowed*/max_splice_mismatches_3,querylength3,
						  /*pairedp*/true,/*first_read_p*/false,genestrand);
    }

    if (singlesplicing5 != NULL || singlesplicing3 != NULL) {
      /* 8.  Pairing after short-overlaps */
      hitarray5[HITARRAY_SINGLESPLICING] = singlesplicing5 /* = Stage3end_remove_duplicates(singlesplicing5,queryseq5,queryseq3) */;
      hitarray3[HITARRAY_SINGLESPLICING] = singlesplicing3 /* = Stage3end_remove_duplicates(singlesplicing3,queryseq5,queryseq3) */;
      hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					   &(*samechr),&(*conc_transloc),&(*with_terminal),
					   hitpairs,hitarray5,/*narray5*/HITARRAY_DOUBLESPLICING+1,
					   hitarray3,/*narray3*/HITARRAY_DOUBLESPLICING+1,
					   /*terminals5*/NULL,/*terminals3*/NULL,
					   *cutoff_level_5,*cutoff_level_3,subopt_levels,
					   splicesites,query5_compress_fwd,query5_compress_rev,
					   query3_compress_fwd,query3_compress_rev,
					   querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					   genestrand);
      debug(printf("After pairing short-overlap splicing, found %d concordant, %d samechr, found_score %d\n",
		   nconcordant,nsamechr,*found_score));
      if (*abort_pairing_p == false) {
	opt_level = (*found_score < opt_level) ? *found_score : opt_level;
	if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	  done_level_5 = user_maxlevel_5;
	}
	if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	  done_level_3 = user_maxlevel_3;
	}
	debug(printf("Pairing after 8A and 8B> found_score = %d, opt_level %d, done_level %d,%d\n",
		     *found_score,opt_level,done_level_5,done_level_3));
      }
    }

    if (alloc5p == true) {
      /* Clean up 5 */
      for (i = 0; i <= max_splice_mismatches_5; i++) {
	substringlist_gc(&(donors_plus_5[i]));
	substringlist_gc(&(antidonors_plus_5[i]));
	substringlist_gc(&(acceptors_plus_5[i]));
	substringlist_gc(&(antiacceptors_plus_5[i]));
	substringlist_gc(&(donors_minus_5[i]));
	substringlist_gc(&(antidonors_minus_5[i]));
	substringlist_gc(&(acceptors_minus_5[i]));
	substringlist_gc(&(antiacceptors_minus_5[i]));
      }
      FREE(donors_plus_5);
      FREE(antidonors_plus_5);
      FREE(acceptors_plus_5);
      FREE(antiacceptors_plus_5);
      FREE(donors_minus_5);
      FREE(antidonors_minus_5);
      FREE(acceptors_minus_5);
      FREE(antiacceptors_minus_5);
    }

    if (alloc3p == true) {
      /* Clean up 3 */
      for (i = 0; i <= max_splice_mismatches_3; i++) {
	substringlist_gc(&(donors_plus_3[i]));
	substringlist_gc(&(antidonors_plus_3[i]));
	substringlist_gc(&(acceptors_plus_3[i]));
	substringlist_gc(&(antiacceptors_plus_3[i]));
	substringlist_gc(&(donors_minus_3[i]));
	substringlist_gc(&(antidonors_minus_3[i]));
	substringlist_gc(&(acceptors_minus_3[i]));
	substringlist_gc(&(antiacceptors_minus_3[i]));
      }
      FREE(donors_plus_3);
      FREE(antidonors_plus_3);
      FREE(acceptors_plus_3);
      FREE(antiacceptors_plus_3);
      FREE(donors_minus_3);
      FREE(antidonors_minus_3);
      FREE(acceptors_minus_3);
      FREE(antiacceptors_minus_3);
    }
  }

  *hits5 = List_append(subs5,List_append(indels5,List_append(singlesplicing5,doublesplicing5)));
  *hits3 = List_append(subs3,List_append(indels3,List_append(singlesplicing3,doublesplicing3)));
  debug(printf("overall: 5' end has %d subs, %d indels, %d single splices, %d double splices\n",
	       List_length(subs5),List_length(indels5),List_length(singlesplicing5),List_length(doublesplicing5)));
  debug(printf("overall: 3' end has %d subs, %d indels, %d single splices, %d double splices\n",
	       List_length(subs3),List_length(indels3),List_length(singlesplicing3),List_length(doublesplicing3)));

  if (gmap_pairsearch_p == true) {
    debug(printf("Test for stage 9.  Comparing if found score %d >= trigger_score_for_gmap %d\n",
		 *found_score,trigger_score_for_gmap));

    /* 9A,B.  GMAP pairsearch/halfmapping/unpaired */
    /* Our previous test for doing GMAP was if nconcordant == 0, but
       could lead to a false positive concordant match. */
    /* Do not update nconcordant, because poor GMAP alignments can stop search for a distant splice */

    /* Relying upon trigger_score_for_gmap can occasionally lead to faulty concordant alignments.  However, running it on everything
       speed by half */
    if (*found_score >= trigger_score_for_gmap && *abort_pairing_p == false) {
      debug(printf("Stage 9.  Found score %d > %d.  Seeing if GMAP will help on %d + %d results\n",
		   *found_score,trigger_score_for_gmap,List_length(*hits5),List_length(*hits3)));

      /* Go ahead and resolve overlaps on each end by Stage3end, since
	 we cannot do it by Stage3pair, but do not apply optimal
	 score */
      debug(printf("Before remove_overlaps of 5' at cutoff level %d: %d hits\n",*cutoff_level_5,List_length(*hits5)));
      *hits5 = Stage3end_sort_bymatches(Stage3end_remove_overlaps(*hits5,/*finalp*/false));
      debug(printf("After remove_overlaps: %d\n",List_length(*hits5)));

      debug(printf("Before remove_overlaps of 3' at cutoff level %d: %d hits\n",*cutoff_level_3,List_length(*hits3)));
      *hits3 = Stage3end_sort_bymatches(Stage3end_remove_overlaps(*hits3,/*finalp*/false));
      debug(printf("After remove_overlaps: %d\n",List_length(*hits3)));

      /* Previously did pairsearch only if hits were limited, but affected by poor terminals */
      if (1 || List_length(*hits5) <= max_gmap_pairsearch) {
	i = 0;
	best_score_paired = Stage3end_best_score_paired(*hits5);
	debug13(printf("%d hits on 5' end\n",List_length(*hits5)));
	debug13(printf("For pairsearch, running GMAP on 3' end to match with 5' ends with score <= score %d\n",
		       best_score_paired));
	for (p = *hits5; p != NULL && i < max_gmap_pairsearch; p = List_next(p)) {
	  hit5 = (Stage3end_T) List_head(p);
	  if (Stage3end_hittype(hit5) == TRANSLOC_SPLICE) {
	    debug13(printf("No GMAP on transloc splice\n"));
	  } else if (Stage3end_paired_usedp(hit5) == false && Stage3end_score(hit5) <= best_score_paired) {
	    gmap3_hits = align_halfmapping_with_gmap(gmap_history_3,hit5,/*hit3*/NULL,queryseq5,queryseq3,
						     queryuc_ptr_3,/*querylength*/querylength3,query3_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
						     queryrc3,Shortread_invertedp(queryseq3),
#endif
						     query3_compress_fwd,query3_compress_rev,
						     this3->plus_segments,this3->plus_nsegments,this3->minus_segments,this3->minus_nsegments,
						     oligoindices_major,noligoindices_major,
						     oligoindices_minor,noligoindices_minor,
						     pairpool,diagpool,dynprogL,dynprogM,dynprogR,
						     pairmax,shortsplicedist,user_maxlevel_5,genestrand);
	    for (a = gmap3_hits; a != NULL; a = List_next(a)) {
	      gmap3 = (Stage3end_T) List_head(a);
	      debug13(printf("=> Successful pairsearch GMAP on hit3 with score %d and nmatches %d\n",
			     Stage3end_score(gmap3),Stage3end_nmatches(gmap3)));

	      if (Stage3end_score(gmap3) > *cutoff_level_3 + gmap_allowance) {
		/* nsalvage += 1; */
		debug13(printf("Score is only %d vs cutoff level %d\n",Stage3end_score(gmap3),*cutoff_level_3));
		Stage3end_free(&gmap3);

	      } else if ((newpair = Stage3pair_new(Stage3end_copy(hit5),gmap3,splicesites,
						   query5_compress_fwd,query5_compress_rev,
						   query3_compress_fwd,query3_compress_rev,genestrand,
						   /*pairtype*/CONCORDANT,localsplicing_penalty,
						   /*private5p*/true,/*private3p*/true,/*expect_concordant_p*/true)) == NULL) {
		debug13(printf(  "newpair is NULL\n"));
		/* Stage3end_free(&gmap3); -- done by Stage3pair_new */

	      } else if (Stage3end_hittype(hit5) != TERMINAL) {
		if (Stage3end_nmatches(gmap3) >= querylength3 - (*cutoff_level_3) &&
		    Stage3end_gmap_max_match_length(gmap3) >= querylength3/2) {
		  /* Want high standard for nconcordant, since this precludes finding terminals */
		  nconcordant += 1;
		  debug13(printf("High quality (nmatches %d >= querylength %d - cutoff level %d) => nconcordant %d\n",
				 Stage3end_nmatches(gmap3),querylength3,*cutoff_level_3,nconcordant));
		}
		hitpairs = List_push(hitpairs,(void *) newpair);
	      } else if (Stage3end_terminal_length(hit5) >= terminal_output_minlength) {
		if (Stage3end_nmatches(gmap3) >= querylength3 - (*cutoff_level_3) &&
		    Stage3end_gmap_max_match_length(gmap3) >= querylength3/2) {
		  /* Want high standard for nconcordant, since this precludes finding terminals */
		  nconcordant += 1;
		  debug13(printf("High quality (nmatches %d >= querylength %d - cutoff level %d) => nconcordant %d\n",
				 Stage3end_nmatches(gmap3),querylength3,*cutoff_level_3,nconcordant));
		}
		*with_terminal = List_push(*with_terminal,(void *) newpair);
	      } else {
		/* Stage3end_free(&gmap3); */
		Stage3pair_free(&newpair);
	      }
	    }
	    List_free(&gmap3_hits);
	    i++;
	  }
	}
      }

      /* Previously did pairsearch only if hits were limited, but affected by poor terminals */
      if (1 || List_length(*hits3) <= max_gmap_pairsearch) {
	i = 0;
	best_score_paired = Stage3end_best_score_paired(*hits3);
	debug13(printf("%d hits on 3' end\n",List_length(*hits3)));
	debug13(printf("For pairsearch, running GMAP on 5' end to match with 3' ends with score <= score %d\n",
		       best_score_paired));
	for (p = *hits3; p != NULL && i < max_gmap_pairsearch; p = List_next(p)) {
	  hit3 = (Stage3end_T) List_head(p);
	  if (Stage3end_hittype(hit3) == TRANSLOC_SPLICE) {
	    debug13(printf("Not GMAP on transloc splice\n"));
	  } else if (Stage3end_paired_usedp(hit3) == false && Stage3end_score(hit3) <= best_score_paired) {
	    gmap5_hits = align_halfmapping_with_gmap(gmap_history_5,/*hit5*/NULL,hit3,queryseq5,queryseq3,
						     queryuc_ptr_5,/*querylength*/querylength5,query5_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
						     queryrc5,Shortread_invertedp(queryseq5),
#endif
						     query5_compress_fwd,query5_compress_rev,
						     this5->plus_segments,this5->plus_nsegments,this5->minus_segments,this5->minus_nsegments,
						     oligoindices_major,noligoindices_major,
						     oligoindices_minor,noligoindices_minor,
						     pairpool,diagpool,dynprogL,dynprogM,dynprogR,
						     pairmax,shortsplicedist,user_maxlevel_5,genestrand);
	    for (a = gmap5_hits; a != NULL; a = List_next(a)) {
	      gmap5 = (Stage3end_T) List_head(a);
	      debug13(printf("=> Successful pairsearch GMAP on hit5 with score %d and nmatches %d\n",
			     Stage3end_score(gmap5),Stage3end_nmatches(gmap5)));

	      /* Stage3end_nmatches(gmap5) >= querylength5 - (*cutoff_level_5); */
	      if (Stage3end_score(gmap5) > *cutoff_level_5 + gmap_allowance) {
		/* nsalvage += 1; */
		debug13(printf("Score is only %d vs cutoff level %d\n",Stage3end_score(gmap5),*cutoff_level_5));
		Stage3end_free(&gmap5);

	      } else if ((newpair = Stage3pair_new(gmap5,Stage3end_copy(hit3),splicesites,
						   query5_compress_fwd,query5_compress_rev,
						   query3_compress_fwd,query3_compress_rev,genestrand,
						   /*pairtype*/CONCORDANT,localsplicing_penalty,
						   /*private5p*/true,/*private3p*/true,/*expect_concordant_p*/true)) == NULL) {
		debug13(printf(  "newpair is NULL\n"));
		/* Stage3end_free(&gmap5); -- done by Stage3pair_new */

	      } else if (Stage3end_hittype(hit3) != TERMINAL) {
		if (Stage3end_nmatches(gmap5) >= querylength5 - (*cutoff_level_5) &&
		    Stage3end_gmap_max_match_length(gmap5) >= querylength5/2) {
		  /* Want high standard for nconcordant, since this precludes finding terminals */
		  nconcordant += 1;
		  debug13(printf("High quality (nmatches %d >= querylength %d - cutoff level %d) => nconcordant %d\n",
				 Stage3end_nmatches(gmap5),querylength5,*cutoff_level_5,nconcordant));
		}
		hitpairs = List_push(hitpairs,(void *) newpair);
	      } else if (Stage3end_terminal_length(hit3) >= terminal_output_minlength) {
		if (Stage3end_nmatches(gmap5) >= querylength5 - (*cutoff_level_5) &&
		    Stage3end_gmap_max_match_length(gmap5) >= querylength5/2) {
		  /* Want high standard for nconcordant, since this precludes finding terminals */
		  nconcordant += 1;
		  debug13(printf("High quality (nmatches %d >= querylength %d - cutoff level %d) => nconcordant %d\n",
				 Stage3end_nmatches(gmap5),querylength5,*cutoff_level_5,nconcordant));
		}
		*with_terminal = List_push(*with_terminal,(void *) newpair);
	      } else {
		/* Stage3end_free(&gmap5); */
		Stage3pair_free(&newpair);
	      }
	    }
	    List_free(&gmap5_hits);
	    i++;
	  }
	}
      }
      debug(printf("9> After GMAP pairsearch, found %d concordant\n",nconcordant));
    }
  }


  /* 10. Distant splicing */
  /* We can allow nconcordant == 0 here, if we don't update
     nconcordant from GMAP hitpairs */
  if ((knownsplicingp || novelsplicingp) && nconcordant == 0 &&
      *abort_pairing_p == false) {

    if (done_level_5 >= distantsplicing_penalty) {
      /* Want >= and not >, because otherwise distant splicing does not work on 50-bp reads */
      /* Want > and not >=, because distant splicing needs to be better than other alternatives */
      max_splice_mismatches_5 = done_level_5 - distantsplicing_penalty;

      donors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antidonors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      acceptors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antiacceptors_plus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      donors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antidonors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      acceptors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));
      antiacceptors_minus_5 = (List_T *) CALLOC(max_splice_mismatches_5+1,sizeof(List_T));


      /* 10A.  Distant splicing */
      debug(printf("Starting find_spliceends (plus)\n"));
      find_spliceends_distant(&donors_plus_5,&antidonors_plus_5,&acceptors_plus_5,&antiacceptors_plus_5,
			      this5->plus_segments,this5->plus_nsegments,
#ifdef DEBUG4E
			      /*queryptr*/queryuc_ptr_5,
#endif
			      floors5,querylength5,query5_lastpos,/*query_compress*/query5_compress_fwd,
			      max_splice_mismatches_5,/*plusp*/true,genestrand);
      debug(printf("Finished find_spliceends (plus)\n"));

      debug(printf("Starting find_spliceends (minus)\n"));
      find_spliceends_distant(&antidonors_minus_5,&donors_minus_5,&antiacceptors_minus_5,&acceptors_minus_5,
			      this5->minus_segments,this5->minus_nsegments,
#ifdef DEBUG4E
			      /*queryptr*/queryrc5,
#endif
			      floors5,querylength5,query5_lastpos,/*query_compress*/query5_compress_rev,
			      max_splice_mismatches_5,/*plusp*/false,genestrand);
      debug(printf("Finished find_spliceends (minus)\n"));


      /* 10A.  Distant splicing */
      nmismatches = 0;
      while (longsinglesplicing5 == NULL &&
	     nmismatches <= max_splice_mismatches_5 /* && nsplicepairs5 < MAXCHIMERAPATHS */) {
	debug(printf("*** Stage 10A.  Distant splicing, allowing %d mismatches out of %d***\n",
		     nmismatches,max_splice_mismatches_5));
	
	debug4e(printf("Sorting splice ends\n"));
	donors_plus_5[nmismatches] = Substring_sort_chimera_halves(donors_plus_5[nmismatches],/*ascendingp*/true);
	acceptors_plus_5[nmismatches] = Substring_sort_chimera_halves(acceptors_plus_5[nmismatches],/*ascendingp*/true);

	antidonors_plus_5[nmismatches] = Substring_sort_chimera_halves(antidonors_plus_5[nmismatches],/*ascendingp*/false);
	antiacceptors_plus_5[nmismatches] = Substring_sort_chimera_halves(antiacceptors_plus_5[nmismatches],/*ascendingp*/false);

	donors_minus_5[nmismatches] = Substring_sort_chimera_halves(donors_minus_5[nmismatches],/*ascendingp*/false);
	acceptors_minus_5[nmismatches] = Substring_sort_chimera_halves(acceptors_minus_5[nmismatches],/*ascendingp*/false);

	antidonors_minus_5[nmismatches] = Substring_sort_chimera_halves(antidonors_minus_5[nmismatches],/*ascendingp*/true);
	antiacceptors_minus_5[nmismatches] = Substring_sort_chimera_halves(antiacceptors_minus_5[nmismatches],/*ascendingp*/true);

	debug4e(printf("Splice ends at %d nmismatches: +donors/acceptors %d/%d, +antidonors/antiacceptors %d/%d, -donors/acceptors %d/%d, -antidonors/antiacceptors %d/%d\n",
		       nmismatches,
		       List_length(donors_plus_5[nmismatches]),List_length(acceptors_plus_5[nmismatches]),
		       List_length(antidonors_plus_5[nmismatches]),List_length(antiacceptors_plus_5[nmismatches]),
		       List_length(donors_minus_5[nmismatches]),List_length(acceptors_minus_5[nmismatches]),
		       List_length(antidonors_minus_5[nmismatches]),List_length(antiacceptors_minus_5[nmismatches])));

	*hits5 = find_splicepairs_distant(&ignore_found_score,&nsplicepairs5,&longsinglesplicing5,*hits5,
					  donors_plus_5,antidonors_plus_5,acceptors_plus_5,antiacceptors_plus_5,
					  donors_minus_5,antidonors_minus_5,acceptors_minus_5,antiacceptors_minus_5,
					  localsplicing_penalty,distantsplicing_penalty,
					  querylength5,nmismatches,/*first_read_p*/true);
	nmismatches++;
      }

      if (longsinglesplicing5 != NULL) {
	*hits5 = List_append(*hits5,longsinglesplicing5);
      }

      /* Clean up 5 */
      for (i = 0; i <= max_splice_mismatches_5; i++) {
	substringlist_gc(&(donors_plus_5[i]));
	substringlist_gc(&(antidonors_plus_5[i]));
	substringlist_gc(&(acceptors_plus_5[i]));
	substringlist_gc(&(antiacceptors_plus_5[i]));
	substringlist_gc(&(donors_minus_5[i]));
	substringlist_gc(&(antidonors_minus_5[i]));
	substringlist_gc(&(acceptors_minus_5[i]));
	substringlist_gc(&(antiacceptors_minus_5[i]));
      }
      FREE(donors_plus_5);
      FREE(antidonors_plus_5);
      FREE(acceptors_plus_5);
      FREE(antiacceptors_plus_5);
      FREE(donors_minus_5);
      FREE(antidonors_minus_5);
      FREE(acceptors_minus_5);
      FREE(antiacceptors_minus_5);
    }

    if (done_level_3 >= distantsplicing_penalty) {
      /* Want >= and not >, because otherwise distant splicing does not work on 50-bp reads */
      /* Want > and not >=, because distant splicing needs to be better than other alternatives */
      max_splice_mismatches_3 = done_level_3 - distantsplicing_penalty;

      donors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antidonors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      acceptors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antiacceptors_plus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      donors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antidonors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      acceptors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));
      antiacceptors_minus_3 = (List_T *) CALLOC(max_splice_mismatches_3+1,sizeof(List_T));

      /* 10B.  Distant splicing */
      debug(printf("Starting find_spliceends (plus)\n"));
      find_spliceends_distant(&donors_plus_3,&antidonors_plus_3,&acceptors_plus_3,&antiacceptors_plus_3,
			      this3->plus_segments,this3->plus_nsegments,
#ifdef DEBUG4E
			      /*queryptr*/queryuc_ptr_3,
#endif
			      floors3,querylength3,query3_lastpos,/*query_compress*/query3_compress_fwd,
			      max_splice_mismatches_3,/*plusp*/true,genestrand);
      debug(printf("Finished find_spliceends (plus)\n"));

      debug(printf("Starting find_spliceends (minus)\n"));
      find_spliceends_distant(&antidonors_minus_3,&donors_minus_3,&antiacceptors_minus_3,&acceptors_minus_3,
			      this3->minus_segments,this3->minus_nsegments,
#ifdef DEBUG4E
			      /*queryptr*/queryrc3,
#endif
			      floors3,querylength3,query3_lastpos,/*query_compress*/query3_compress_rev,
			      max_splice_mismatches_3,/*plusp*/false,genestrand);
      debug(printf("Finished find_spliceends (minus)\n"));

      /* 10B.  Distant splicing */
      nmismatches = 0;
      while (longsinglesplicing3 == NULL &&
	     nmismatches <= max_splice_mismatches_3 /* && nsplicepairs3 < MAXCHIMERAPATHS */) {
	debug(printf("*** Stage 10B.  Distant splicing, allowing %d mismatches out of %d***\n",
		     nmismatches,max_splice_mismatches_3));

	debug4e(printf("Sorting splice ends\n"));
	donors_plus_3[nmismatches] = Substring_sort_chimera_halves(donors_plus_3[nmismatches],/*ascendingp*/true);
	acceptors_plus_3[nmismatches] = Substring_sort_chimera_halves(acceptors_plus_3[nmismatches],/*ascendingp*/true);

	antidonors_plus_3[nmismatches] = Substring_sort_chimera_halves(antidonors_plus_3[nmismatches],/*ascendingp*/false);
	antiacceptors_plus_3[nmismatches] = Substring_sort_chimera_halves(antiacceptors_plus_3[nmismatches],/*ascendingp*/false);

	donors_minus_3[nmismatches] = Substring_sort_chimera_halves(donors_minus_3[nmismatches],/*ascendingp*/false);
	acceptors_minus_3[nmismatches] = Substring_sort_chimera_halves(acceptors_minus_3[nmismatches],/*ascendingp*/false);

	antidonors_minus_3[nmismatches] = Substring_sort_chimera_halves(antidonors_minus_3[nmismatches],/*ascendingp*/true);
	antiacceptors_minus_3[nmismatches] = Substring_sort_chimera_halves(antiacceptors_minus_3[nmismatches],/*ascendingp*/true);

	debug4e(printf("Splice ends at %d nmismatches: +donors/acceptors %d/%d, +antidonors/antiacceptors %d/%d, -donors/acceptors %d/%d, -antidonors/antiacceptors %d/%d\n",
		       nmismatches,
		       List_length(donors_plus_3[nmismatches]),List_length(acceptors_plus_3[nmismatches]),
		       List_length(antidonors_plus_3[nmismatches]),List_length(antiacceptors_plus_3[nmismatches]),
		       List_length(donors_minus_3[nmismatches]),List_length(acceptors_minus_3[nmismatches]),
		       List_length(antidonors_minus_3[nmismatches]),List_length(antiacceptors_minus_3[nmismatches])));

	*hits3 = find_splicepairs_distant(&ignore_found_score,&nsplicepairs3,&longsinglesplicing3,*hits3,
					  donors_plus_3,antidonors_plus_3,acceptors_plus_3,antiacceptors_plus_3,
					  donors_minus_3,antidonors_minus_3,acceptors_minus_3,antiacceptors_minus_3,
					  localsplicing_penalty,distantsplicing_penalty,
					  querylength3,nmismatches,/*first_read_p*/false);
	nmismatches++;
      }

      if (longsinglesplicing3 != NULL) {
	*hits3 = List_append(*hits3,longsinglesplicing3);
      }

      /* Clean up 3 */
      for (i = 0; i <= max_splice_mismatches_3; i++) {
	substringlist_gc(&(donors_plus_3[i]));
	substringlist_gc(&(antidonors_plus_3[i]));
	substringlist_gc(&(acceptors_plus_3[i]));
	substringlist_gc(&(antiacceptors_plus_3[i]));
	substringlist_gc(&(donors_minus_3[i]));
	substringlist_gc(&(antidonors_minus_3[i]));
	substringlist_gc(&(acceptors_minus_3[i]));
	substringlist_gc(&(antiacceptors_minus_3[i]));
      }
      FREE(donors_plus_3);
      FREE(antidonors_plus_3);
      FREE(acceptors_plus_3);
      FREE(antiacceptors_plus_3);
      FREE(donors_minus_3);
      FREE(antidonors_minus_3);
      FREE(acceptors_minus_3);
      FREE(antiacceptors_minus_3);
    }

    /* 10.  Pairing after distant splicing */
    if (nconcordant == 0) {
      /* Note: cannot use hitarray after we have removed overlapping alignments */
      hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					   &(*samechr),&(*conc_transloc),&(*with_terminal),
					   hitpairs,/*hitarray5*/&(*hits5),/*narray5*/1,
					   /*hitarray3*/&(*hits3),/*narray3*/1,
					   /*terminals5*/NULL,/*terminals3*/NULL,
					   *cutoff_level_5,*cutoff_level_3,subopt_levels,
					   splicesites,query5_compress_fwd,query5_compress_rev,
					   query3_compress_fwd,query3_compress_rev,
					   querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					   genestrand);
      debug(printf("10> After pairing distant splicing, found %d concordant, %d samechr, found_score %d\n",
		   nconcordant,nsamechr,*found_score));

      if (*abort_pairing_p == false) {
	opt_level = (*found_score < opt_level) ? *found_score : opt_level;
	if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	  done_level_5 = user_maxlevel_5;
	}
	if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	  done_level_3 = user_maxlevel_3;
	}
	debug(printf("10> found_score = %d, opt_level %d, done_level %d,%d\n",*found_score,opt_level,done_level_5,done_level_3));
      }

    }
  }


  debug13(printf("nconcordant = %d\n",nconcordant));
  /* 11A,B.  Terminals */  /* Without nconcordant constraint, speed drops by 1/2 */
  if (nconcordant == 0 && /* nsalvage == 0 && */ *abort_pairing_p == false) {
    /* Previously used found_score > trigger_score_for_terminals */
    debug(printf("Stage 11.  nconcordant == 0.  Seeing if terminals will help\n"));

    if (done_level_5 >= terminal_threshold) {
      max_mismatches_allowed = done_level_5;
      debug(printf("Stage 11A.  Finding terminals5, done_level_5 = %d, terminal_threshold = %d\n",
		   done_level_5,terminal_threshold));
      if (floors5_computed_p == false) {
	floors5 = compute_floors(&any_omitted_p_5,&alloc_floors_p_5,floors_array,this5,
				 querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
      }

      if (segments5_computed_p == false) {
	this5->plus_segments = identify_all_segments_for_terminals(&this5->plus_nsegments,this5->plus_positions,this5->plus_npositions,
								   this5->omitted,querylength5,query5_lastpos,
								   floors5,max_mismatches_allowed,/*plusp*/true);
	this5->minus_segments = identify_all_segments_for_terminals(&this5->minus_nsegments,this5->minus_positions,this5->minus_npositions,
								    this5->omitted,querylength5,query5_lastpos,
								    floors5,max_mismatches_allowed,/*plusp*/false);
      }

      /* Don't run Stage3end_remove_duplicates until after concordant pairs are found, but do run Stage3end_remove_overlaps */
      terminals5 = find_terminals(this5->plus_segments,this5->plus_nsegments,this5->minus_segments,this5->minus_nsegments,
#ifdef DEBUG4T
				  queryuc_ptr_5,queryrc5,
#endif
				  floors5,querylength5,query5_lastpos,
				  query5_compress_fwd,query5_compress_rev,
				  max_mismatches_allowed,/*max_terminal_length*/end_miss_one,genestrand);
#if 0
      *hits5 = Stage3end_remove_overlaps(List_append(*hits5,terminals5),/*finalp*/false);
#endif
    }

    if (done_level_3 >= terminal_threshold) {
      max_mismatches_allowed = done_level_3;
      debug(printf("Stage 11B.  Finding terminals3, done_level_3 = %d, terminal_threshold = %d\n",
		   done_level_3,terminal_threshold));

      if (floors3_computed_p == false) {
	floors3 = compute_floors(&any_omitted_p_3,&alloc_floors_p_3,floors_array,this3,
				 querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
      }
      if (segments3_computed_p == false) {
	this3->plus_segments = identify_all_segments_for_terminals(&this3->plus_nsegments,this3->plus_positions,this3->plus_npositions,
								   this3->omitted,querylength3,query3_lastpos,
								   floors3,max_mismatches_allowed,/*plusp*/true);
	this3->minus_segments = identify_all_segments_for_terminals(&this3->minus_nsegments,this3->minus_positions,this3->minus_npositions,
								    this3->omitted,querylength3,query3_lastpos,
								    floors3,max_mismatches_allowed,/*plusp*/false);
      }

      /* Don't run Stage3end_remove_duplicates until after concordant pairs are found, but do run Stage3end_remove_overlaps */
      terminals3 = find_terminals(this3->plus_segments,this3->plus_nsegments,this3->minus_segments,this3->minus_nsegments,
#ifdef DEBUG4T
				  queryuc_ptr_3,queryrc3,
#endif
				  floors3,querylength3,query3_lastpos,
				  query3_compress_fwd,query3_compress_rev,
				  max_mismatches_allowed,/*max_terminal_length*/end_miss_one,genestrand);
#if 0
      *hits3 = Stage3end_remove_overlaps(List_append(*hits3,terminals3),/*finalp*/false);
#endif
    }

    if (terminals5 != NULL || terminals3 != NULL) {
      found_terminals_p = true;
      debug4t(printf("Running Stage3_pair_up_concordant\n"));
      /* Note: cannot use hitarray after we have removed overlapping alignments */
      hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					   &(*samechr),&(*conc_transloc),&(*with_terminal),
					   hitpairs,/*hitarray5*/&(*hits5),/*narray5*/1,
					   /*hitarray3*/&(*hits3),/*narray3*/1,
					   terminals5,terminals3,
					   *cutoff_level_5,*cutoff_level_3,subopt_levels,
					   splicesites,query5_compress_fwd,query5_compress_rev,
					   query3_compress_fwd,query3_compress_rev,
					   querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					   genestrand);
      debug(printf("11> After pairing terminals, found %d concordant, %d samechr, found_score %d\n",
		   nconcordant,nsamechr,*found_score));
      if (*abort_pairing_p == false) {
	opt_level = (*found_score < opt_level) ? *found_score : opt_level;
	if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	  done_level_5 = user_maxlevel_5;
	}
	if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	  done_level_3 = user_maxlevel_3;
	}
	debug(printf("Pairing after 11A and 11B> found_score = %d, opt_level %d, done_level %d,%d\n",
		     *found_score,opt_level,done_level_5,done_level_3));
      }
    }
  }


#if 0
  /* 12A,B.  Terminals.  Not sure why we had a second round of
     terminals.  Results on simulated test set are the same without
     this section. */
  if (nconcordant == 0 && nsalvage == 0 && *abort_pairing_p == false) {
    /* Previously used found_score > trigger_score_for_terminals */
    debug(printf("Stage 12.  nconcordant == 0.  Seeing if terminals 2 will help\n"));

    if (end_miss_one < querylength5/2 && done_level_5 >= terminal_threshold) {
      max_mismatches_allowed = done_level_5;
      debug(printf("Stage 12A.  Finding terminals5, done_level_5 = %d, terminal_threshold = %d\n",
		   done_level_5,terminal_threshold));
      if (floors5_computed_p == false) {
	floors5 = compute_floors(&any_omitted_p_5,&alloc_floors_p_5,floors_array,this5,
				 querylength5,query5_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
      }

      if (segments5_computed_p == false) {
	this5->plus_segments = identify_all_segments_for_terminals(&this5->plus_nsegments,this5->plus_positions,this5->plus_npositions,
								   this5->omitted,querylength5,query5_lastpos,
								   floors5,max_mismatches_allowed,/*plusp*/true);
	this5->minus_segments = identify_all_segments_for_terminals(&this5->minus_nsegments,this5->minus_positions,this5->minus_npositions,
								    this5->omitted,querylength5,query5_lastpos,
								    floors5,max_mismatches_allowed,/*plusp*/false);
      }

      /* Don't run Stage3end_remove_duplicates until after concordant pairs are found, but do run Stage3end_remove_overlaps */
      terminals5 = find_terminals(this5->plus_segments,this5->plus_nsegments,this5->minus_segments,this5->minus_nsegments,
#ifdef DEBUG4T
				  queryuc_ptr_5,queryrc5,
#endif
				  floors5,querylength5,query5_lastpos,
				  query5_compress_fwd,query5_compress_rev,
				  max_mismatches_allowed,/*max_terminal_length*/end_miss_two,genestrand);
#if 0
      *hits5 = Stage3end_remove_overlaps(List_append(*hits5,terminals5),/*finalp*/false);
#endif
    }


    if (end_miss_one < querylength3/2 && done_level_3 >= terminal_threshold) {
      max_mismatches_allowed = done_level_3;
      debug(printf("Stage 12B.  Finding terminals3, done_level_3 = %d, terminal_threshold = %d\n",
		   done_level_3,terminal_threshold));
      if (floors3_computed_p == false) {
	floors3 = compute_floors(&any_omitted_p_3,&alloc_floors_p_3,floors_array,this3,
				 querylength3,query3_lastpos,indexdb_fwd,indexdb_rev,indexdb_size_threshold,
				 max_end_insertions,/*omit_frequent_p*/true,/*omit_repetitive_p*/true,
				 keep_floors_p);
      }
      if (segments3_computed_p == false) {
	this3->plus_segments = identify_all_segments_for_terminals(&this3->plus_nsegments,this3->plus_positions,this3->plus_npositions,
								   this3->omitted,querylength3,query3_lastpos,
								   floors3,max_mismatches_allowed,/*plusp*/true);
	this3->minus_segments = identify_all_segments_for_terminals(&this3->minus_nsegments,this3->minus_positions,this3->minus_npositions,
								    this3->omitted,querylength3,query3_lastpos,
								    floors3,max_mismatches_allowed,/*plusp*/false);
      }

      /* Don't run Stage3end_remove_duplicates until after concordant pairs are found, but do run Stage3end_remove_overlaps */
      terminals3 = find_terminals(this3->plus_segments,this3->plus_nsegments,this3->minus_segments,this3->minus_nsegments,
#ifdef DEBUG4T
				  queryuc_ptr_3,queryrc3,
#endif
				  floors3,querylength3,query3_lastpos,
				  query3_compress_fwd,query3_compress_rev,
				  max_mismatches_allowed,/*max_terminal_length*/end_miss_two,genestrand);
#if 0
      *hits3 = Stage3end_remove_overlaps(List_append(*hits3,terminals3),/*finalp*/false);
#endif
    }

    if (terminals5 != NULL || terminals3 != NULL) {
      found_terminals_p = true;
      debug4t(printf("Running Stage3_pair_up_concordant\n"));
      /* Cannot use hitarray after we have removed overlapping alignments */
      hitpairs = Stage3_pair_up_concordant(&(*abort_pairing_p),&(*found_score),&nconcordant,&nsamechr,
					   &(*samechr),&(*conc_transloc),&(*with_terminal),
					   hitpairs,/*hitarray5*/&(*hits5),/*narray5*/1,
					   /*hitarray3*/&(*hits3),/*narray3*/1,
					   terminals5,terminals3,*cutoff_level_5,*cutoff_level_3,subopt_levels,
					   splicesites,query5_compress_fwd,query5_compress_rev,
					   query3_compress_fwd,query3_compress_rev,
					   querylength5,querylength3,maxpairedpaths,localsplicing_penalty,
					   genestrand);
      debug(printf("12> After pairing terminals, found %d concordant, %d samechr, found_score %d\n",
		   nconcordant,nsamechr,*found_score));

#if 0      
      /* Not needed at end */
      if (*abort_pairing_p == false) {
	opt_level = (*found_score < opt_level) ? *found_score : opt_level;
	if ((done_level_5 = opt_level + subopt_levels) > user_maxlevel_5) {
	  done_level_5 = user_maxlevel_5;
	}
	if ((done_level_3 = opt_level + subopt_levels) > user_maxlevel_3) {
	  done_level_3 = user_maxlevel_3;
	}
	debug(printf("Pairing after 12A and 12B> found_score = %d, opt_level %d, done_level %d,%d\n",
		     *found_score,opt_level,done_level_5,done_level_3));
      }
#endif

    }
  }
#endif


  debug13(printf("found_terminals_p = %d\n",found_terminals_p));
  /* nconcordant might include a concordant pair of terminals */
  if (/* nconcordant == 0 && */ found_terminals_p == true && gmap_terminal_p == true) {
    /* 13.  GMAP terminal */
    /* Go ahead and resolve overlaps on each end by Stage3end, since
       we cannot do it by Stage3pair, but do not apply optimal
       score */
#if 1
    debug13(printf("Before remove_overlaps of 5' at cutoff level %d: %d hits\n",*cutoff_level_5,List_length(*hits5)));
    *hits5 = Stage3end_sort_bymatches(Stage3end_remove_overlaps(*hits5,/*finalp*/false));
    debug13(printf("After remove_overlaps: %d\n",List_length(*hits5)));
    
    debug13(printf("Before remove_overlaps of 3' at cutoff level %d: %d hits\n",*cutoff_level_3,List_length(*hits3)));
    *hits3 = Stage3end_sort_bymatches(Stage3end_remove_overlaps(*hits3,/*finalp*/false));
    debug13(printf("After remove_overlaps: %d\n",List_length(*hits3)));
#else
    /* Focus on those terminals not yet processed */
    terminals5 = Stage3end_sort_by_paired_seenp(terminals5);
    terminals3 = Stage3end_sort_by_paired_seenp(terminals3);
#endif

    i = 0;
    debug13(printf("%d terminals on 5' end (vs max_gmap_terminal %d)\n",List_length(terminals5),max_gmap_terminal));
    debug13(printf("For terminals, running GMAP on 3' end to match with 5' ends\n"));
    terminals5 = Stage3end_sort_bymatches(terminals5);
    for (p = terminals5; p != NULL && i < max_gmap_terminal; p = List_next(p)) {
      hit5 = (Stage3end_T) List_head(p);
      debug13(printf("#%d/%d with nmatches %d\n",i,max_gmap_terminal,Stage3end_nmatches(hit5)));
      assert(Stage3end_hittype(hit5) == TERMINAL);
      if (1 || Stage3end_hittype(hit5) == TERMINAL /* && Stage3end_paired_usedp(hit5) == false && Stage3end_score(hit5) <= best_score_paired */) {
	gmap3_hits = align_halfmapping_with_gmap(gmap_history_3,hit5,/*hit3*/NULL,queryseq5,queryseq3,
						 queryuc_ptr_3,/*querylength*/querylength3,query3_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
						 queryrc3,Shortread_invertedp(queryseq3),
#endif
						 query3_compress_fwd,query3_compress_rev,
						 this3->plus_segments,this3->plus_nsegments,this3->minus_segments,this3->minus_nsegments,
						 oligoindices_major,noligoindices_major,
						 oligoindices_minor,noligoindices_minor,
						 pairpool,diagpool,dynprogL,dynprogM,dynprogR,
						 pairmax,shortsplicedist,user_maxlevel_3,genestrand);

	for (a = gmap3_hits; a != NULL; a = List_next(a)) {
	  gmap3 = (Stage3end_T) List_head(a);
	  debug13(printf("=> Successful terminal GMAP on hit3 %p with score %d and nmatches %d.  Copying hit5 %p\n",
			 gmap3,Stage3end_score(gmap3),Stage3end_nmatches(gmap3),hit5));
	  if (Stage3end_score(gmap3) > *cutoff_level_3 + gmap_allowance) {
	    debug13(printf("Score is only %d vs cutoff level %d\n",Stage3end_score(gmap3),*cutoff_level_3));
	    Stage3end_free(&gmap3);
	  } else if ((newpair = Stage3pair_new(Stage3end_copy(hit5),gmap3,splicesites,
					       query5_compress_fwd,query5_compress_rev,
					       query3_compress_fwd,query3_compress_rev,genestrand,
					       /*pairtype*/CONCORDANT,localsplicing_penalty,
					       /*private5p*/true,/*private3p*/true,/*expect_concordant_p*/true)) == NULL) {
	    /* Stage3end_free(&gmap3); -- done by Stage3pair_new */
	  } else if (Stage3end_terminal_length(hit5) >= terminal_output_minlength) {
	    /* Save hit5-gmap3 */
	    *with_terminal = List_push(*with_terminal,(void *) newpair);
	  } else {
	    /* Stage3end_free(&gmap3); */
	    Stage3pair_free(&newpair);
	  }
	}
	List_free(&gmap3_hits);
	i++;
      }
    }

    i = 0;
    debug13(printf("%d terminals on 3' end (vs max_gmap_terminal %d)\n",List_length(terminals3),max_gmap_terminal));
    debug13(printf("For terminals, running GMAP on 5' end to match with 3' ends\n"));
    terminals3 = Stage3end_sort_bymatches(terminals3);
    for (p = terminals3; p != NULL && i < max_gmap_terminal; p = List_next(p)) {
      hit3 = (Stage3end_T) List_head(p);
      debug13(printf("#%d/%d with nmatches %d\n",i,max_gmap_terminal,Stage3end_nmatches(hit3)));
      assert(Stage3end_hittype(hit3) == TERMINAL);
      if (1 || Stage3end_hittype(hit3) == TERMINAL /* && Stage3end_paired_usedp(hit3) == false && Stage3end_score(hit3) <= best_score_paired */) {
	gmap5_hits = align_halfmapping_with_gmap(gmap_history_5,/*hit5*/NULL,hit3,queryseq5,queryseq3,
						 queryuc_ptr_5,/*querylength*/querylength5,query5_lastpos,
#ifdef END_KNOWNSPLICING_SHORTCUT
						 queryrc5,Shortread_invertedp(queryseq5),
#endif
						 query5_compress_fwd,query5_compress_rev,
						 this5->plus_segments,this5->plus_nsegments,this5->minus_segments,this5->minus_nsegments,
						 oligoindices_major,noligoindices_major,
						 oligoindices_minor,noligoindices_minor,
						 pairpool,diagpool,dynprogL,dynprogM,dynprogR,
						 pairmax,shortsplicedist,user_maxlevel_5,genestrand);
	for (a = gmap5_hits; a != NULL; a = List_next(a)) {
	  gmap5 = (Stage3end_T) List_head(a);
	  debug13(printf("=> Successful terminal GMAP on hit5 %p with score %d and nmatches %d.  Copying hit3 %p\n",
			 hit5,Stage3end_score(gmap5),Stage3end_nmatches(gmap5),hit3));
	  if (Stage3end_score(gmap5) > *cutoff_level_5 + gmap_allowance) {
	    debug13(printf("Score is only %d vs cutoff level %d\n",Stage3end_score(gmap5),*cutoff_level_5));
	    Stage3end_free(&gmap5);

	  } else if ((newpair = Stage3pair_new(gmap5,Stage3end_copy(hit3),splicesites,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,genestrand,
					/*pairtype*/CONCORDANT,localsplicing_penalty,
					/*private5p*/true,/*private3p*/true,/*expect_concordant_p*/true)) == NULL) {
	    /* Stage3end_free(&gmap5); -- done by Stage3pair_new */
	  } else if (Stage3end_terminal_length(hit3) >= terminal_output_minlength) {
	    /* Save gmap5-hit3 */
	    *with_terminal = List_push(*with_terminal,(void *) newpair);
	  } else {
	    /* Stage3end_free(&gmap5); */
	    Stage3pair_free(&newpair);
	  }
	}
	List_free(&gmap5_hits);
	i++;
      }
    }

    debug(printf("13> After GMAP terminals, found %d concordant\n",nconcordant));
  }

  if (alloc_floors_p_5 == true) {
    Floors_free(&floors5);
  }
  if (alloc_floors_p_3 == true) {
    Floors_free(&floors3);
  }

  debug(printf("Ending with %d hitpairs, %d samechr, %d conc_transloc, %d with_terminal\n",
	       List_length(hitpairs),List_length(*samechr),List_length(*conc_transloc),List_length(*with_terminal)));

  /* Return terminals so they can be freed */
  if (terminals5 != NULL) {
    *hits5 = List_append(*hits5,terminals5);
  }
  if (terminals3 != NULL) {
    *hits3 = List_append(*hits3,terminals3);
  }

  return Stage3pair_remove_circular_alias(hitpairs);
}


static Pairtype_T
choose_among_paired (List_T hitpairs, List_T samechr, List_T conc_transloc, List_T with_terminal) {
  Pairtype_T final_pairtype = UNPAIRED;
  List_T p;
  Stage3pair_T hitpair;
  int best_nmatches = 0, nmatches;

  debug13(printf("choose: %d hitpairs, %d conc_transloc, %d with_terminal, %d samechr\n",
		 List_length(hitpairs),List_length(conc_transloc),List_length(with_terminal),List_length(samechr)));
  for (p = hitpairs; p != NULL; p = p->rest) {
    hitpair = (Stage3pair_T) p->first;
    if ((nmatches = Stage3pair_nmatches(hitpair)) > best_nmatches) {
      final_pairtype = CONCORDANT;
      best_nmatches = nmatches;
    }
  }

  best_nmatches += 1;		/* penalty for choosing translocation over others */

  for (p = conc_transloc; p != NULL; p = p->rest) {
    hitpair = (Stage3pair_T) p->first;
    if ((nmatches = Stage3pair_nmatches(hitpair)) > best_nmatches) {
      final_pairtype = CONCORDANT_TRANSLOCATIONS;
      best_nmatches = nmatches;
    }
  }

  for (p = with_terminal; p != NULL; p = p->rest) {
    hitpair = (Stage3pair_T) p->first;
    if ((nmatches = Stage3pair_nmatches(hitpair)) > best_nmatches) {
      final_pairtype = CONCORDANT_TERMINAL;
      best_nmatches = nmatches;
    }
  }

  for (p = samechr; p != NULL; p = p->rest) {
    hitpair = (Stage3pair_T) p->first;
    if ((nmatches = Stage3pair_nmatches(hitpair)) > best_nmatches) {
      final_pairtype = PAIRED_UNSPECIFIED;
      best_nmatches = nmatches;
    }
  }

  return final_pairtype;
}



/* Clean up all previous calculations */
static void
paired_results_free (T this5, T this3, List_T hitpairs, List_T samechr, List_T conc_transloc, List_T with_terminal,
		     List_T hits5, List_T hits3, int querylength5, int querylength3) {
  List_T p;
  Stage3pair_T stage3pair;

  for (p = hitpairs; p != NULL; p = List_next(p)) {
    stage3pair = (Stage3pair_T) List_head(p);
    Stage3pair_free(&stage3pair);
  }
  List_free(&hitpairs);

  for (p = samechr; p != NULL; p = List_next(p)) {
    stage3pair = (Stage3pair_T) List_head(p);
    Stage3pair_free(&stage3pair);
  }
  List_free(&samechr);

  for (p = conc_transloc; p != NULL; p = List_next(p)) {
    stage3pair = (Stage3pair_T) List_head(p);
    Stage3pair_free(&stage3pair);
  }
  List_free(&conc_transloc);

  for (p = with_terminal; p != NULL; p = List_next(p)) {
    stage3pair = (Stage3pair_T) List_head(p);
    Stage3pair_free(&stage3pair);
  }
  List_free(&with_terminal);

  stage3list_gc(&hits3);
  stage3list_gc(&hits5);
  Stage1_free(&this3,querylength3);
  Stage1_free(&this5,querylength5);

  return;
}

static void
realign_separately (Stage3end_T **stage3array5, int *nhits5, int *first_absmq5, int *second_absmq5,
		    Stage3end_T **stage3array3, int *nhits3, int *first_absmq3, int *second_absmq3,
		    History_T gmap_history_5, History_T gmap_history_3,
		    T this5, T this3, Compress_T query5_compress_fwd, Compress_T query5_compress_rev,
		    Compress_T query3_compress_fwd, Compress_T query3_compress_rev,
		    Shortread_T queryseq5, char *queryuc_ptr_5, char *queryrc5, char *quality_string_5, int querylength5, int query5_lastpos,
		    Shortread_T queryseq3, char *queryuc_ptr_3, char *queryrc3, char *quality_string_3, int querylength3, int query3_lastpos,
		    Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev, int indexdb_size_threshold,
		    Genome_T genome, Floors_T *floors_array,
		    int user_maxlevel_5, int user_maxlevel_3, int subopt_levels,
		    int indel_penalty_middle, int indel_penalty_end,
		    bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
		    int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
		    Oligoindex_T *oligoindices_major, int noligoindices_major,
		    Oligoindex_T *oligoindices_minor, int noligoindices_minor,
		    Pairpool_T pairpool, Diagpool_T diagpool,
		    Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		    bool keep_floors_p, int genestrand) {
  List_T singlehits5, singlehits3;
  int cutoff_level_5, cutoff_level_3;
  bool allvalidp5, allvalidp3;

  /* Re-align 5' end as a single end */
  if (read_oligos(&allvalidp5,this5,queryuc_ptr_5,querylength5,query5_lastpos,genestrand) == 0) {
    debug(printf("Aborting because no hits found anywhere\n"));
    singlehits5 = (List_T) NULL;
  } else {
    singlehits5 = align_end(&cutoff_level_5,gmap_history_5,this5,
			    query5_compress_fwd,query5_compress_rev,
			    queryuc_ptr_5,queryrc5,querylength5,query5_lastpos,
			    indexdb_fwd,indexdb_rev,indexdb_size_threshold,floors_array,
			    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			    pairpool,diagpool,dynprogL,dynprogM,dynprogR,
			    user_maxlevel_5,subopt_levels,
			    indel_penalty_middle,indel_penalty_end,
			    localsplicing_penalty,distantsplicing_penalty,min_shortend,
			    allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			    allvalidp5,keep_floors_p,genestrand);
  }

  if ((*nhits5 = List_length(singlehits5)) == 0) {
    *stage3array5 = (Stage3end_T *) NULL;
  } else {
    *stage3array5 = (Stage3end_T *) List_to_array_out(singlehits5,NULL); List_free(&singlehits5);
    *stage3array5 = Stage3end_eval_and_sort(&(*nhits5),&(*first_absmq5),&(*second_absmq5),
					    *stage3array5,maxpaths_search,queryseq5,
					    query5_compress_fwd,query5_compress_rev,
					    genome,quality_string_5,/*displayp*/true);
  }

  /* Re-align 3' end as a single end */
  if (read_oligos(&allvalidp3,this3,queryuc_ptr_3,querylength3,query3_lastpos,genestrand) == 0) {
    debug(printf("Aborting because no hits found anywhere\n"));
    singlehits3 = (List_T) NULL;
  } else {
    singlehits3 = align_end(&cutoff_level_3,gmap_history_3,this3,
			    query3_compress_fwd,query3_compress_rev,
			    queryuc_ptr_3,queryrc3,querylength3,query3_lastpos,
			    indexdb_fwd,indexdb_rev,indexdb_size_threshold,floors_array,
			    oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			    pairpool,diagpool,dynprogL,dynprogM,dynprogR,
			    user_maxlevel_3,subopt_levels,
			    indel_penalty_middle,indel_penalty_end,
			    localsplicing_penalty,distantsplicing_penalty,min_shortend,
			    allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			    allvalidp3,keep_floors_p,genestrand);
  }

  if ((*nhits3 = List_length(singlehits3)) == 0) {
    *stage3array3 = (Stage3end_T *) NULL;
  } else {
    *stage3array3 = (Stage3end_T *) List_to_array_out(singlehits3,NULL); List_free(&singlehits3);
    *stage3array3 = Stage3end_eval_and_sort(&(*nhits3),&(*first_absmq3),&(*second_absmq3),
					    *stage3array3,maxpaths_search,queryseq3,
					    query3_compress_fwd,query3_compress_rev,
					    genome,quality_string_3,/*displayp*/true);
  }

  return;
}


/* Have three lists: hitpairs, samechr, and conc_transloc => result */
static Stage3pair_T *
consolidate_paired_results (int *npaths, int *first_absmq, int *second_absmq, Pairtype_T *final_pairtype,
			    Stage3end_T **stage3array5, int *nhits5, int *first_absmq5, int *second_absmq5,
			    Stage3end_T **stage3array3, int *nhits3, int *first_absmq3, int *second_absmq3,
			    List_T hitpairs, List_T samechr, List_T conc_transloc, List_T with_terminal,
			    List_T hits5, List_T hits3, History_T gmap_history_5, History_T gmap_history_3,
			    Compress_T query5_compress_fwd, Compress_T query5_compress_rev,
			    Compress_T query3_compress_fwd, Compress_T query3_compress_rev,
			    struct Segment_T **plus_segments_genestrand_5, int *plus_nsegments_genestrand_5,
			    struct Segment_T **minus_segments_genestrand_5, int *minus_nsegments_genestrand_5,
			    struct Segment_T **plus_segments_genestrand_3, int *plus_nsegments_genestrand_3,
			    struct Segment_T **minus_segments_genestrand_3, int *minus_nsegments_genestrand_3,
			    Shortread_T queryseq5, char *queryuc_ptr_5, char *quality_string_5, int querylength5, int query5_lastpos,
			    Shortread_T queryseq3, char *queryuc_ptr_3, char *quality_string_3, int querylength3, int query3_lastpos,
			    Genome_T genome, int subopt_levels, int cutoff_level_5, int cutoff_level_3,
			    int localsplicing_penalty,
			    Oligoindex_T *oligoindices_major, int noligoindices_major,
			    Oligoindex_T *oligoindices_minor, int noligoindices_minor,
			    Pairpool_T pairpool, Diagpool_T diagpool,
			    Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
			    Chrpos_T pairmax, int user_maxlevel_5, int user_maxlevel_3) {
  Stage3pair_T *stage3pairarray, stage3pair, newpair;
  Stage3end_T hit5, hit3;
  List_T result, singlehits5, singlehits3, p;
  Pairtype_T pairtype;

  
  debug16(printf("Entered consolidate_paired_results\n"));
  *final_pairtype = choose_among_paired(hitpairs,samechr,conc_transloc,with_terminal);

  if (*final_pairtype == CONCORDANT) {
    /* Have concordant results */
    debug16(printf("Have %d concordant results\n",List_length(hitpairs)));
    for (p = samechr; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&samechr);

    for (p = conc_transloc; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&conc_transloc);
	  
    for (p = with_terminal; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&with_terminal);

    if (novelsplicingp || knownsplicingp) {
      hitpairs = Stage3pair_remove_excess_terminals(hitpairs);
    }

    if (gmap_improvement_p == false) {
      debug16(printf("No GMAP improvement: Before removing overlaps, %d results\n",List_length(hitpairs)));
      result = Stage3pair_optimal_score(hitpairs,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/true);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/true);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/true);
      result = Stage3pair_resolve_multimapping(result);
      /* result = Stage3pair_sort_distance(result); */
      debug16(printf("After removing overlaps, %d results\n",List_length(result)));

    } else {
      debug16(printf("GMAP improvement: Before removing overlaps, %d results.  Starting with run Stage3pair_optimal_score (1)\n",
		     List_length(hitpairs)));
      result = Stage3pair_optimal_score(hitpairs,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/false);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/false);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/false);
      result = Stage3pair_resolve_multimapping(result);
      /* result = Stage3pair_sort_distance(result); */
      debug16(printf("After removing overlaps, %d results.  Starting align_pair_with_gmap\n",List_length(result)));

      result = align_pair_with_gmap(&(*final_pairtype),result,gmap_history_5,gmap_history_3,
				    query5_compress_fwd,query5_compress_rev,
				    query3_compress_fwd,query3_compress_rev,
				    plus_segments_genestrand_5,plus_nsegments_genestrand_5,
				    minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				    plus_segments_genestrand_3,plus_nsegments_genestrand_3,
				    minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				    queryseq5,queryseq3,queryuc_ptr_5,queryuc_ptr_3,
				    querylength5,querylength3,query5_lastpos,query3_lastpos,
				    localsplicing_penalty,
				    oligoindices_major,noligoindices_major,
				    oligoindices_minor,noligoindices_minor,
				    pairpool,diagpool,dynprogL,dynprogM,dynprogR,
				    pairmax,user_maxlevel_5,user_maxlevel_3,cutoff_level_5,cutoff_level_3,
				    /*pairtype*/CONCORDANT,/*expect_concordant_p*/true,
				    /*redo_for_sense_p*/false);
      debug16(printf("Finished with align_pair_with_gmap, round 1\n"));

      if (Stage3pair_sense_consistent_p(result) == false) {
	debug16(printf("Starting align_pair_with_gmap to preserve sense\n"));
	result = align_pair_with_gmap(&(*final_pairtype),result,gmap_history_5,gmap_history_3,
				      query5_compress_fwd,query5_compress_rev,
				      query3_compress_fwd,query3_compress_rev,
				      plus_segments_genestrand_5,plus_nsegments_genestrand_5,
				      minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				      plus_segments_genestrand_3,plus_nsegments_genestrand_3,
				      minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				      queryseq5,queryseq3,queryuc_ptr_5,queryuc_ptr_3,
				      querylength5,querylength3,query5_lastpos,query3_lastpos,
				      localsplicing_penalty,
				      oligoindices_major,noligoindices_major,
				      oligoindices_minor,noligoindices_minor,
				      pairpool,diagpool,dynprogL,dynprogM,dynprogR,
				      pairmax,user_maxlevel_5,user_maxlevel_3,cutoff_level_5,cutoff_level_3,
				      /*pairtype*/CONCORDANT,/*expect_concordant_p*/true,
				      /*redo_for_sense_p*/true);
	debug16(printf("Finished with align_pair_with_gmap, round 2\n"));
      }

      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/true);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/true);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/true);
      result = Stage3pair_resolve_multimapping(result);
    }

  } else if (*final_pairtype == PAIRED_UNSPECIFIED) {
    /* Have paired results */
    debug16(printf("Have paired results\n"));
    for (p = hitpairs; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&hitpairs);

    for (p = conc_transloc; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&conc_transloc);

    for (p = with_terminal; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&with_terminal);

    if (gmap_improvement_p == false) {
      debug16(printf("No GMAP improvement: Before removing overlaps, %d results\n",List_length(samechr)));
      result = Stage3pair_optimal_score(samechr,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/true);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/true);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/true);
      result = Stage3pair_resolve_multimapping(result);
    } else {
      debug16(printf("GMAP improvement: Before removing overlaps, %d results.  Starting to run Stage3pair_optimal_score (2)\n",
		     List_length(samechr)));
      result = Stage3pair_optimal_score(samechr,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/false);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/false);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/false);
      result = Stage3pair_resolve_multimapping(result);

      result = align_pair_with_gmap(&(*final_pairtype),result,gmap_history_5,gmap_history_3,
				    query5_compress_fwd,query5_compress_rev,
				    query3_compress_fwd,query3_compress_rev,
				    plus_segments_genestrand_5,plus_nsegments_genestrand_5,
				    minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				    plus_segments_genestrand_3,plus_nsegments_genestrand_3,
				    minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				    queryseq5,queryseq3,queryuc_ptr_5,queryuc_ptr_3,
				    querylength5,querylength3,query5_lastpos,query3_lastpos,
				    localsplicing_penalty,
				    oligoindices_major,noligoindices_major,
				    oligoindices_minor,noligoindices_minor,
				    pairpool,diagpool,dynprogL,dynprogM,dynprogR,
				    pairmax,user_maxlevel_5,user_maxlevel_3,cutoff_level_5,cutoff_level_3,
				    /*pairtype*/PAIRED_UNSPECIFIED,/*expect_concordant_p*/false,
				    /*redo_for_sense_p*/false);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/true);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/true);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/true);
      result = Stage3pair_resolve_multimapping(result);

      if (Stage3pair_concordantp(result) == true) {
	debug16(printf("Found remaining concordant solution, so removing non-concordant ones\n"));
	*final_pairtype = CONCORDANT;
	result = Stage3pair_filter_nonconcordant(result);
      } else {
	*final_pairtype = PAIRED_UNSPECIFIED;
      }
    }

  } else if (*final_pairtype == CONCORDANT_TRANSLOCATIONS) {
    debug16(printf("Have %d concordant translocation results\n",List_length(conc_transloc)));
    for (p = hitpairs; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&hitpairs);

    for (p = samechr; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&samechr);

    for (p = with_terminal; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&with_terminal);

    result = Stage3pair_optimal_score(conc_transloc,/*cutoff*/1000000,subopt_levels,
				      query5_compress_fwd,query5_compress_rev,
				      query3_compress_fwd,query3_compress_rev,
				      /*keep_gmap_p*/true,/*finalp*/true);
    result = Stage3pair_remove_overlaps(result,/*translocp*/true,/*finalp*/true);
    result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
				      query5_compress_fwd,query5_compress_rev,
				      query3_compress_fwd,query3_compress_rev,
				      /*keep_gmap_p*/false,/*finalp*/true);
    result = Stage3pair_resolve_multimapping(result);
    debug16(printf("Finally, have %d concordant translocation results\n",List_length(result)));

  } else if (*final_pairtype == CONCORDANT_TERMINAL) {
    debug16(printf("Have concordant terminal results\n"));
    *final_pairtype = CONCORDANT; /* CONCORDANT_TERMINAL used just to rank results behind translocations */

    for (p = hitpairs; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&hitpairs);

    for (p = samechr; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&samechr);

    for (p = conc_transloc; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&conc_transloc);

    if (novelsplicingp || knownsplicingp) {
      with_terminal = Stage3pair_remove_excess_terminals(with_terminal);
    }

    if (gmap_improvement_p == false) {
      debug16(printf("No GMAP improvement: Before removing overlaps, %d results\n",List_length(with_terminal)));
      result = Stage3pair_optimal_score(with_terminal,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/true);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/true);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/true);
      result = Stage3pair_resolve_multimapping(result);
      /* result = Stage3pair_sort_distance(result); */
      debug16(printf("After removing overlaps, %d results\n",List_length(result)));

    } else {
      debug16(printf("GMAP improvement: Before removing overlaps, %d results.  Starting to run Stage3pair_optimal_score (3)\n",
		     List_length(with_terminal)));
      result = Stage3pair_optimal_score(with_terminal,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/false);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/false);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/false);
      result = Stage3pair_resolve_multimapping(result);
      /* result = Stage3pair_sort_distance(result); */
      debug16(printf("After removing overlaps, %d results.  Starting align_pair_with_gmap, round 1\n",List_length(result)));

      result = align_pair_with_gmap(&(*final_pairtype),result,gmap_history_5,gmap_history_3,
				    query5_compress_fwd,query5_compress_rev,
				    query3_compress_fwd,query3_compress_rev,
				    plus_segments_genestrand_5,plus_nsegments_genestrand_5,
				    minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				    plus_segments_genestrand_3,plus_nsegments_genestrand_3,
				    minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				    queryseq5,queryseq3,queryuc_ptr_5,queryuc_ptr_3,
				    querylength5,querylength3,query5_lastpos,query3_lastpos,
				    localsplicing_penalty,
				    oligoindices_major,noligoindices_major,
				    oligoindices_minor,noligoindices_minor,
				    pairpool,diagpool,dynprogL,dynprogM,dynprogR,
				    pairmax,user_maxlevel_5,user_maxlevel_3,cutoff_level_5,cutoff_level_3,
				    /*pairtype*/CONCORDANT,/*expect_concordant_p*/true,
				    /*redo_for_sense_p*/false);
      if (Stage3pair_sense_consistent_p(result) == false) {
	result = align_pair_with_gmap(&(*final_pairtype),result,gmap_history_5,gmap_history_3,
				      query5_compress_fwd,query5_compress_rev,
				      query3_compress_fwd,query3_compress_rev,
				      plus_segments_genestrand_5,plus_nsegments_genestrand_5,
				      minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				      plus_segments_genestrand_3,plus_nsegments_genestrand_3,
				      minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				      queryseq5,queryseq3,queryuc_ptr_5,queryuc_ptr_3,
				      querylength5,querylength3,query5_lastpos,query3_lastpos,
				      localsplicing_penalty,
				      oligoindices_major,noligoindices_major,
				      oligoindices_minor,noligoindices_minor,
				      pairpool,diagpool,dynprogL,dynprogM,dynprogR,
				      pairmax,user_maxlevel_5,user_maxlevel_3,cutoff_level_5,cutoff_level_3,
				      /*pairtype*/CONCORDANT,/*expect_concordant_p*/true,
				      /*redo_for_sense_p*/true);
      }

      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/true,/*finalp*/true);
      result = Stage3pair_remove_overlaps(result,/*translocp*/false,/*finalp*/true);
      result = Stage3pair_optimal_score(result,/*cutoff*/1000000,subopt_levels,
					query5_compress_fwd,query5_compress_rev,
					query3_compress_fwd,query3_compress_rev,
					/*keep_gmap_p*/false,/*finalp*/true);
      result = Stage3pair_resolve_multimapping(result);
    }

  } else {
    debug16(printf("Have unpaired results\n"));
    /* Need to free conc_transloc, since we can get here with multiple results */
    for (p = conc_transloc; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&conc_transloc);

    for (p = with_terminal; p != NULL; p = List_next(p)) {
      stage3pair = (Stage3pair_T) List_head(p);
      Stage3pair_free(&stage3pair);
    }
    List_free(&with_terminal);

    result = (List_T) NULL;
  }


  if (result == NULL) {
    singlehits5 = Stage3end_optimal_score(hits5,cutoff_level_5,subopt_levels,query5_compress_fwd,query5_compress_rev,
					  /*keep_gmap_p*/true,/*finalp*/true);
    singlehits5 = Stage3end_linearize_5(singlehits5);
    singlehits5 = Stage3end_remove_overlaps(singlehits5,/*finalp*/true);
    singlehits5 = Stage3end_optimal_score(singlehits5,cutoff_level_5,subopt_levels,query5_compress_fwd,query5_compress_rev,
					  /*keep_gmap_p*/false,/*finalp*/true);
    singlehits5 = Stage3end_resolve_multimapping(singlehits5);

    singlehits3 = Stage3end_optimal_score(hits3,cutoff_level_3,subopt_levels,query3_compress_fwd,query3_compress_rev,
					  /*keep_gmap_p*/true,/*finalp*/true);
    singlehits3 = Stage3end_linearize_3(singlehits3);
    singlehits3 = Stage3end_remove_overlaps(singlehits3,/*finalp*/true);
    singlehits3 = Stage3end_optimal_score(singlehits3,cutoff_level_3,subopt_levels,query3_compress_fwd,query3_compress_rev,
					  /*keep_gmap_p*/false,/*finalp*/true);
    singlehits3 = Stage3end_resolve_multimapping(singlehits3);

    debug16(printf("5' end has %d hits and 3' end has %d hits\n",
		   List_length(singlehits5),List_length(singlehits3)));

    if (List_length(singlehits5) == 1 && List_length(singlehits3) == 1 &&
	(pairtype = Stage3_determine_pairtype(hit5=(Stage3end_T) List_head(singlehits5),hit3=(Stage3end_T) List_head(singlehits3))) != UNPAIRED) {
      /* Convert unpaired uniq to a paired uniq */
      debug16(printf("Converting unpaired uniq to paired uniq, with initial pairtype %s\n",Pairtype_string(pairtype)));
      if ((newpair = Stage3pair_new(hit5,hit3,splicesites,
				    query5_compress_fwd,query5_compress_rev,
				    query3_compress_fwd,query3_compress_rev,
				    /*genestrand*/0,pairtype,localsplicing_penalty,
				    /*private5p*/false,/*private3p*/false,
				    /*expect_concordant_p*/pairtype == CONCORDANT ? true : false)) != NULL) {
	stage3pairarray = (Stage3pair_T *) CALLOC_OUT(1,sizeof(Stage3pair_T));
	stage3pairarray[0] = newpair;
	    
	*nhits5 = *nhits3 = 0;
	*stage3array5 = *stage3array3 = (Stage3end_T *) NULL;
	    
	*npaths = 1;
	if (pairtype == CONCORDANT) {
	  debug16(printf("final pairtype is CONCORDANT\n"));
	  *final_pairtype = CONCORDANT;
	} else {
	  debug16(printf("final pairtype is PAIRED_UNSPECIFIED\n"));
	  *final_pairtype = PAIRED_UNSPECIFIED;
	}
	Stage3pair_privatize(stage3pairarray,/*npairs*/1);
	Stage3pair_eval_and_sort(&(*npaths),&(*first_absmq),&(*second_absmq),
				 stage3pairarray,maxpaths_search,queryseq5,queryseq3,
				 query5_compress_fwd,query5_compress_rev,
				 query3_compress_fwd,query3_compress_rev,
				 genome,quality_string_5,quality_string_3);
	    
	stage3list_gc(&singlehits3);
	stage3list_gc(&singlehits5);
	return stage3pairarray;
      }
    }

    /* Fall through: halfmapping or unpaired */
    *npaths = 0;
    *final_pairtype = UNPAIRED;
	  
    if ((*nhits5 = List_length(singlehits5)) == 0) {
      *stage3array5 = (Stage3end_T *) NULL;
    } else {
#if 0
      singlehits5 = Stage3end_unalias_circular(singlehits5);
#else
      singlehits5 = Stage3end_remove_circular_alias(singlehits5); /* Contains a call to unalias_circular */
#endif
      *stage3array5 = (Stage3end_T *) List_to_array_out(singlehits5,NULL); List_free(&singlehits5);
    }

    if ((*nhits3 = List_length(singlehits3)) == 0) {
      *stage3array3 = (Stage3end_T *) NULL;
    } else {
#if 0
      singlehits3 = Stage3end_unalias_circular(singlehits3);
#else
      singlehits3 = Stage3end_remove_circular_alias(singlehits3); /* Contains a call to unalias_circular */
#endif
      *stage3array3 = (Stage3end_T *) List_to_array_out(singlehits3,NULL); List_free(&singlehits3);
    }

    if (*nhits5 > 0) {
      if (*nhits3 == 1) {
	/* Use single 3' hit to guide sorting of multiple 5' hits */
	*stage3array5 = Stage3end_eval_and_sort_guided(&(*nhits5),&(*first_absmq5),&(*second_absmq5),
						       /*guide*/(*stage3array3)[0],
						       *stage3array5,maxpaths_search,queryseq5,
						       query5_compress_fwd,query5_compress_rev,
						       genome,quality_string_5,/*displayp*/true);
      } else {
	*stage3array5 = Stage3end_eval_and_sort(&(*nhits5),&(*first_absmq5),&(*second_absmq5),
						*stage3array5,maxpaths_search,queryseq5,
						query5_compress_fwd,query5_compress_rev,
						genome,quality_string_5,/*displayp*/true);
      }
    }

    if (*nhits3 > 0) {
      if (*nhits5 == 1) {
	/* Use single 5' hit to guide sorting of multiple 3' hits */
	*stage3array3 = Stage3end_eval_and_sort_guided(&(*nhits3),&(*first_absmq3),&(*second_absmq3),
						       /*guide*/(*stage3array5)[0],
						       *stage3array3,maxpaths_search,queryseq3,
						       query3_compress_fwd,query3_compress_rev,
						       genome,quality_string_3,/*displayp*/true);
      } else {
	*stage3array3 = Stage3end_eval_and_sort(&(*nhits3),&(*first_absmq3),&(*second_absmq3),
						*stage3array3,maxpaths_search,queryseq3,
						query3_compress_fwd,query3_compress_rev,
						genome,quality_string_3,/*displayp*/true);
      }
    }
    debug16(printf("Result is NULL, and we have %d hits on 5' end and %d hits on 3' end\n",*nhits5,*nhits3));
    return (Stage3pair_T *) NULL;

  } else {
    /* result != NULL */
    /* Concordant, paired, or transloc pairs found.  Remove single hits. */
    debug16(printf("Result is not NULL (%d paths), and we fall through to concordant, paired, or transloc pairs\n",
		   List_length(result)));

    *npaths = List_length(result);
    stage3pairarray = (Stage3pair_T *) List_to_array_out(result,NULL); List_free(&result);
    Stage3pair_privatize(stage3pairarray,*npaths);
    Stage3pair_eval_and_sort(&(*npaths),&(*first_absmq),&(*second_absmq),
			     stage3pairarray,maxpaths_search,queryseq5,queryseq3,
			     query5_compress_fwd,query5_compress_rev,
			     query3_compress_fwd,query3_compress_rev,
			     genome,quality_string_5,quality_string_3);

    stage3list_gc(&hits3);
    stage3list_gc(&hits5);

    *nhits5 = *nhits3 = 0;
    *stage3array5 = *stage3array3 = (Stage3end_T *) NULL;
    return stage3pairarray;
  }
}


static Stage3pair_T *
paired_read (int *npaths, int *first_absmq, int *second_absmq, Pairtype_T *final_pairtype,
	     Stage3end_T **stage3array5, int *nhits5, int *first_absmq5, int *second_absmq5,
	     Stage3end_T **stage3array3, int *nhits3, int *first_absmq3, int *second_absmq3,
	     Shortread_T queryseq5, Shortread_T queryseq3,
	     Indexdb_T indexdb_fwd, Indexdb_T indexdb_rev, int indexdb_size_threshold,
	     Genome_T genome, Floors_T *floors_array,
	     double user_maxlevel_float, int subopt_levels,
	     int indel_penalty_middle, int indel_penalty_end,
	     bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
	     int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
	     Oligoindex_T *oligoindices_major, int noligoindices_major,
	     Oligoindex_T *oligoindices_minor, int noligoindices_minor,
	     Pairpool_T pairpool, Diagpool_T diagpool,
	     Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
	     Chrpos_T pairmax, bool keep_floors_p) {
  Stage3pair_T *stage3pairarray;
  List_T hitpairs = NULL, samechr = NULL, conc_transloc = NULL, with_terminal = NULL,
    hits5 = NULL, hits3 = NULL;
  T this5, this3;
  char *queryuc_ptr_5, *queryuc_ptr_3, *quality_string_5, *quality_string_3;
  char queryrc5[MAX_READLENGTH+1], queryrc3[MAX_READLENGTH+1];
  Compress_T query5_compress_fwd = NULL, query5_compress_rev = NULL, query3_compress_fwd = NULL, query3_compress_rev = NULL;
  History_T gmap_history_5, gmap_history_3;
  int user_maxlevel_5, user_maxlevel_3;
  int found_score, cutoff_level_5, cutoff_level_3;
  int querylength5, querylength3, query5_lastpos, query3_lastpos;
  int noligos5, noligos3;
  bool allvalidp5, allvalidp3;
#if 0
  int maxpairedpaths = 10*maxpaths; /* For computation, not for printing. */
#else
  int maxpairedpaths = maxpaths_search;  /* 100000 */
#endif
  bool abort_pairing_p;


  querylength5 = Shortread_fulllength(queryseq5);
  querylength3 = Shortread_fulllength(queryseq3);

  if (querylength5 < min_readlength || querylength3 < min_readlength) {
    fprintf(stderr,"GSNAP cannot handle reads shorter than %d bp with this k-mer = %d genomic index.\n",
	    min_readlength,index1part);
    fprintf(stderr,"You may want to build a genomic index with a smaller k-mer value using the -k flag to gmap_build\n");
    *npaths = *nhits5 = *nhits3 = 0;
    *stage3array5 = *stage3array3 = (Stage3end_T *) NULL;
    return (Stage3pair_T *) NULL;

  } else if (querylength5 > MAX_READLENGTH || querylength3 > MAX_READLENGTH) {
    fprintf(stderr,"GSNAP cannot handle reads longer than %d bp.  Either run configure and make again with a higher value of MAX_READLENGTH, or consider using GMAP instead.\n",
	    MAX_READLENGTH);
    *npaths = *nhits5 = *nhits3 = 0;
    *stage3array5 = *stage3array3 = (Stage3end_T *) NULL;
    return (Stage3pair_T *) NULL;

  } else {
    if (user_maxlevel_float < 0.0) {
      user_maxlevel_5 = user_maxlevel_3 = -1;
    } else if (user_maxlevel_float > 0.0 && user_maxlevel_float < 1.0) {
      user_maxlevel_5 = (int) rint(user_maxlevel_float * (double) querylength5);
      user_maxlevel_3 = (int) rint(user_maxlevel_float * (double) querylength3);
    } else {
      user_maxlevel_5 = user_maxlevel_3 = (int) user_maxlevel_float;
    }

    this5 = Stage1_new(querylength5);
    this3 = Stage1_new(querylength3);
    queryuc_ptr_5 = Shortread_fullpointer_uc(queryseq5);
    queryuc_ptr_3 = Shortread_fullpointer_uc(queryseq3);
    quality_string_5 = Shortread_quality_string(queryseq5);
    quality_string_3 = Shortread_quality_string(queryseq3);
    query5_lastpos = querylength5 - index1part;
    query3_lastpos = querylength3 - index1part;

    /* Limit search on repetitive sequences */
    if (check_dinucleotides(queryuc_ptr_5,querylength5) == false) {
      user_maxlevel_5 = 0;
    }
    if (check_dinucleotides(queryuc_ptr_3,querylength3) == false) {
      user_maxlevel_3 = 0;
    }

    noligos5 = read_oligos(&allvalidp5,this5,queryuc_ptr_5,querylength5,query5_lastpos,/*genestrand*/0);
    noligos3 = read_oligos(&allvalidp3,this3,queryuc_ptr_3,querylength3,query3_lastpos,/*genestrand*/0);
    if (noligos5 == 0 && noligos3 == 0) {
      debug(printf("Aborting because no hits found anywhere\n"));
      Stage1_free(&this3,querylength3);
      Stage1_free(&this5,querylength5);

      *npaths = *nhits5 = *nhits3 = 0;
      *stage3array5 = *stage3array3 = (Stage3end_T *) NULL;
      return (Stage3pair_T *) NULL;

    } else {
      abort_pairing_p = false;

      query5_compress_fwd = Compress_new_fwd(queryuc_ptr_5,querylength5);
      query5_compress_rev = Compress_new_rev(queryuc_ptr_5,querylength5);
      query3_compress_fwd = Compress_new_fwd(queryuc_ptr_3,querylength3);
      query3_compress_rev = Compress_new_rev(queryuc_ptr_3,querylength3);
      gmap_history_5 = History_new();
      gmap_history_3 = History_new();
      make_complement_buffered(queryrc5,queryuc_ptr_5,querylength5);
      make_complement_buffered(queryrc3,queryuc_ptr_3,querylength3);

      hitpairs = align_pair(&abort_pairing_p,&found_score,&cutoff_level_5,&cutoff_level_3,
			    &samechr,&conc_transloc,&with_terminal,gmap_history_5,gmap_history_3,
			    &hits5,&hits3,this5,this3,query5_compress_fwd,query5_compress_rev,
			    query3_compress_fwd,query3_compress_rev,
			    queryuc_ptr_5,queryuc_ptr_3,queryrc5,queryrc3,
			    querylength5,querylength3,query5_lastpos,query3_lastpos,
			    indexdb_fwd,indexdb_rev,indexdb_size_threshold,floors_array,

			    oligoindices_major,noligoindices_major,
			    oligoindices_minor,noligoindices_minor,
			    pairpool,diagpool,dynprogL,dynprogM,dynprogR,

			    user_maxlevel_5,user_maxlevel_3,subopt_levels,
			    indel_penalty_middle,indel_penalty_end,
			    localsplicing_penalty,distantsplicing_penalty,min_shortend,
			    allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			    allvalidp5,allvalidp3,pairmax,maxpairedpaths,keep_floors_p,
			    queryseq5,queryseq3,/*genestrand*/0);
      debug(printf("Done with align_pair\n"));

      if (abort_pairing_p == true) {
	debug(printf("abort_pairing_p is true, so realigning separately\n"));
	debug13(printf("abort_pairing_p is true\n"));
	paired_results_free(this5,this3,hitpairs,samechr,conc_transloc,with_terminal,
			    hits5,hits3,querylength5,querylength3);

	this5 = Stage1_new(querylength5);
	this3 = Stage1_new(querylength3);
	realign_separately(stage3array5,&(*nhits5),&(*first_absmq5),&(*second_absmq5),
			   stage3array3,&(*nhits3),&(*first_absmq3),&(*second_absmq3),
			   gmap_history_5,gmap_history_3,this5,this3,
			   query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
			   queryseq5,queryuc_ptr_5,queryrc5,quality_string_5,querylength5,query5_lastpos,
			   queryseq3,queryuc_ptr_3,queryrc3,quality_string_3,querylength3,query3_lastpos,
			   indexdb_fwd,indexdb_rev,indexdb_size_threshold,genome,floors_array,
			   user_maxlevel_5,user_maxlevel_3,subopt_levels,
			   indel_penalty_middle,indel_penalty_end,
			   allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			   localsplicing_penalty,distantsplicing_penalty,min_shortend,
			   oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			   pairpool,diagpool,dynprogL,dynprogM,dynprogR,
			   keep_floors_p,/*genestrand*/0);

	*npaths = 0;
	*final_pairtype = UNPAIRED;
	History_free(&gmap_history_3);
	History_free(&gmap_history_5);
	Compress_free(&query5_compress_fwd);
	Compress_free(&query5_compress_rev);
	Compress_free(&query3_compress_fwd);
	Compress_free(&query3_compress_rev);
	Stage1_free(&this5,querylength5);
	Stage1_free(&this3,querylength3);

	debug(printf("Returning NULL\n"));
	return (Stage3pair_T *) NULL;

      } else {
	debug(printf("abort_pairing_p is false, so consolidating paired results\n"));
	stage3pairarray =
	  consolidate_paired_results(&(*npaths),&(*first_absmq),&(*second_absmq),&(*final_pairtype),
				     &(*stage3array5),&(*nhits5),&(*first_absmq5),&(*second_absmq5),
				     &(*stage3array3),&(*nhits3),&(*first_absmq3),&(*second_absmq3),
				     hitpairs,samechr,conc_transloc,with_terminal,hits5,hits3,gmap_history_5,gmap_history_3,
				     query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
				     &this5->plus_segments,&this5->plus_nsegments,&this5->minus_segments,&this5->minus_nsegments,
				     &this3->plus_segments,&this3->plus_nsegments,&this3->minus_segments,&this3->minus_nsegments,
				     queryseq5,queryuc_ptr_5,quality_string_5,querylength5,query5_lastpos,
				     queryseq3,queryuc_ptr_3,quality_string_3,querylength3,query3_lastpos,
				     genome,subopt_levels,cutoff_level_5,cutoff_level_3,
				     localsplicing_penalty,
				     oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
				     pairpool,diagpool,dynprogL,dynprogM,dynprogR,pairmax,user_maxlevel_5,user_maxlevel_3);

	History_free(&gmap_history_3);
	History_free(&gmap_history_5);
	Compress_free(&query5_compress_fwd);
	Compress_free(&query5_compress_rev);
	Compress_free(&query3_compress_fwd);
	Compress_free(&query3_compress_rev);
	Stage1_free(&this5,querylength5);
	Stage1_free(&this3,querylength3);

	debug(printf("Returning stage3pairarray\n"));
	return stage3pairarray;
      }
    }
  }
}


static Stage3pair_T *
paired_read_tolerant_nonstranded (int *npaths, int *first_absmq, int *second_absmq, Pairtype_T *final_pairtype,
				  Stage3end_T **stage3array5, int *nhits5, int *first_absmq5, int *second_absmq5,
				  Stage3end_T **stage3array3, int *nhits3, int *first_absmq3, int *second_absmq3,
				  Shortread_T queryseq5, Shortread_T queryseq3,
				  Indexdb_T indexdb_geneplus, Indexdb_T indexdb_geneminus, int indexdb_size_threshold,
				  Genome_T genome, Floors_T *floors_array,
				  double user_maxlevel_float, int subopt_levels,
				  int indel_penalty_middle, int indel_penalty_end,
				  bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
				  int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
				  Oligoindex_T *oligoindices_major, int noligoindices_major,
				  Oligoindex_T *oligoindices_minor, int noligoindices_minor,
				  Pairpool_T pairpool, Diagpool_T diagpool,
				  Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
				  Chrpos_T pairmax, bool keep_floors_p) {
  Stage3pair_T *stage3pairarray;
  List_T hitpairs, hitpairs_geneplus = NULL, hitpairs_geneminus = NULL;
  List_T samechr, samechr_geneplus = NULL, samechr_geneminus = NULL;
  List_T conc_transloc, conc_transloc_geneplus = NULL, conc_transloc_geneminus = NULL;
  List_T with_terminal, with_terminal_geneplus = NULL, with_terminal_geneminus = NULL;
  List_T hits5, hits3, hits_geneplus_5 = NULL, hits_geneplus_3 = NULL, hits_geneminus_5 = NULL, hits_geneminus_3 = NULL;
  T this_geneplus_5, this_geneplus_3, this_geneminus_5, this_geneminus_3;
  char *queryuc_ptr_5, *queryuc_ptr_3, *quality_string_5, *quality_string_3;
  char queryrc5[MAX_READLENGTH+1], queryrc3[MAX_READLENGTH+1];
  Compress_T query5_compress_fwd = NULL, query5_compress_rev = NULL, query3_compress_fwd = NULL, query3_compress_rev = NULL;
  History_T gmap_history_5, gmap_history_3;
  int user_maxlevel_5, user_maxlevel_3;
  int found_score_geneplus, found_score_geneminus;
  int cutoff_level_5, cutoff_level_3;
  int querylength5, querylength3, query5_lastpos, query3_lastpos;
  int noligos5, noligos3;
  bool allvalidp5, allvalidp3;
#if 0
  int maxpairedpaths = 10*maxpaths; /* For computation, not for printing. */
#else
  int maxpairedpaths = maxpaths_search;  /* 100000 */
#endif
  bool abort_pairing_p_geneplus, abort_pairing_p_geneminus;
  struct Segment_T *plus_segments_genestrand_5[3], *minus_segments_genestrand_5[3],
    *plus_segments_genestrand_3[3], *minus_segments_genestrand_3[3];
  int plus_nsegments_genestrand_5[3], minus_nsegments_genestrand_5[3],
    plus_nsegments_genestrand_3[3], minus_nsegments_genestrand_3[3];


  querylength5 = Shortread_fulllength(queryseq5);
  querylength3 = Shortread_fulllength(queryseq3);

  if (querylength5 < min_readlength || querylength3 < min_readlength) {
    fprintf(stderr,"GSNAP cannot handle reads shorter than %d bp with this k-mer = %d genomic index.\n",
	    min_readlength,index1part);
    fprintf(stderr,"You may want to build a genomic index with a smaller k-mer value using the -k flag to gmap_build\n");
    *npaths = *nhits5 = *nhits3 = 0;
    *stage3array5 = *stage3array3 = (Stage3end_T *) NULL;
    return (Stage3pair_T *) NULL;

  } else if (querylength5 > MAX_READLENGTH || querylength3 > MAX_READLENGTH) {
    fprintf(stderr,"GSNAP cannot handle reads longer than %d bp.  Either run configure and make again with a higher value of MAX_READLENGTH, or consider using GMAP instead.\n",
	    MAX_READLENGTH);
    *npaths = *nhits5 = *nhits3 = 0;
    *stage3array5 = *stage3array3 = (Stage3end_T *) NULL;
    return (Stage3pair_T *) NULL;

  } else {
    if (user_maxlevel_float < 0.0) {
      user_maxlevel_5 = user_maxlevel_3 = -1;
    } else if (user_maxlevel_float > 0.0 && user_maxlevel_float < 1.0) {
      user_maxlevel_5 = (int) rint(user_maxlevel_float * (double) querylength5);
      user_maxlevel_3 = (int) rint(user_maxlevel_float * (double) querylength3);
    } else {
      user_maxlevel_5 = user_maxlevel_3 = (int) user_maxlevel_float;
    }

    this_geneplus_5 = Stage1_new(querylength5);
    this_geneplus_3 = Stage1_new(querylength3);
    this_geneminus_5 = Stage1_new(querylength5);
    this_geneminus_3 = Stage1_new(querylength3);

    queryuc_ptr_5 = Shortread_fullpointer_uc(queryseq5);
    queryuc_ptr_3 = Shortread_fullpointer_uc(queryseq3);
    quality_string_5 = Shortread_quality_string(queryseq5);
    quality_string_3 = Shortread_quality_string(queryseq3);
    query5_lastpos = querylength5 - index1part;
    query3_lastpos = querylength3 - index1part;

    /* Limit search on repetitive sequences */
    if (check_dinucleotides(queryuc_ptr_5,querylength5) == false) {
      user_maxlevel_5 = 0;
    }
    if (check_dinucleotides(queryuc_ptr_3,querylength3) == false) {
      user_maxlevel_3 = 0;
    }

    query5_compress_fwd = Compress_new_fwd(queryuc_ptr_5,querylength5);
    query5_compress_rev = Compress_new_rev(queryuc_ptr_5,querylength5);
    query3_compress_fwd = Compress_new_fwd(queryuc_ptr_3,querylength3);
    query3_compress_rev = Compress_new_rev(queryuc_ptr_3,querylength3);
    gmap_history_5 = History_new();
    gmap_history_3 = History_new();
    make_complement_buffered(queryrc5,queryuc_ptr_5,querylength5);
    make_complement_buffered(queryrc3,queryuc_ptr_3,querylength3);

    abort_pairing_p_geneplus = false;
    noligos5 = read_oligos(&allvalidp5,this_geneplus_5,queryuc_ptr_5,querylength5,query5_lastpos,/*genestrand*/+1);
    noligos3 = read_oligos(&allvalidp3,this_geneplus_3,queryuc_ptr_3,querylength3,query3_lastpos,/*genestrand*/+1);

    if (noligos5 == 0 && noligos3 == 0) {
      debug(printf("Aborting because no hits found anywhere\n"));
      hitpairs_geneplus = (List_T) NULL;

    } else {
      hitpairs_geneplus = align_pair(&abort_pairing_p_geneplus,&found_score_geneplus,
				     &cutoff_level_5,&cutoff_level_3,
				     &samechr_geneplus,&conc_transloc_geneplus,&with_terminal_geneplus,
				     gmap_history_5,gmap_history_3,
				     &hits_geneplus_5,&hits_geneplus_3,this_geneplus_5,this_geneplus_3,
				     query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
				     queryuc_ptr_5,queryuc_ptr_3,queryrc5,queryrc3,
				     querylength5,querylength3,query5_lastpos,query3_lastpos,
				     indexdb_geneplus,indexdb_geneplus,indexdb_size_threshold,floors_array,

				     oligoindices_major,noligoindices_major,
				     oligoindices_minor,noligoindices_minor,
				     pairpool,diagpool,dynprogL,dynprogM,dynprogR,

				     user_maxlevel_5,user_maxlevel_3,subopt_levels,
				     indel_penalty_middle,indel_penalty_end,
				     localsplicing_penalty,distantsplicing_penalty,min_shortend,
				     allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
				     allvalidp5,allvalidp3,pairmax,maxpairedpaths,keep_floors_p,
				     queryseq5,queryseq3,/*genestrand*/+1);
    }

    abort_pairing_p_geneminus = false;
    noligos5 = read_oligos(&allvalidp5,this_geneminus_5,queryuc_ptr_5,querylength5,query5_lastpos,/*genestrand*/+2);
    noligos3 = read_oligos(&allvalidp3,this_geneminus_3,queryuc_ptr_3,querylength3,query3_lastpos,/*genestrand*/+2);

    if (noligos5 == 0 && noligos3 == 0) {
      debug(printf("Aborting because no hits found anywhere\n"));
      hitpairs_geneplus = (List_T) NULL;

    } else {
      hitpairs_geneminus = align_pair(&abort_pairing_p_geneminus,&found_score_geneminus,
				      &cutoff_level_5,&cutoff_level_3,
				      &samechr_geneminus,&conc_transloc_geneminus,&with_terminal_geneminus,
				      gmap_history_5,gmap_history_3,
				      &hits_geneminus_5,&hits_geneminus_3,this_geneminus_5,this_geneminus_3,
				      query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
				      queryuc_ptr_5,queryuc_ptr_3,queryrc5,queryrc3,
				      querylength5,querylength3,query5_lastpos,query3_lastpos,
				      indexdb_geneminus,indexdb_geneminus,indexdb_size_threshold,floors_array,

				      oligoindices_major,noligoindices_major,
				      oligoindices_minor,noligoindices_minor,
				      pairpool,diagpool,dynprogL,dynprogM,dynprogR,

				      user_maxlevel_5,user_maxlevel_3,subopt_levels,
				      indel_penalty_middle,indel_penalty_end,
				      localsplicing_penalty,distantsplicing_penalty,min_shortend,
				      allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
				      allvalidp5,allvalidp3,pairmax,maxpairedpaths,keep_floors_p,
				      queryseq5,queryseq3,/*genestrand*/+2);
    }

    if (found_score_geneplus < found_score_geneminus) {
      paired_results_free(this_geneminus_5,this_geneminus_3,hitpairs_geneminus,samechr_geneminus,conc_transloc_geneminus,
			  with_terminal_geneminus,hits_geneminus_5,hits_geneminus_3,querylength5,querylength3);

      if (abort_pairing_p_geneplus == true) {
	debug13(printf("abort_pairing_p_geneplus is true\n"));
	paired_results_free(this_geneplus_5,this_geneplus_3,hitpairs_geneplus,samechr_geneplus,conc_transloc_geneplus,
			    with_terminal_geneplus,hits_geneplus_5,hits_geneplus_3,querylength5,querylength3);

	this_geneplus_5 = Stage1_new(querylength5);
	this_geneplus_3 = Stage1_new(querylength3);
	realign_separately(stage3array5,&(*nhits5),&(*first_absmq5),&(*second_absmq5),
			   stage3array3,&(*nhits3),&(*first_absmq3),&(*second_absmq3),
			   gmap_history_5,gmap_history_3,this_geneplus_5,this_geneplus_3,
			   query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
			   queryseq5,queryuc_ptr_5,queryrc5,quality_string_5,querylength5,query5_lastpos,
			   queryseq3,queryuc_ptr_3,queryrc3,quality_string_3,querylength3,query3_lastpos,
			   indexdb_geneplus,indexdb_geneplus,indexdb_size_threshold,genome,floors_array,
			   user_maxlevel_5,user_maxlevel_3,subopt_levels,
			   indel_penalty_middle,indel_penalty_end,
			   allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			   localsplicing_penalty,distantsplicing_penalty,min_shortend,
			   oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			   pairpool,diagpool,dynprogL,dynprogM,dynprogR,
			   keep_floors_p,/*genestrand*/+1);

	*npaths = 0;
	*final_pairtype = UNPAIRED;
	History_free(&gmap_history_3);
	History_free(&gmap_history_5);
	Compress_free(&query5_compress_fwd);
	Compress_free(&query5_compress_rev);
	Compress_free(&query3_compress_fwd);
	Compress_free(&query3_compress_rev);
	Stage1_free(&this_geneplus_5,querylength5);
	Stage1_free(&this_geneplus_3,querylength3);
	return (Stage3pair_T *) NULL;

      } else {
	plus_segments_genestrand_5[+1] = this_geneplus_5->plus_segments;
	plus_nsegments_genestrand_5[+1] = this_geneplus_5->plus_nsegments;
	minus_segments_genestrand_5[+1] = this_geneplus_5->minus_segments;
	minus_nsegments_genestrand_5[+1] = this_geneplus_5->minus_nsegments;

	plus_segments_genestrand_3[+1] = this_geneplus_3->plus_segments;
	plus_nsegments_genestrand_3[+1] = this_geneplus_3->plus_nsegments;
	minus_segments_genestrand_3[+1] = this_geneplus_3->minus_segments;
	minus_nsegments_genestrand_3[+1] = this_geneplus_3->minus_nsegments;

	stage3pairarray =
	  consolidate_paired_results(&(*npaths),&(*first_absmq),&(*second_absmq),&(*final_pairtype),
				     &(*stage3array5),&(*nhits5),&(*first_absmq5),&(*second_absmq5),
				     &(*stage3array3),&(*nhits3),&(*first_absmq3),&(*second_absmq3),
				     hitpairs_geneplus,samechr_geneplus,conc_transloc_geneplus,with_terminal_geneplus,
				     hits_geneplus_5,hits_geneplus_3,gmap_history_5,gmap_history_3,
				     query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
				     plus_segments_genestrand_5,plus_nsegments_genestrand_5,minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				     plus_segments_genestrand_3,plus_nsegments_genestrand_3,minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				     queryseq5,queryuc_ptr_5,quality_string_5,querylength5,query5_lastpos,
				     queryseq3,queryuc_ptr_3,quality_string_3,querylength3,query3_lastpos,
				     genome,subopt_levels,cutoff_level_5,cutoff_level_3,
				     localsplicing_penalty,
				     oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
				     pairpool,diagpool,dynprogL,dynprogM,dynprogR,pairmax,user_maxlevel_5,user_maxlevel_3);
	History_free(&gmap_history_3);
	History_free(&gmap_history_5);
	Compress_free(&query5_compress_fwd);
	Compress_free(&query5_compress_rev);
	Compress_free(&query3_compress_fwd);
	Compress_free(&query3_compress_rev);
	Stage1_free(&this_geneplus_5,querylength5);
	Stage1_free(&this_geneplus_3,querylength3);
	return stage3pairarray;
      }

    } else if (found_score_geneminus < found_score_geneplus) {
      paired_results_free(this_geneplus_5,this_geneplus_3,hitpairs_geneplus,samechr_geneplus,conc_transloc_geneplus,
			  with_terminal_geneplus,hits_geneplus_5,hits_geneplus_3,querylength5,querylength3);

      if (abort_pairing_p_geneminus == true) {
	debug13(printf("abort_pairing_p_geneminus is true\n"));
	paired_results_free(this_geneminus_5,this_geneminus_3,hitpairs_geneminus,samechr_geneminus,conc_transloc_geneminus,
			    with_terminal_geneminus,hits_geneminus_5,hits_geneminus_3,querylength5,querylength3);

	this_geneminus_5 = Stage1_new(querylength5);
	this_geneminus_3 = Stage1_new(querylength3);
	realign_separately(stage3array5,&(*nhits5),&(*first_absmq5),&(*second_absmq5),
			   stage3array3,&(*nhits3),&(*first_absmq3),&(*second_absmq3),
			   gmap_history_5,gmap_history_3,this_geneminus_5,this_geneminus_3,
			   query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
			   queryseq5,queryuc_ptr_5,queryrc5,quality_string_5,querylength5,query5_lastpos,
			   queryseq3,queryuc_ptr_3,queryrc3,quality_string_3,querylength3,query3_lastpos,
			   indexdb_geneminus,indexdb_geneminus,indexdb_size_threshold,genome,floors_array,
			   user_maxlevel_5,user_maxlevel_3,subopt_levels,
			   indel_penalty_middle,indel_penalty_end,
			   allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
			   localsplicing_penalty,distantsplicing_penalty,min_shortend,
			   oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
			   pairpool,diagpool,dynprogL,dynprogM,dynprogR,
			   keep_floors_p,/*genestrand*/+2);

	*npaths = 0;
	*final_pairtype = UNPAIRED;
	History_free(&gmap_history_3);
	History_free(&gmap_history_5);
	Compress_free(&query5_compress_fwd);
	Compress_free(&query5_compress_rev);
	Compress_free(&query3_compress_fwd);
	Compress_free(&query3_compress_rev);
	Stage1_free(&this_geneminus_5,querylength5);
	Stage1_free(&this_geneminus_3,querylength3);
	return (Stage3pair_T *) NULL;

      } else {
	plus_segments_genestrand_5[+2] = this_geneminus_5->plus_segments;
	plus_nsegments_genestrand_5[+2] = this_geneminus_5->plus_nsegments;
	minus_segments_genestrand_5[+2] = this_geneminus_5->minus_segments;
	minus_nsegments_genestrand_5[+2] = this_geneminus_5->minus_nsegments;

	plus_segments_genestrand_3[+2] = this_geneminus_3->plus_segments;
	plus_nsegments_genestrand_3[+2] = this_geneminus_3->plus_nsegments;
	minus_segments_genestrand_3[+2] = this_geneminus_3->minus_segments;
	minus_nsegments_genestrand_3[+2] = this_geneminus_3->minus_nsegments;

	stage3pairarray =
	  consolidate_paired_results(&(*npaths),&(*first_absmq),&(*second_absmq),&(*final_pairtype),
				     &(*stage3array5),&(*nhits5),&(*first_absmq5),&(*second_absmq5),
				     &(*stage3array3),&(*nhits3),&(*first_absmq3),&(*second_absmq3),
				     hitpairs_geneminus,samechr_geneminus,conc_transloc_geneminus,with_terminal_geneminus,
				     hits_geneminus_5,hits_geneminus_3,gmap_history_5,gmap_history_3,
				     query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
				     plus_segments_genestrand_5,plus_nsegments_genestrand_5,minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				     plus_segments_genestrand_3,plus_nsegments_genestrand_3,minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				     queryseq5,queryuc_ptr_5,quality_string_5,querylength5,query5_lastpos,
				     queryseq3,queryuc_ptr_3,quality_string_3,querylength3,query3_lastpos,
				     genome,subopt_levels,cutoff_level_5,cutoff_level_3,
				     localsplicing_penalty,
				     oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
				     pairpool,diagpool,dynprogL,dynprogM,dynprogR,pairmax,user_maxlevel_5,user_maxlevel_3);
	History_free(&gmap_history_3);
	History_free(&gmap_history_5);
	Compress_free(&query5_compress_fwd);
	Compress_free(&query5_compress_rev);
	Compress_free(&query3_compress_fwd);
	Compress_free(&query3_compress_rev);
	Stage1_free(&this_geneminus_5,querylength5);
	Stage1_free(&this_geneminus_3,querylength3);
	return stage3pairarray;
      }

    } else {
      hitpairs = List_append(hitpairs_geneplus,hitpairs_geneminus);
      samechr = List_append(samechr_geneplus,samechr_geneminus);
      conc_transloc = List_append(conc_transloc_geneplus,conc_transloc_geneminus);
      with_terminal = List_append(with_terminal_geneplus,with_terminal_geneminus);
      hits5 = List_append(hits_geneplus_5,hits_geneminus_5);
      hits3 = List_append(hits_geneplus_3,hits_geneminus_3);

      plus_segments_genestrand_5[+1] = this_geneplus_5->plus_segments;
      plus_nsegments_genestrand_5[+1] = this_geneplus_5->plus_nsegments;
      minus_segments_genestrand_5[+1] = this_geneplus_5->minus_segments;
      minus_nsegments_genestrand_5[+1] = this_geneplus_5->minus_nsegments;

      plus_segments_genestrand_3[+1] = this_geneplus_3->plus_segments;
      plus_nsegments_genestrand_3[+1] = this_geneplus_3->plus_nsegments;
      minus_segments_genestrand_3[+1] = this_geneplus_3->minus_segments;
      minus_nsegments_genestrand_3[+1] = this_geneplus_3->minus_nsegments;

      plus_segments_genestrand_5[+2] = this_geneminus_5->plus_segments;
      plus_nsegments_genestrand_5[+2] = this_geneminus_5->plus_nsegments;
      minus_segments_genestrand_5[+2] = this_geneminus_5->minus_segments;
      minus_nsegments_genestrand_5[+2] = this_geneminus_5->minus_nsegments;

      plus_segments_genestrand_3[+2] = this_geneminus_3->plus_segments;
      plus_nsegments_genestrand_3[+2] = this_geneminus_3->plus_nsegments;
      minus_segments_genestrand_3[+2] = this_geneminus_3->minus_segments;
      minus_nsegments_genestrand_3[+2] = this_geneminus_3->minus_nsegments;

      stage3pairarray =
	consolidate_paired_results(&(*npaths),&(*first_absmq),&(*second_absmq),&(*final_pairtype),
				   &(*stage3array5),&(*nhits5),&(*first_absmq5),&(*second_absmq5),
				   &(*stage3array3),&(*nhits3),&(*first_absmq3),&(*second_absmq3),
				   hitpairs,samechr,conc_transloc,with_terminal,hits5,hits3,gmap_history_5,gmap_history_3,
				   query5_compress_fwd,query5_compress_rev,query3_compress_fwd,query3_compress_rev,
				   plus_segments_genestrand_5,plus_nsegments_genestrand_5,minus_segments_genestrand_5,minus_nsegments_genestrand_5,
				   plus_segments_genestrand_3,plus_nsegments_genestrand_3,minus_segments_genestrand_3,minus_nsegments_genestrand_3,
				   queryseq5,queryuc_ptr_5,quality_string_5,querylength5,query5_lastpos,
				   queryseq3,queryuc_ptr_3,quality_string_3,querylength3,query3_lastpos,
				   genome,subopt_levels,cutoff_level_5,cutoff_level_3,
				   localsplicing_penalty,
				   oligoindices_major,noligoindices_major,oligoindices_minor,noligoindices_minor,
				   pairpool,diagpool,dynprogL,dynprogM,dynprogR,pairmax,user_maxlevel_5,user_maxlevel_3);
      History_free(&gmap_history_3);
      History_free(&gmap_history_5);
      Compress_free(&query5_compress_fwd);
      Compress_free(&query5_compress_rev);
      Compress_free(&query3_compress_fwd);
      Compress_free(&query3_compress_rev);
      Stage1_free(&this_geneminus_5,querylength5);
      Stage1_free(&this_geneminus_3,querylength3);
      Stage1_free(&this_geneplus_5,querylength5);
      Stage1_free(&this_geneplus_3,querylength3);
      return stage3pairarray;
    }
  }
}


Stage3pair_T *
Stage1_paired_read (int *npaths, int *first_absmq, int *second_absmq, Pairtype_T *final_pairtype,
		    Stage3end_T **stage3array5, int *nhits5, int *first_absmq5, int *second_absmq5,
		    Stage3end_T **stage3array3, int *nhits3, int *first_absmq3, int *second_absmq3,
		    Shortread_T queryseq5, Shortread_T queryseq3,
		    Indexdb_T indexdb, Indexdb_T indexdb2, int indexdb_size_threshold,
		    Genome_T genome, Floors_T *floors_array,
		    double user_maxlevel_float, int subopt_levels,
		    int indel_penalty_middle, int indel_penalty_end,
		    bool allow_end_indels_p, int max_end_insertions, int max_end_deletions, int min_indel_end_matches,
		    int localsplicing_penalty, int distantsplicing_penalty, int min_shortend,
		    Oligoindex_T *oligoindices_major, int noligoindices_major,
		    Oligoindex_T *oligoindices_minor, int noligoindices_minor,
		    Pairpool_T pairpool, Diagpool_T diagpool,
		    Dynprog_T dynprogL, Dynprog_T dynprogM, Dynprog_T dynprogR,
		    Chrpos_T pairmax, bool keep_floors_p) {

  if (mode == STANDARD || mode == CMET_STRANDED || mode == ATOI_STRANDED) {
    return paired_read(&(*npaths),&(*first_absmq),&(*second_absmq),&(*final_pairtype),
		       &(*stage3array5),&(*nhits5),&(*first_absmq5),&(*second_absmq5),
		       &(*stage3array3),&(*nhits3),&(*first_absmq3),&(*second_absmq3),
		       queryseq5,queryseq3,/*indexdb_fwd*/indexdb,/*indexdb_rev*/indexdb2,indexdb_size_threshold,
		       genome,floors_array,user_maxlevel_float,subopt_levels,
		       indel_penalty_middle,indel_penalty_end,
		       allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
		       localsplicing_penalty,distantsplicing_penalty,min_shortend,
		       oligoindices_major,noligoindices_major,
		       oligoindices_minor,noligoindices_minor,pairpool,diagpool,
		       dynprogL,dynprogM,dynprogR,pairmax,keep_floors_p);

  } else if (mode == CMET_NONSTRANDED || mode == ATOI_NONSTRANDED) {
    return paired_read_tolerant_nonstranded(&(*npaths),&(*first_absmq),&(*second_absmq),&(*final_pairtype),
					    &(*stage3array5),&(*nhits5),&(*first_absmq5),&(*second_absmq5),
					    &(*stage3array3),&(*nhits3),&(*first_absmq3),&(*second_absmq3),
					    queryseq5,queryseq3,/*indexdb_geneplus*/indexdb,/*indexdb_geneminus*/indexdb2,indexdb_size_threshold,
					    genome,floors_array,user_maxlevel_float,subopt_levels,
					    indel_penalty_middle,indel_penalty_end,
					    allow_end_indels_p,max_end_insertions,max_end_deletions,min_indel_end_matches,
					    localsplicing_penalty,distantsplicing_penalty,min_shortend,
					    oligoindices_major,noligoindices_major,
					    oligoindices_minor,noligoindices_minor,pairpool,diagpool,
					    dynprogL,dynprogM,dynprogR,pairmax,keep_floors_p);
  } else {
    fprintf(stderr,"Do not recognize mode %d\n",mode);
    abort();
  }
}


void
Stage1hr_cleanup () {
  FREE(chroffsets);
  FREE(chrhighs);
  FREE(chrlengths);
  return;
}


void
Stage1hr_setup (bool use_sarray_p_in, int index1part_in, int index1interval_in,
		int spansize_in, Univ_IIT_T chromosome_iit_in, int nchromosomes_in,
		Genome_T genomealt, Mode_T mode_in, int maxpaths_search_in,
		int terminal_threshold_in, int terminal_output_minlength_in,

		Univcoord_T *splicesites_in, Splicetype_T *splicetypes_in,
		Chrpos_T *splicedists_in, int nsplicesites_in,
		
		bool novelsplicingp_in, bool knownsplicingp_in,
		bool distances_observed_p_in,
		Chrpos_T max_middle_insertions_in, Chrpos_T max_middle_deletions_in,
		Chrpos_T shortsplicedist_in, Chrpos_T shortsplicedist_known_in, Chrpos_T shortsplicedist_novelend_in,
		Chrpos_T min_intronlength_in,

		int min_distantsplicing_end_matches_in, int min_distantsplicing_identity_in,

		int nullgap_in, int maxpeelback_in, int maxpeelback_distalmedial_in,
		int extramaterial_end_in, int extramaterial_paired_in,
		int gmap_mode, int trigger_score_for_gmap_in, int gmap_allowance_in,
		int max_gmap_pairsearch_in, int max_gmap_terminal_in,
		int max_gmap_improvement_in, int antistranded_penalty_in) {
  bool gmapp = false;

  use_sarray_p = use_sarray_p_in;
  index1part = index1part_in;
  index1interval = index1interval_in;
  two_index1intervals = index1interval_in + index1interval_in;
  spansize = spansize_in;

  min_readlength = index1part_in + index1interval_in - 1;
  chromosome_iit = chromosome_iit_in;
  circular_typeint = Univ_IIT_typeint(chromosome_iit,"circular");
  nchromosomes = nchromosomes_in;
  Univ_IIT_intervals_setup(&chroffsets,&chrhighs,&chrlengths,chromosome_iit,nchromosomes,circular_typeint);

  leftreadshift = 32 - index1part - index1part; /* For 12-mers, 8 */
  oligobase_mask = ~(~0UL << 2*index1part);  /* For 12-mers, was 0x00FFFFFF */
  one_miss_querylength = spansize + spansize - (index1interval - 1); /* For 12-mers, 22 */

#if 0
  /* Should be 2 index1parts to handle a mismatch, plus a shift of 2, minus 3 if second one aligns */
  /* But this leads to many false positives, and GMAP can handle these other cases. */
  end_miss_two = spansize + (index1interval - 1) + spansize - index1interval;
#else
  end_miss_one = spansize + (index1interval - 1);
  end_miss_two = spansize + (index1interval - 1) + spansize - index1interval;
#endif
  
  mode = mode_in;
  maxpaths_search = maxpaths_search_in;

  terminal_threshold = terminal_threshold_in;
  terminal_output_minlength = terminal_output_minlength_in;

  splicesites = splicesites_in;
  splicetypes = splicetypes_in;
  splicedists = splicedists_in;
  nsplicesites = nsplicesites_in;

  novelsplicingp = novelsplicingp_in;
  knownsplicingp = knownsplicingp_in;
  distances_observed_p = distances_observed_p_in;

  max_middle_insertions = max_middle_insertions_in;
  max_middle_deletions = max_middle_deletions_in;

  shortsplicedist = shortsplicedist_in;
  shortsplicedist_known = shortsplicedist_known_in;
  shortsplicedist_novelend = shortsplicedist_novelend_in;

  overall_max_distance = shortsplicedist;
  if (max_middle_deletions > overall_max_distance) {
    overall_max_distance = max_middle_deletions;
  }
  if (max_middle_insertions > overall_max_distance) {
    overall_max_distance = max_middle_insertions;
  }

  min_intronlength = min_intronlength_in;
  min_distantsplicing_end_matches = min_distantsplicing_end_matches_in;
  min_distantsplicing_identity = min_distantsplicing_identity_in;

  nullgap = nullgap_in;
  maxpeelback = maxpeelback_in;
  maxpeelback_distalmedial = maxpeelback_distalmedial_in;
  extramaterial_end = extramaterial_end_in;
  extramaterial_paired = extramaterial_paired_in;

  gmap_pairsearch_p = false;
  gmap_indel_knownsplice_p = false;
  gmap_terminal_p = false;
  gmap_improvement_p = false;

  fprintf(stderr,"GMAP modes:");
  if ((gmap_mode & GMAP_PAIRSEARCH) != 0) {
    if (gmapp == true) {
      fprintf(stderr,",");
    } else {
      gmapp = true;
    }
    fprintf(stderr," pairsearch");
    gmap_pairsearch_p = true;
  }
  if ((gmap_mode & GMAP_INDEL_KNOWNSPLICE) != 0) {
    if (gmapp == true) {
      fprintf(stderr,",");
    } else {
      gmapp = true;
    }
    fprintf(stderr," indel_knownsplice");
    gmap_indel_knownsplice_p = true;
  }
  if ((gmap_mode & GMAP_TERMINAL) != 0) {
    if (gmapp == true) {
      fprintf(stderr,",");
    } else {
      gmapp = true;
    }
    fprintf(stderr," terminal");
    gmap_terminal_p = true;
  }
  if ((gmap_mode & GMAP_IMPROVEMENT) != 0) {
    if (gmapp == true) {
      fprintf(stderr,",");
    } else {
      gmapp = true;
    }
    fprintf(stderr," improvement");
    gmap_improvement_p = true;
  }
  if (gmapp == false) {
    fprintf(stderr," none");
  }
  fprintf(stderr,"\n");


  trigger_score_for_gmap = trigger_score_for_gmap_in;
  gmap_allowance = gmap_allowance_in;

  max_gmap_pairsearch = max_gmap_pairsearch_in;
  max_gmap_terminal = max_gmap_terminal_in;
  max_gmap_improvement = max_gmap_improvement_in;

  antistranded_penalty = antistranded_penalty_in;

  if (genomealt != NULL) {
    snpp = true;
  } else {
    snpp = false;
  }

  return;
}

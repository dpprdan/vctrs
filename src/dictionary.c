#include "vctrs.h"
#include "dictionary.h"
#include "utils.h"

// Initialised at load time
struct vctrs_arg args_needles;
struct vctrs_arg args_haystack;


// http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
int32_t ceil2(int32_t x) {
  x--;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  x++;
  return x;
}

// Dictonary object ------------------------------------------------------------

static void dict_init_impl(dictionary* d, SEXP x, bool partial);

// Dictionaries must be protected and unprotected in consistent stack
// order with `PROTECT_DICT()` and `UNPROTECT_DICT()`.
void dict_init(dictionary* d, SEXP x) {
  dict_init_impl(d, x, false);
}
void dict_init_partial(dictionary* d, SEXP x) {
  dict_init_impl(d, x, true);
}

static void dict_init_impl(dictionary* d, SEXP x, bool partial) {
  d->vec = PROTECT(vec_proxy_recursive(x, vctrs_proxy_equal));
  d->used = 0;

  if (partial) {
    d->key = NULL;
    d->size = 0;
  } else {
    // assume worst case, that every value is distinct, aiming for a load factor
    // of at most 77%. We round up to power of 2 to ensure quadratic probing
    // strategy works.
    // Rprintf("size: %i\n", size);
    R_len_t size = ceil2(vec_size(x) / 0.77);
    size = (size < 16) ? 16 : size;

    d->key = (R_len_t*) R_alloc(size, sizeof(R_len_t));
    memset(d->key, DICT_EMPTY, size * sizeof(R_len_t));

    d->size = size;
  }

  R_len_t n = vec_size(x);
  d->hash = (uint32_t*) R_alloc(n, sizeof(uint32_t));

  if (d->hash) {
    memset(d->hash, 0, n * sizeof(R_len_t));
    hash_fill(d->hash, n, x);
  }

  UNPROTECT(1);
}

uint32_t dict_hash_with(dictionary* d, dictionary* x, R_len_t i) {
  uint32_t hash = x->hash[i];

  // Quadratic probing: will try every slot if d->size is power of 2
  // http://research.cs.vt.edu/AVresearch/hashing/quadratic.php
  for (uint32_t k = 0; k < d->size; ++k) {
    uint32_t probe = (hash + k * (k + 1) / 2) & (d->size - 1);
    // Rprintf("Probe: %i\n", probe);

    // If we circled back to start, dictionary is full
    if (k > 1 && probe == hash) {
      break;
    }

    // Check for unused slot
    R_len_t idx = d->key[probe];
    if (idx == DICT_EMPTY) {
      return probe;
    }

    // Check for same value as there might be a collision. If there is
    // a collision, next iteration will find another spot using
    // quadratic probing.
    if (equal_scalar(d->vec, idx, x->vec, i, true)) {
      return probe;
    }
  }

  Rf_errorcall(R_NilValue, "Internal error: Dictionary is full!");
}

uint32_t dict_hash_scalar(dictionary* d, R_len_t i) {
  return dict_hash_with(d, d, i);
}


void dict_put(dictionary* d, uint32_t hash, R_len_t i) {
  d->key[hash] = i;
  d->used++;
}

// R interface -----------------------------------------------------------------
// TODO: rename to match R function names
// TODO: separate out into individual files

SEXP vctrs_unique_loc(SEXP x) {
  int nprot = 0;
  x = PROTECT_N(vec_proxy_equal(x), &nprot);

  dictionary d;
  dict_init(&d, x);
  PROTECT_DICT(&d, &nprot);

  growable g;
  growable_init(&g, INTSXP, 256);
  PROTECT_GROWABLE(&g, &nprot);

  R_len_t n = vec_size(x);
  for (int i = 0; i < n; ++i) {
    uint32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      dict_put(&d, hash, i);
      growable_push_int(&g, i + 1);
    }
  }

  SEXP out = growable_values(&g);

  UNPROTECT(nprot);
  return out;
}

SEXP vctrs_duplicated_any(SEXP x) {
  bool out = duplicated_any(x);
  return Rf_ScalarLogical(out);
}

// [[ include("vctrs.h") ]]
bool duplicated_any(SEXP x) {
  int nprot = 0;
  x = PROTECT_N(vec_proxy_equal(x), &nprot);

  dictionary d;
  dict_init(&d, x);
  PROTECT_DICT(&d, &nprot);

  bool out = false;
  R_len_t n = vec_size(x);

  for (int i = 0; i < n; ++i) {
    uint32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      dict_put(&d, hash, i);
    } else {
      out = true;
      break;
    }
  }

  UNPROTECT(nprot);
  return out;
}

SEXP vctrs_n_distinct(SEXP x) {
  int nprot = 0;
  x = PROTECT_N(vec_proxy_equal(x), &nprot);

  dictionary d;
  dict_init(&d, x);
  PROTECT_DICT(&d, &nprot);

  R_len_t n = vec_size(x);
  for (int i = 0; i < n; ++i) {
    uint32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY)
      dict_put(&d, hash, i);
  }

  UNPROTECT(nprot);
  return Rf_ScalarInteger(d.used);
}

SEXP vctrs_id(SEXP x) {
  int nprot = 0;
  x = PROTECT_N(vec_proxy_equal(x), &nprot);

  dictionary d;
  dict_init(&d, x);
  PROTECT_DICT(&d, &nprot);

  R_len_t n = vec_size(x);
  SEXP out = PROTECT_N(Rf_allocVector(INTSXP, n), &nprot);
  int* p_out = INTEGER(out);

  for (int i = 0; i < n; ++i) {
    uint32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      dict_put(&d, hash, i);
    }
    p_out[i] = d.key[hash] + 1;
  }

  UNPROTECT(nprot);
  return out;
}

// [[ register() ]]
SEXP vctrs_match(SEXP needles, SEXP haystack) {
  int nprot = 0;
  int _;
  SEXP type = PROTECT_N(vec_type2(needles, haystack, &args_needles, &args_haystack, &_), &nprot);

  needles = PROTECT_N(vec_cast(needles, type, args_empty, args_empty), &nprot);
  haystack = PROTECT_N(vec_cast(haystack, type, args_empty, args_empty), &nprot);

  needles = PROTECT_N(vec_proxy_equal(needles), &nprot);
  haystack = PROTECT_N(vec_proxy_equal(haystack), &nprot);

  R_len_t n_haystack = vec_size(haystack);
  R_len_t n_needle = vec_size(needles);

  SEXP translated = PROTECT_N(obj_translate_encoding2(needles, n_needle, haystack, n_haystack), &nprot);
  needles = VECTOR_ELT(translated, 0);
  haystack = VECTOR_ELT(translated, 1);

  dictionary d;
  dict_init(&d, haystack);
  PROTECT_DICT(&d, &nprot);

  // Load dictionary with haystack
  for (int i = 0; i < n_haystack; ++i) {
    uint32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      dict_put(&d, hash, i);
    }
  }

  dictionary d_needles;
  dict_init_partial(&d_needles, needles);

  // Locate needles
  SEXP out = PROTECT_N(Rf_allocVector(INTSXP, n_needle), &nprot);
  int* p_out = INTEGER(out);

  for (int i = 0; i < n_needle; ++i) {
    uint32_t hash = dict_hash_with(&d, &d_needles, i);
    if (d.key[hash] == DICT_EMPTY) {
      p_out[i] = NA_INTEGER;
    } else {
      p_out[i] = d.key[hash] + 1;
    }
  }

  UNPROTECT(nprot);
  return out;
}

// [[ register() ]]
SEXP vctrs_in(SEXP needles, SEXP haystack) {
  int nprot = 0;

  int _;
  SEXP type = PROTECT_N(vec_type2(needles, haystack, &args_needles, &args_haystack, &_), &nprot);

  needles = PROTECT_N(vec_cast(needles, type, args_empty, args_empty), &nprot);
  haystack = PROTECT_N(vec_cast(haystack, type, args_empty, args_empty), &nprot);

  needles = PROTECT_N(vec_proxy_equal(needles), &nprot);
  haystack = PROTECT_N(vec_proxy_equal(haystack), &nprot);

  R_len_t n_haystack = vec_size(haystack);
  R_len_t n_needle = vec_size(needles);

  SEXP translated = PROTECT_N(obj_translate_encoding2(needles, n_needle, haystack, n_haystack), &nprot);
  needles = VECTOR_ELT(translated, 0);
  haystack = VECTOR_ELT(translated, 1);

  dictionary d;
  dict_init(&d, haystack);
  PROTECT_DICT(&d, &nprot);

  // Load dictionary with haystack
  for (int i = 0; i < n_haystack; ++i) {
    uint32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      dict_put(&d, hash, i);
    }
  }

  dictionary d_needles;
  dict_init_partial(&d_needles, needles);
  PROTECT_DICT(&d_needles, &nprot);

  // Locate needles
  SEXP out = PROTECT_N(Rf_allocVector(LGLSXP, n_needle), &nprot);
  int* p_out = LOGICAL(out);

  for (int i = 0; i < n_needle; ++i) {
    uint32_t hash = dict_hash_with(&d, &d_needles, i);
    p_out[i] = (d.key[hash] != DICT_EMPTY);
  }

  UNPROTECT(nprot);
  return out;
}

SEXP vctrs_count(SEXP x) {
  int nprot = 0;

  dictionary d;
  dict_init(&d, x);
  PROTECT_DICT(&d, &nprot);

  SEXP val = PROTECT_N(Rf_allocVector(INTSXP, d.size), &nprot);
  int* p_val = INTEGER(val);

  R_len_t n = vec_size(x);
  for (int i = 0; i < n; ++i) {
    int32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      dict_put(&d, hash, i);
      p_val[hash] = 0;
    }
    p_val[hash]++;
  }

  // Create output
  SEXP out_key = PROTECT_N(Rf_allocVector(INTSXP, d.used), &nprot);
  SEXP out_val = PROTECT_N(Rf_allocVector(INTSXP, d.used), &nprot);
  int* p_out_key = INTEGER(out_key);
  int* p_out_val = INTEGER(out_val);

  int i = 0;
  for (int hash = 0; hash < d.size; ++hash) {
    if (d.key[hash] == DICT_EMPTY)
      continue;

    p_out_key[i] = d.key[hash] + 1;
    p_out_val[i] = p_val[hash];
    i++;
  }

  SEXP out = PROTECT_N(Rf_allocVector(VECSXP, 2), &nprot);
  SET_VECTOR_ELT(out, 0, out_key);
  SET_VECTOR_ELT(out, 1, out_val);
  SEXP names = PROTECT_N(Rf_allocVector(STRSXP, 2), &nprot);
  SET_STRING_ELT(names, 0, Rf_mkChar("key"));
  SET_STRING_ELT(names, 1, Rf_mkChar("val"));
  Rf_setAttrib(out, R_NamesSymbol, names);

  UNPROTECT(nprot);
  return out;
}

SEXP vctrs_duplicated(SEXP x) {
  int nprot = 0;

  x = PROTECT_N(vec_proxy_equal(x), &nprot);

  dictionary d;
  dict_init(&d, x);
  PROTECT_DICT(&d, &nprot);

  SEXP val = PROTECT_N(Rf_allocVector(INTSXP, d.size), &nprot);
  int* p_val = INTEGER(val);

  R_len_t n = vec_size(x);
  for (int i = 0; i < n; ++i) {
    int32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      dict_put(&d, hash, i);
      p_val[hash] = 0;
    }
    p_val[hash]++;
  }

  // Create output
  SEXP out = PROTECT_N(Rf_allocVector(LGLSXP, n), &nprot);
  int* p_out = LOGICAL(out);

  for (int i = 0; i < n; ++i) {
    int32_t hash = dict_hash_scalar(&d, i);
    p_out[i] = p_val[hash] != 1;
  }

  UNPROTECT(nprot);
  return out;
}

SEXP vctrs_split_id(SEXP x) {
  int nprot = 0;

  SEXP proxy = PROTECT_N(vec_proxy_equal(x), &nprot);

  dictionary d;
  dict_init(&d, proxy);
  PROTECT_DICT(&d, &nprot);

  // Tracks the order in which keys are seen
  SEXP tracker = PROTECT_N(Rf_allocVector(INTSXP, d.size), &nprot);
  int* p_tracker = INTEGER(tracker);

  // Collects the counts of each key
  SEXP count = PROTECT_N(Rf_allocVector(INTSXP, d.size), &nprot);
  int* p_count = INTEGER(count);

  R_len_t n = vec_size(proxy);

  // Tells us which element of the index list x[i] goes in
  SEXP out_pos = PROTECT_N(Rf_allocVector(INTSXP, n), &nprot);
  int* p_out_pos = INTEGER(out_pos);

  // Fill dictionary, out_pos, and count
  for (int i = 0; i < n; ++i) {
    uint32_t hash = dict_hash_scalar(&d, i);

    if (d.key[hash] == DICT_EMPTY) {
      p_tracker[hash] = d.used;
      dict_put(&d, hash, i);
      p_count[hash] = 0;
    }

    p_out_pos[i] = p_tracker[hash];
    p_count[hash]++;
  }

  // Track the first position of each key in `x`
  SEXP key_id = PROTECT_N(Rf_allocVector(INTSXP, d.used), &nprot);
  int* p_key_id = INTEGER(key_id);

  SEXP out_id = PROTECT_N(Rf_allocVector(VECSXP, d.used), &nprot);
  init_list_of(out_id, vctrs_shared_empty_int);

  SEXP counters = PROTECT_N(Rf_allocVector(INTSXP, d.used), &nprot);
  int* p_counters = INTEGER(counters);
  memset(p_counters, 0, d.used * sizeof(int));

  // Set up empty index container
  for (int hash = 0; hash < d.size; ++hash) {
    if (d.key[hash] == DICT_EMPTY) {
      continue;
    }

    SET_VECTOR_ELT(out_id, p_tracker[hash], Rf_allocVector(INTSXP, p_count[hash]));
  }

  // Fill index container and key locations
  for (int i = 0; i < n; ++i) {
    int j = p_out_pos[i];
    int hash = p_counters[j];

    if (hash == 0) {
      p_key_id[j] = i + 1;
    }

    INTEGER(VECTOR_ELT(out_id, j))[hash] = i + 1;
    p_counters[j] = hash + 1;
  }

  SEXP out_key = PROTECT_N(vec_slice(x, key_id), &nprot);

  // Construct output data frame
  SEXP out = PROTECT_N(Rf_allocVector(VECSXP, 2), &nprot);
  SET_VECTOR_ELT(out, 0, out_key);
  SET_VECTOR_ELT(out, 1, out_id);

  SEXP names = PROTECT_N(Rf_allocVector(STRSXP, 2), &nprot);
  SET_STRING_ELT(names, 0, strings_key);
  SET_STRING_ELT(names, 1, strings_id);

  Rf_setAttrib(out, R_NamesSymbol, names);

  out = new_data_frame(out, d.used);

  UNPROTECT(nprot);
  return out;
}


void vctrs_init_dictionary(SEXP ns) {
  args_needles = new_wrapper_arg(NULL, "needles");
  args_haystack = new_wrapper_arg(NULL, "haystack");
}

/*
 * DType related API shared by the (experimental) public API And internal API.
 */

#ifndef NUMPY_CORE_INCLUDE_NUMPY___DTYPE_API_H_
#define NUMPY_CORE_INCLUDE_NUMPY___DTYPE_API_H_

#define __EXPERIMENTAL_DTYPE_API_VERSION 8

struct PyArrayMethodObject_tag;

/*
 * Largely opaque struct for DType classes (i.e. metaclass instances).
 * The internal definition is currently in `ndarraytypes.h` (export is a bit
 * more complex because `PyArray_Descr` is a DTypeMeta internall but not
 * externally).
 */
#if !(defined(NPY_INTERNAL_BUILD) && NPY_INTERNAL_BUILD)

    typedef struct PyArray_DTypeMeta_tag {
        PyHeapTypeObject super;

        /*
        * Most DTypes will have a singleton default instance, for the
        * parametric legacy DTypes (bytes, string, void, datetime) this
        * may be a pointer to the *prototype* instance?
        */
        PyArray_Descr *singleton;
        /* Copy of the legacy DTypes type number, usually invalid. */
        int type_num;

        /* The type object of the scalar instances (may be NULL?) */
        PyTypeObject *scalar_type;
        /*
        * DType flags to signal legacy, parametric, or
        * abstract.  But plenty of space for additional information/flags.
        */
        npy_uint64 flags;

        /*
        * Use indirection in order to allow a fixed size for this struct.
        * A stable ABI size makes creating a static DType less painful
        * while also ensuring flexibility for all opaque API (with one
        * indirection due the pointer lookup).
        */
        void *dt_slots;
        /* Allow growing (at the moment also beyond this) */
        void *reserved[3];
    } PyArray_DTypeMeta;

#endif  /* not internal build */

/*
 * ******************************************************
 *         ArrayMethod API (Casting and UFuncs)
 * ******************************************************
 */
/*
 * NOTE: Expected changes:
 *       * probably split runtime and general flags into two
 *       * should possibly not use an enum for typedef for more stable ABI?
 */
typedef enum {
    /* Flag for whether the GIL is required */
    NPY_METH_REQUIRES_PYAPI = 1 << 0,
    /*
     * Some functions cannot set floating point error flags, this flag
     * gives us the option (not requirement) to skip floating point error
     * setup/check. No function should set error flags and ignore them
     * since it would interfere with chaining operations (e.g. casting).
     */
    NPY_METH_NO_FLOATINGPOINT_ERRORS = 1 << 1,
    /* Whether the method supports unaligned access (not runtime) */
    NPY_METH_SUPPORTS_UNALIGNED = 1 << 2,
    /*
     * Used for reductions to allow reordering the operation.  At this point
     * assume that if set, it also applies to normal operations though!
     */
    NPY_METH_IS_REORDERABLE = 1 << 3,
    /*
     * Private flag for now for *logic* functions.  The logical functions
     * `logical_or` and `logical_and` can always cast the inputs to booleans
     * "safely" (because that is how the cast to bool is defined).
     * @seberg: I am not sure this is the best way to handle this, so its
     * private for now (also it is very limited anyway).
     * There is one "exception". NA aware dtypes cannot cast to bool
     * (hopefully), so the `??->?` loop should error even with this flag.
     * But a second NA fallback loop will be necessary.
     */
    _NPY_METH_FORCE_CAST_INPUTS = 1 << 17,

    /* All flags which can change at runtime */
    NPY_METH_RUNTIME_FLAGS = (
            NPY_METH_REQUIRES_PYAPI |
            NPY_METH_NO_FLOATINGPOINT_ERRORS),
} NPY_ARRAYMETHOD_FLAGS;


typedef struct PyArrayMethod_Context_tag {
    /* The caller, which is typically the original ufunc.  May be NULL */
    PyObject *caller; 
    /* The method "self".  Publically currentl an opaque object. */
    struct PyArrayMethodObject_tag *method;

    /* Operand descriptors, filled in by resolve_descriptors */
    PyArray_Descr **descriptors;
    /* Structure may grow (this is harmless for DType authors) */
} PyArrayMethod_Context;


/*
 * The main object for creating a new ArrayMethod. We use the typical `slots`
 * mechanism used by the Python limited API (see below for the slot defs).
 */
typedef struct {
    const char *name;
    int nin, nout;
    NPY_CASTING casting;
    NPY_ARRAYMETHOD_FLAGS flags;
    PyArray_DTypeMeta **dtypes;
    PyType_Slot *slots;
} PyArrayMethod_Spec;


/*
 * ArrayMethod slots
 * -----------------
 * 
 * SLOTS IDs For the ArrayMethod creation, once fully public, IDs are fixed
 * but can be deprecated and arbitrarily extended.
 */
#define NPY_METH_resolve_descriptors 1
/* We may want to adapt the `get_loop` signature a bit: */
#define _NPY_METH_get_loop 2
#define NPY_METH_get_reduction_initial 3
/* specific loops for constructions/default get_loop: */
#define NPY_METH_strided_loop 4
#define NPY_METH_contiguous_loop 5
#define NPY_METH_unaligned_strided_loop 6
#define NPY_METH_unaligned_contiguous_loop 7
#define NPY_METH_contiguous_indexed_loop 8

/* other slots are in order, so note last one (internal use!) */
#define _NPY_NUM_DTYPE_SLOTS 8

/*
 * The resolve descriptors function, must be able to handle NULL values for
 * all output (but not input) `given_descrs` and fill `loop_descrs`.
 * Return -1 on error or 0 if the operation is not possible without an error
 * set.  (This may still be in flux.)
 * Otherwise must return the "casting safety", for normal functions, this is
 * almost always "safe" (or even "equivalent"?).
 *
 * `resolve_descriptors` is optional if all output DTypes are non-parametric.
 */
typedef NPY_CASTING (resolve_descriptors_function)(
        /* "method" is currently opaque (necessary e.g. to wrap Python) */
        struct PyArrayMethodObject_tag *method,
        /* DTypes the method was created for */
        PyArray_DTypeMeta **dtypes,
        /* Input descriptors (instances).  Outputs may be NULL. */
        PyArray_Descr **given_descrs,
        /* Exact loop descriptors to use, must not hold references on error */
        PyArray_Descr **loop_descrs,
        npy_intp *view_offset);


typedef int (PyArrayMethod_StridedLoop)(PyArrayMethod_Context *context,
        char *const *data, const npy_intp *dimensions, const npy_intp *strides,
        NpyAuxData *transferdata);


typedef int (get_loop_function)(
        PyArrayMethod_Context *context,
        int aligned, int move_references,
        const npy_intp *strides,
        PyArrayMethod_StridedLoop **out_loop,
        NpyAuxData **out_transferdata,
        NPY_ARRAYMETHOD_FLAGS *flags);

/**
 * Query an ArrayMethod for the initial value for use in reduction.
 *
 * @param context The arraymethod context, mainly to access the descriptors.
 * @param reduction_is_empty Whether the reduction is empty. When it is, the
 *     value returned may differ.  In this case it is a "default" value that
 *     may differ from the "identity" value normally used.  For example:
 *     - `0.0` is the default for `sum([])`.  But `-0.0` is the correct
 *       identity otherwise as it preserves the sign for `sum([-0.0])`.
 *     - We use no identity for object, but return the default of `0` and `1`
 *       for the empty `sum([], dtype=object)` and `prod([], dtype=object)`.
 *       This allows `np.sum(np.array(["a", "b"], dtype=object))` to work.
 *     - `-inf` or `INT_MIN` for `max` is an identity, but at least `INT_MIN`
 *       not a good *default* when there are no items.
 * @param initial Pointer to initial data to be filled (if possible)
 *
 * @returns -1, 0, or 1 indicating error, no initial value, and initial being
 *     successfully filled.  Errors must not be given where 0 is correct, NumPy
 *     may call this even when not strictly necessary.
 */
typedef int (get_reduction_initial_function)(
        PyArrayMethod_Context *context, npy_bool reduction_is_empty,
        char *initial);


/*
 * The following functions are only used be the wrapping array method defined
 * in umath/wrapping_array_method.c
 */

/*
 * The function to convert the given descriptors (passed in to
 * `resolve_descriptors`) and translates them for the wrapped loop.
 * The new descriptors MUST be viewable with the old ones, `NULL` must be
 * supported (for outputs) and should normally be forwarded.
 *
 * The function must clean up on error.
 *
 * NOTE: We currently assume that this translation gives "viewable" results.
 *       I.e. there is no additional casting related to the wrapping process.
 *       In principle that could be supported, but not sure it is useful.
 *       This currently also means that e.g. alignment must apply identically
 *       to the new dtypes.
 *
 * TODO: Due to the fact that `resolve_descriptors` is also used for `can_cast`
 *       there is no way to "pass out" the result of this function.  This means
 *       it will be called twice for every ufunc call.
 *       (I am considering including `auxdata` as an "optional" parameter to
 *       `resolve_descriptors`, so that it can be filled there if not NULL.)
 */
typedef int translate_given_descrs_func(int nin, int nout,
        PyArray_DTypeMeta *wrapped_dtypes[],
        PyArray_Descr *given_descrs[], PyArray_Descr *new_descrs[]);

/**
 * The function to convert the actual loop descriptors (as returned by the
 * original `resolve_descriptors` function) to the ones the output array
 * should use.
 * This function must return "viewable" types, it must not mutate them in any
 * form that would break the inner-loop logic.  Does not need to support NULL.
 *
 * The function must clean up on error.
 *
 * @param nargs Number of arguments
 * @param new_dtypes The DTypes of the output (usually probably not needed)
 * @param given_descrs Original given_descrs to the resolver, necessary to
 *        fetch any information related to the new dtypes from the original.
 * @param original_descrs The `loop_descrs` returned by the wrapped loop.
 * @param loop_descrs The output descriptors, compatible to `original_descrs`.
 *
 * @returns 0 on success, -1 on failure.
 */
typedef int translate_loop_descrs_func(int nin, int nout,
        PyArray_DTypeMeta *new_dtypes[], PyArray_Descr *given_descrs[],
        PyArray_Descr *original_descrs[], PyArray_Descr *loop_descrs[]);


/*
 * ****************************
 *          DTYPE API
 * ****************************
 */

#define NPY_DT_ABSTRACT 1 << 1
#define NPY_DT_PARAMETRIC 1 << 2
#define NPY_DT_NUMERIC 1 << 3

#define NPY_DT_discover_descr_from_pyobject 1
#define _NPY_DT_is_known_scalar_type 2
#define NPY_DT_default_descr 3
#define NPY_DT_common_dtype 4
#define NPY_DT_common_instance 5
#define NPY_DT_ensure_canonical 6
#define NPY_DT_setitem 7
#define NPY_DT_getitem 8


// TODO: These slots probably still need some thought, and/or a way to "grow"?
typedef struct {
    PyTypeObject *typeobj;    /* type of python scalar or NULL */
    int flags;                /* flags, including parametric and abstract */
    /* NULL terminated cast definitions. Use NULL for the newly created DType */
    PyArrayMethod_Spec **casts;
    PyType_Slot *slots;
    /* Baseclass or NULL (will always subclass `np.dtype`) */
    PyTypeObject *baseclass;
} PyArrayDTypeMeta_Spec;


typedef PyArray_Descr *(discover_descr_from_pyobject_function)(
        PyArray_DTypeMeta *cls, PyObject *obj);

/*
 * Before making this public, we should decide whether it should pass
 * the type, or allow looking at the object. A possible use-case:
 * `np.array(np.array([0]), dtype=np.ndarray)`
 * Could consider arrays that are not `dtype=ndarray` "scalars".
 */
typedef int (is_known_scalar_type_function)(
        PyArray_DTypeMeta *cls, PyTypeObject *obj);

typedef PyArray_Descr *(default_descr_function)(PyArray_DTypeMeta *cls);
typedef PyArray_DTypeMeta *(common_dtype_function)(
        PyArray_DTypeMeta *dtype1, PyArray_DTypeMeta *dtype2);
typedef PyArray_Descr *(common_instance_function)(
        PyArray_Descr *dtype1, PyArray_Descr *dtype2);
typedef PyArray_Descr *(ensure_canonical_function)(PyArray_Descr *dtype);

/*
 * TODO: These two functions are currently only used for experimental DType
 *       API support.  Their relation should be "reversed": NumPy should
 *       always use them internally.
 *       There are open points about "casting safety" though, e.g. setting
 *       elements is currently always unsafe.
 */
typedef int(setitemfunction)(PyArray_Descr *, PyObject *, char *);
typedef PyObject *(getitemfunction)(PyArray_Descr *, char *);


#endif  /* NUMPY_CORE_INCLUDE_NUMPY___DTYPE_API_H_ */

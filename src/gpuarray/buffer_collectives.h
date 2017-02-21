#ifndef GPUARRAY_BUFFER_COLLECTIVES_H
#define GPUARRAY_BUFFER_COLLECTIVES_H

#include <gpuarray/buffer.h>
#include <gpuarray/config.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus
#ifdef CONFUSE_EMACS
}
#endif  // CONFUSE_EMACS

/*****************************************************************************
*                   Multi-gpu collectives buffer interface                   *
******************************************************************************/

/**
 * Multi-gpu communicator structure.
 */
struct _gpucomm;

typedef struct _gpucomm gpucomm;

/**
 * Enum for reduce ops of gpucomm
 */
enum _gpucomm_reduce_ops {
  GA_SUM = 0,   //!< to sum (elemwise) arrays across ranks
  GA_PROD = 1,  //!< to multiply (elemwise) arrays across ranks
  GA_MAX = 2,   //!< to find max (elemwise) of arrays across ranks
  GA_MIN = 3,   //!< to find min (elemwise) of arrays across ranks
};

#define GA_COMM_ID_BYTES 128  //!< sizeof(gpucommCliqueId)

/**
 * Dummy struct to define byte-array's length through a type
 */
typedef struct _gpucommCliqueId {
  char internal[GA_COMM_ID_BYTES];
} gpucommCliqueId;

/**
 * \brief Create a new gpu communicator instance.
 *
 * \param comm pointer to get a new gpu communicator
 * \param ctx gpu context in which `comm` will be used (contains
 * device information)
 * \param comm_id id unique to communicators consisting a world
 * \param ndev number of communicators/devices participating in the world
 * \param rank user-defined rank, from 0 to `ndev`-1, of `comm` in the
 * world
 *
 * \note `rank` is defined to be unique for each new `comm`
 * participating in the same world.
 *
 * \note Must be called in parallel by all separate new `comm`, which
 * will consist a new world (failing will lead to deadlock).
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_new(gpucomm** comm, gpucontext* ctx,
                                gpucommCliqueId comm_id, int ndev, int rank);

/**
 * Destroy a gpu communicator instance.
 *
 * \param comm gpu communicator to be destroyed
 */
GPUARRAY_PUBLIC void gpucomm_free(gpucomm* comm);

/**
 * Returns nice error message concerning \ref GA_COMM_ERROR.
 *
 * \param ctx gpu context in which communicator was used
 *
 * \return const char* useful backend error message
 */
GPUARRAY_PUBLIC const char* gpucomm_error(gpucontext* ctx);

/**
 * Returns gpu context in which `comm` is used.
 *
 * \param comm gpu communicator
 *
 * \return gpucontext* gpu context
 */
GPUARRAY_PUBLIC gpucontext* gpucomm_context(gpucomm* comm);

/**
 * Creates a unique `comm_id` to be shared in a world of communicators.
 *
 * \param ctx gpu context
 * \param comm_id pointer to instance containing id
 *
 * \note Id is guaranteed to be unique across callers in a single host.
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_gen_clique_id(gpucontext* ctx,
                                          gpucommCliqueId* comm_id);

/**
 * Returns total number of device/communicators participating in
 * `comm`'s world.
 *
 * \param comm gpu communicator
 * \param gpucount pointer to number of gpus in `comm`'s world
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_get_count(gpucomm* comm, int* gpucount);

/**
 * Returns rank of `comm` inside its world as defined by user upon
 * creation.
 *
 * \param comm gpu communicator
 * \param rank pointer to `comm`'s rank
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_get_rank(gpucomm* comm, int* rank);

/**
 * Reduce collective operation for ranks in a communicator world
 * [buffer level].
 *
 * \param src data in device's buffer to be reduced
 * \param offsrc memory offset after which data is saved in buffer
 * `src`
 * \param dest data in device's buffer to collect result
 * \param offdest memory offset after which data will be saved in
 * buffer `dest`
 * \param count number of elements to be reduced in each array
 * \param typecode code for elements' data type, see \ref enum
 * GPUARRAY_TYPES
 * \param opcode reduce operation code, see \ref enum _gpucomm_reduce_ops
 * \param root rank in `comm` which will collect result
 * \param comm gpu communicator
 *
 * \note Non root ranks can call this, using a NULL `dest`. In this
 * case, `offdest` will not be used.
 *
 * \note Must be called separately for each rank in `comm`.
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_reduce(gpudata* src, size_t offsrc, gpudata* dest,
                                   size_t offdest, size_t count, int typecode,
                                   int opcode, int root, gpucomm* comm);

/**
 * AllReduce collective operation for ranks in a communicator world
 * [buffer level].
 *
 * Reduces data pointed by `src` using op operation and leaves
 * identical copies of result in data pointed by `dest` on each rank
 * of `comm`.
 *
 * \param src data in device's buffer to be reduced
 * \param offsrc memory offset after which data is saved in buffer
 * `src`
 * \param dest data in device's buffer to collect result
 * \param offdest memory offset after which data will be saved in
 * buffer `dest`
 * \param count number of elements to be reduced in each array
 * \param typecode code for elements' data type, see \ref enum
 * GPUARRAY_TYPES
 * \param opcode reduce operation code, see \ref enum _gpucomm_reduce_ops
 * \param comm gpu communicator
 *
 * \note Must be called separately for each rank in `comm`.
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_all_reduce(gpudata* src, size_t offsrc,
                                       gpudata* dest, size_t offdest,
                                       size_t count, int typecode, int opcode,
                                       gpucomm* comm);

/**
 * ReduceScatter collective operation for ranks in a communicator
 * world [buffer level].
 *
 * Reduces data pointed by `src` using `opcode` operation and leaves
 * reduced result scattered over data pointed by `dest` in the
 * user-defined rank order in `comm`.
 *
 * \param src data in device's buffer to be reduced
 * \param offsrc memory offset after which data is saved in buffer
 * `src`
 * \param dest data in device's buffer to collect scattered result
 * \param offdest memory offset after which data will be saved in
 * buffer `dest`
 * \param count number of elements to be contained in result `dest`
 * \param typecode code for elements' data type, see \ref enum
 * GPUARRAY_TYPES
 * \param opcode reduce operation code, see \ref enum _gpucomm_reduce_ops
 * \param comm gpu communicator
 *
 * \note Must be called separately for each rank in `comm`.
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_reduce_scatter(gpudata* src, size_t offsrc,
                                           gpudata* dest, size_t offdest,
                                           size_t count, int typecode,
                                           int opcode, gpucomm* comm);

/**
 * Broadcast collective operation for ranks in a communicator world
 * [buffer level].
 *
 * Copies data pointed by `array` to all ranks in `comm`.
 *
 * \param array data in device's buffer to get copied or be received
 * \param offset memory offset after which data in `array` begin
 * \param count number of elements to be contained in `array`
 * \param typecode code for elements' data type, see \ref enum
 * GPUARRAY_TYPES
 * \param root rank in `comm` which broadcasts its array
 * \param comm gpu communicator
 *
 * \note Must be called separately for each rank in `comm`.
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_broadcast(gpudata* array, size_t offset,
                                      size_t count, int typecode, int root,
                                      gpucomm* comm);

/**
 * AllGather collective operation for ranks in a communicator world.
 *
 * Each rank receives all data pointed by `src` of every rank in the
 * user-defined rank order in `comm`.
 *
 * \param src data in device's buffer to be gathered
 * \param offsrc memory offset after which data in `src` begin
 * \param dest data in device's buffer to gather from all ranks
 * \param offdest memory offset after which data in `dest` begin
 * \param count number of elements to be gathered from each rank in
 * `src`
 * \param typecode code for elements' data type, see \ref enum
 * GPUARRAY_TYPES
 * \param comm gpu communicator
 *
 * \note Must be called separately for each rank in `comm`.
 *
 * \return int error code, \ref GA_NO_ERROR if success
 */
GPUARRAY_PUBLIC int gpucomm_all_gather(gpudata* src, size_t offsrc,
                                       gpudata* dest, size_t offdest,
                                       size_t count, int typecode,
                                       gpucomm* comm);

#ifdef __cplusplus
}
#endif

#endif  // GPUARRAY_BUFFER_COLLECTIVES_H

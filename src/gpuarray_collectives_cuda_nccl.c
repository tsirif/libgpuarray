#include <assert.h>
#include <stdlib.h>

#include <nccl.h>

#include "gpuarray/buffer_collectives.h"
#include "gpuarray/util.h"
#include "gpuarray/error.h"
#include "gpuarray/config.h"

#include "private.h"
#include "private_cuda.h"


static const char* nccl_success_error = ncclGetErrorString(ncclSuccess);

#define NCCL_CHKFAIL(ctx, cmd) \
do { \
  ncclResult_t nccl_err = (cmd); \
  if (nccl_err != ncclSuccess) { \
    (ctx)->comm_error = ncclGetErrorString(nccl_err); \
    return GA_COMM_ERROR; \
  } \
  (ctx)->comm_error = nccl_success_error; \
  return GA_NO_ERROR; \
} while (0)

#define NCCL_EXIT_ON_ERROR(ctx, cmd) \
do { \
  ncclResult_t nccl_err = (cmd); \
  if (nccl_err != ncclSuccess) { \
    cuda_exit((ctx)); \
    (ctx)->comm_error = ncclGetErrorString(nccl_err); \
    return GA_COMM_ERROR; \
  } \
  (ctx)->comm_error = nccl_success_error; \
} while (0)

#define GA_CHECK(cmd) \
do { \
  int err = (cmd); \
  if (err != GA_NO_ERROR) \
    return err; \
} while (0)

#define GA_EXIT_ON_ERROR(ctx, cmd) \
do { \
  int err = (cmd); \
  if (err != GA_NO_ERROR) { \
    cuda_exit((ctx)); \
    return err; \
  } \
} while (0)

extern const gpuarray_buffer_ops cuda_ops;

/**
 * Definition of struct _gpucomm
 *
 * Done here in order to avoid ifdefs concerning nccl's existance in core code.
 *
 * \note This must be the only "module" which manages the definition's contents.
 */
struct _gpucomm {
  cuda_context* ctx;  // Start after the context
  ncclComm_t c;
#ifdef DEBUG
  char tag[8];
#endif
};

static void comm_clear(gpucomm* comm)
{
  cuda_ops.buffer_deinit((gpucontext*) comm->ctx);
  CLEAR(comm);
  free(comm);
}

static int comm_new(gpucomm** comm_ptr, gpucontext* ctx, gpucommCliqueId comm_id,
                    int ndev, int rank) {
  ASSERT_CTX(ctx);
  gpucomm* comm;
  comm = calloc(1, sizeof(*comm));
  if (comm == NULL) {
    *comm_ptr = NULL;
    return GA_MEMORY_ERROR;
  }
  comm->ctx = (cuda_context*) ctx;
  comm->ctx->refcnt++;  // So that ctx would not be destroyed before comm
  cuda_enter(comm->ctx);
  ncclResult_t nccl_err = ncclCommInitRank(&comm->c, ndev,
                                           *((ncclUniqueId*) &comm_id), rank);
  cuda_exit(comm->ctx);
  TAG_COMM(comm);
  if (nccl_err != ncclSuccess) {
    *comm_ptr = NULL;
    comm_clear(comm);
    ctx->comm_error = ncclGetErrorString(nccl_err);
    return GA_COMM_ERROR;
  }
  *comm_ptr = comm;
  ctx->comm_error = nccl_success_error;
  return GA_NO_ERROR;
}

static void comm_free(gpucomm* comm) {
  ASSERT_COMM(comm);
  cuda_enter(comm->ctx);
  ncclCommDestroy(comm->c);
  cuda_exit(comm->ctx);
  comm_clear(comm);
}

static int generate_clique_id(gpucontext* c, gpucommCliqueId* comm_id) {
  ASSERT_CTX(c);
  NCCL_CHKFAIL(c, ncclGetUniqueId((ncclUniqueId*) comm_id));
}

static int get_count(const gpucomm* comm, int* count) {
  ASSERT_COMM(comm);
  NCCL_CHKFAIL(comm->ctx, ncclCommCount(comm->c, count));
}

static int get_rank(const gpucomm* comm, int* rank) {
  ASSERT_COMM(comm);
  NCCL_CHKFAIL(comm->ctx, ncclCommUserRank(comm->c, rank));
}

static inline ncclRedOp_t convert_reduce_op(int opcode) {
  switch(opcode) {
  case GA_SUM : return ncclSum;
  case GA_PROD: return ncclProd;
  case GA_MAX : return ncclMax;
  case GA_MIN : return ncclMin;
  }
  return nccl_NUM_OPS;
}

static inline ncclDataType_t convert_data_type(int typecode) {
  switch(typecode) {
  case GA_BYTE  : return ncclChar;
  case GA_INT   : return ncclInt;
#ifdef CUDA_HAS_HALF
  case GA_HALF  : return ncclHalf;
#endif  // CUDA_HAS_HALF
  case GA_FLOAT : return ncclFloat;
  case GA_DOUBLE: return ncclDouble;
  case GA_LONG  : return ncclInt64;
  case GA_ULONG : return ncclUint64;
  }
  return nccl_NUM_TYPES;
}

static inline int check_restrictions(gpudata* src, size_t offsrc,
                                     gpudata* dest, size_t offdest, int count,
                                     int typecode, int opcode, gpucomm* comm,
                                     ncclDataType_t* datatype, ncclRedOp_t* op) {
  // src, dest and comm must refer to the same context
  if (src->ctx != comm->ctx)
    return GA_VALUE_ERROR;
  if (dest != NULL && dest->ctx != comm->ctx)
    return GA_VALUE_ERROR;
  // typecode must correspond to a valid ncclDataType_t
  if (datatype != NULL) {
    *datatype = convert_data_type(typecode);
    if (*datatype == nccl_NUM_TYPES)
      return GA_VALUE_ERROR;
  }
  // opcode must correspond to a valid ncclRedOp_t
  if (op != NULL) {
    *op = convert_reduce_op(opcode);
    if (*op == nccl_NUM_OPS)
      return GA_VALUE_ERROR;
  }
  // size to operate upon must be able to fit inside the gpudata (incl offsets)
  size_t op_size = count * gpuarray_get_elsize(typecode);
  if ((src->sz - offsrc) < op_size)
    return GA_VALUE_ERROR;
  if (dest != NULL && (dest->sz - offdest) < op_size)
    return GA_VALUE_ERROR;
  // offsets must not be larger than gpudata's size itself
  // (else out of alloc-ed mem scope)
  assert(!(offsrc > src->sz));
  assert(!(dest != NULL & offdest > dest->sz));
  return GA_NO_ERROR;
}

static int reduce(gpudata* src, size_t offsrc,
                  gpudata* dest, size_t offdest,
                  int count, int typecode, int opcode,
                  int root, gpucomm* comm) {
  ncclRedOp_t op;
  ncclDataType_t datatype;
  ASSERT_BUF(src);
  ASSERT_COMM(comm);
  gpudata* dst = NULL;
  int rank = 0;
  GA_CHECK(get_rank(comm, &rank));
  if (rank == root) {
    dst = dest;
    ASSERT_BUF(dest);
  }
  GA_CHECK(check_restrictions(src, offsrc, dst, offdest,
                              count, typecode, opcode, comm,
                              &datatype, &op));

  cuda_context* ctx = comm->ctx;
  cuda_enter(ctx);

  // sync: wait till a write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(src, CUDA_WAIT_READ));
  // sync: wait till a read/write has finished (out of concurrent kernels)
  if (rank == root)
    GA_EXIT_ON_ERROR(ctx, cuda_wait(dest, CUDA_WAIT_WRITE));

  // change stream of nccl ops to enable concurrency
  if (rank == root)
    NCCL_EXIT_ON_ERROR(ctx, ncclReduce((void*) (src->ptr + offsrc),
                                       (void*) (dest->ptr + offdest),
                                       count, datatype, op, root, comm->c, ctx->s));
  else
    NCCL_EXIT_ON_ERROR(ctx, ncclReduce((void*) (src->ptr + offsrc), NULL,
                                       count, datatype, op, root, comm->c, ctx->s));

  GA_EXIT_ON_ERROR(ctx, cuda_record(src, CUDA_WAIT_READ));
  if (rank == root)
    GA_EXIT_ON_ERROR(ctx, cuda_record(dest, CUDA_WAIT_WRITE));

  cuda_exit(ctx);

  return GA_NO_ERROR;
}

static int all_reduce(gpudata* src, size_t offsrc,
                      gpudata* dest, size_t offdest,
                      int count, int typecode, int opcode,
                      gpucomm* comm) {
  ncclRedOp_t op;
  ncclDataType_t datatype;
  ASSERT_BUF(src);
  ASSERT_COMM(comm);
  ASSERT_BUF(dest);
  GA_CHECK(check_restrictions(src, offsrc, dest, offdest,
                              count, typecode, opcode, comm,
                              &datatype, &op));

  cuda_context* ctx = comm->ctx;
  cuda_enter(ctx);

  // sync: wait till a write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(src, CUDA_WAIT_READ));
  // sync: wait till a read/write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(dest, CUDA_WAIT_WRITE));

  // change stream of nccl ops to enable concurrency
  NCCL_EXIT_ON_ERROR(ctx, ncclAllReduce((void*) (src->ptr + offsrc),
                                        (void*) (dest->ptr + offdest),
                                        count, datatype, op, comm->c, ctx->s));

  GA_EXIT_ON_ERROR(ctx, cuda_record(src, CUDA_WAIT_READ));
  GA_EXIT_ON_ERROR(ctx, cuda_record(dest, CUDA_WAIT_WRITE));

  cuda_exit(ctx);

  return GA_NO_ERROR;
}

static int reduce_scatter(gpudata* src, size_t offsrc,
                          gpudata* dest, size_t offdest,
                          int count, int typecode, int opcode,
                          gpucomm* comm) {
  ASSERT_BUF(src);
  ASSERT_COMM(comm);
  ASSERT_BUF(dest);
  ncclRedOp_t op;
  ncclDataType_t datatype;
  int ndev = 0;
  GA_CHECK(get_count(comm, &ndev));
  GA_CHECK(check_restrictions(src, offsrc, NULL, 0,
                              count * ndev, typecode, opcode, comm,
                              &datatype, &op));
  if (dest->ctx != comm->ctx)
    return GA_VALUE_ERROR;
  size_t resc_size = count * gpuarray_get_elsize(typecode);
  if ((dest->sz - offdest) < resc_size)
    return GA_VALUE_ERROR;
  assert(!(offdest > dest->sz));

  cuda_context* ctx = comm->ctx;
  cuda_enter(ctx);

  // sync: wait till a write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(src, CUDA_WAIT_READ));
  // sync: wait till a read/write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(dest, CUDA_WAIT_WRITE));

  // change stream of nccl ops to enable concurrency
  NCCL_EXIT_ON_ERROR(ctx, ncclReduceScatter((void*) (src->ptr + offsrc),
                                            (void*) (dest->ptr + offdest),
                                            count, datatype, op, comm->c, ctx->s));

  GA_EXIT_ON_ERROR(ctx, cuda_record(src, CUDA_WAIT_READ));
  GA_EXIT_ON_ERROR(ctx, cuda_record(dest, CUDA_WAIT_WRITE));

  cuda_exit(ctx);

  return GA_NO_ERROR;
}

static int broadcast(gpudata* array, size_t offset,
                     int count, int typecode,
                     int root, gpucomm* comm) {
  ASSERT_BUF(src);
  ASSERT_COMM(comm);
  ASSERT_BUF(dest);
  ncclDataType_t datatype;
  GA_CHECK(check_restrictions(array, offset, NULL, 0,
                              count, typecode, 0, comm,
                              &datatype, NULL));

  cuda_context* ctx = comm->ctx;
  cuda_enter(ctx);

  // sync: wait till a write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(array, CUDA_WAIT_READ));

  // change stream of nccl ops to enable concurrency
  NCCL_EXIT_ON_ERROR(ctx, ncclBcast((void*) (array->ptr + offset),
                                    count, datatype, root, comm->c, ctx->s));

  GA_EXIT_ON_ERROR(ctx, cuda_record(array, CUDA_WAIT_READ));

  cuda_exit(ctx);

  return GA_NO_ERROR;
}

static int all_gather(gpudata* src, size_t offsrc,
                      gpudata* dest, size_t offdest,
                      int count, int typecode,
                      gpucomm* comm) {
  ASSERT_BUF(src);
  ASSERT_COMM(comm);
  ASSERT_BUF(dest);
  ncclDataType_t datatype;
  GA_CHECK(check_restrictions(src, offsrc, NULL, 0,
                              count, typecode, 0, comm,
                              &datatype, NULL));
  if (dest->ctx != comm->ctx)
    return GA_VALUE_ERROR;
  int ndev = 0;
  GA_CHECK(get_count(comm, &ndev));
  size_t resc_size = ndev * count * gpuarray_get_elsize(typecode);
  if ((dest->sz - offdest) < resc_size)
    return GA_VALUE_ERROR;
  assert(!(offdest > dest->sz));

  cuda_context* ctx = comm->ctx;
  cuda_enter(ctx);

  // sync: wait till a write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(src, CUDA_WAIT_READ));
  // sync: wait till a read/write has finished (out of concurrent kernels)
  GA_EXIT_ON_ERROR(ctx, cuda_wait(dest, CUDA_WAIT_WRITE));

  // change stream of nccl ops to enable concurrency
  NCCL_EXIT_ON_ERROR(ctx, ncclAllGather((void*) (src->ptr + offsrc), count,
                                        datatype, (void*) (dest->ptr + offdest),
                                        comm->c, ctx->s));

  GA_EXIT_ON_ERROR(ctx, cuda_record(src, CUDA_WAIT_READ));
  GA_EXIT_ON_ERROR(ctx, cuda_record(dest, CUDA_WAIT_WRITE));

  cuda_exit(ctx);

  return GA_NO_ERROR;
}

GPUARRAY_LOCAL gpuarray_comm_ops nccl_ops = {
  comm_new,
  comm_free,
  generate_clique_id,
  get_count,
  get_rank,
  reduce,
  all_reduce,
  reduce_scatter,
  broadcast,
  all_gather
};

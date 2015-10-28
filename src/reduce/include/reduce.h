#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sharedmem.h>
#include <tad.h>
#include <indexing.h>


//an op for the kernel
template<typename T>
__device__ T op(T d1,T *extraParams);

//calculate an update of the reduce operation
template<typename T>
__device__ T update(T old,T opOutput,T *extraParams);
//invoked when combining two kernels
template<typename T>
__device__ T merge(T f1, T f2,T *extraParams);

//post process result (for things like means etc)
template<typename T>
__device__ T postProcess(T reduction,int n,int xOffset,T *dx,int incx,T *extraParams,T *result);


template<typename T>
__device__ T op(T d1,T d2,T *extraParams);


template <>  double merge<double>(double  opOutput,double other,double *extraParams);
template <> double update<double>(double old,double opOutput,double *extraParams);
template <> double op<double>(double d1,double *extraParams);
template <> double postProcess<double>(double reduction,int n,int xOffset,double *dx,int incx,double *extraParams,double *result);


template <> float merge<float>(float old,float opOutput,float *extraParams);
template <>float update<float>(float old,float opOutput,float *extraParams);
template <> float op<float>(float d1,float *extraParams);
template <> float postProcess<float>(float reduction,int n,int xOffset,float *dx,int incx,float *extraParams,float *result);




template<typename T>
__device__ T doBlock(int n,T *sPartials,T *dx,int xOffset,int incx,T *extraParams) {
	T sum = extraParams[0];
	for (int i = 0; i < n; i ++) {
		int currIdx = xOffset + i * incx;
		T curr = dx[currIdx];
		sum = update(sum,op(curr,extraParams),extraParams);
	}

	return sum;
}


template<typename T>
__device__ void aggregatePartials(T **sPartialsRef,int tid,T *extraParams) {
	// start the shared memory loop on the next power of 2 less
	// than the block size.  If block size is not a power of 2,
	// accumulate the intermediate sums in the remainder range.
	T *sPartials = *sPartialsRef;
	int floorPow2 = blockDim.x;

	if (floorPow2 & (floorPow2 - 1)) {
		while ( floorPow2 & (floorPow2 - 1) ) {
			floorPow2 &= floorPow2 - 1;
		}
		if (tid >= floorPow2) {
			sPartials[tid - floorPow2] = merge(sPartials[tid - floorPow2],sPartials[tid],extraParams);
		}
		__syncthreads();
	}

	for (int activeThreads = floorPow2 >> 1;activeThreads;	activeThreads >>= 1) {
		if (tid < activeThreads) {
			sPartials[tid] = merge(sPartials[tid],sPartials[tid + activeThreads],extraParams);
		}
		__syncthreads();
	}
}


template<typename T>
__global__ void doReduce(
		T *dx
		,T *extraParams
		,int n
		,ShapeInformation *xInfo,
		T *result,
		ShapeInformation *resultInfo) {
	SharedMemory<T> val;
	T *sPartials = val.getPointer();
	int tid = threadIdx.x;
	int start = blockDim.x * blockIdx.x + tid;
	int xOffset = start + xInfo->offset;
	int incx = xInfo->elementWiseStride;
	int resultOffset = start + blockIdx.x * resultInfo->elementWiseStride;
	T sum = doBlock(n,sPartials,dx,xOffset,incx,extraParams);
	sPartials[tid] = sum;
	__syncthreads();
	aggregatePartials(&sPartials,tid,extraParams);
	if (tid == 0) {
		result[start + resultOffset] = postProcess(sPartials[0],n,xOffset,dx,incx,extraParams,result);
	}
}




/**
 * @param n n is the number of
 *        elements to loop through
 * @param dx the data to operate on
 * @param xVectorInfo the meta data for the vector:
 *                              0 is the offset
 *                              1 is the increment/stride
 *                              2 is the real length of the buffer (n and dx.length won't always be the same)
 *                              3 is the element wise stride for the buffer
 *                              4 is the number of elements it takes to get to the next row/column/tensor
 * @param gpuInformation
 *                              0 is the block size
 *                              1 is the grid size
 *                              2 is the shared memory size
 * @param problemDefinition
 *                          0 is the number of elements per vector
 *                          1 is the number of vectors
 */
template<typename T>
__device__ void transform(
		int n
		,T *dx
		,int *xShapeInfo
		,T *extraParams
		,T *result,
		int *resultShapeInfo
		,int *gpuInformation,
		int *dimension,
		int dimensionLength) {


	__shared__ int nIsPow2;
	nIsPow2 = (n % 2 == 0);
	__syncthreads();
	/**
	 * Gpu information for the problem
	 */
	int tid = threadIdx.x;


	//shared shape information for a given block
	__shared__ ShapeInformation *xInfo;
	__shared__ ShapeInformation *resultInfo;



	//setup the shared shape information
	if(tid == 0)  {
		//initialize the shape information only once
		xInfo = infoFromBuffer(xShapeInfo);
		resultInfo =  infoFromBuffer(resultShapeInfo);
	}

	__syncthreads();
	//init the tad off




	__shared__ int xLength;
	if(tid == 0) {
		int *keep2 = keep(xInfo->shape,dimension,dimensionLength,xInfo->rank);
		xLength = prod(keep2,dimensionLength);
		free(keep2);
	}

	__syncthreads();


	__shared__ int resultScalar;
	resultScalar = isScalar(resultInfo);
	__syncthreads();

	//shared memory space for storing intermediate results
	SharedMemory<T> val;
	volatile T *sPartials = val.getPointer();
	if(tid == 0) {
		int sMemSize = gpuInformation[2];
		int sPartialsLength = sMemSize / sizeof(T);
		for(int i = 0; i < sPartialsLength; i++) {
			sPartials[i] = extraParams[0];
		}
	}

	__syncthreads();


	if(!resultScalar) {
		int offset3 = offset(blockIdx.x ,xInfo->rank,xInfo,dimension,dimensionLength);
		sPartials[tid] = dx[offset3 + tid * xInfo->elementWiseStride];

	}
	else {

		int blockSize = gpuInformation[0];

		// perform first level of reduction,
		// reading from global memory, writing to shared memory
		unsigned int tid = threadIdx.x;
		unsigned int i = blockIdx.x * blockSize * 2 + threadIdx.x;


		unsigned int gridSize = blockSize * 2 * gridDim.x;

		T reduction = extraParams[0];
		T curr;

		// we reduce multiple elements per thread.  The number is determined by the
		// number of active thread blocks (via gridDim).  More blocks will result
		// in a larger gridSize and therefore fewer elements per thread
		while (i < n)	{
			curr = dx[i];
			reduction = update(reduction,op(curr,extraParams),extraParams);


			// ensure we don't read out of bounds -- this is optimized away for powerOf2 sized arrays
			if (nIsPow2 || i + blockSize < n) {
				curr = dx[i + blockSize];
				reduction = update(reduction,op(curr,extraParams),extraParams);

			}

			i += gridSize;
		}

		// each thread puts its local sum into shared memory
		sPartials[tid] = reduction;
		__syncthreads();


		// do reduction in shared mem
		if ((blockSize >= 512) && (tid < 256)) {
			curr = sPartials[tid + 256];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();

		if ((blockSize >= 256) &&(tid < 128)) {
			curr = sPartials[tid + 128];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();

		if ((blockSize >= 128) && (tid <  64)) {
			curr = sPartials[tid + 64];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();


		// fully unroll reduction within a single warp
		if ((blockSize >=  64) && (tid < 32)) {
			curr = sPartials[tid + 32];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();

		if ((blockSize >=  32) && (tid < 16)) {
			curr = sPartials[tid + 16];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();

		if ((blockSize >=  16) && (tid <  8)) {
			curr = sPartials[tid + 8];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();

		if ((blockSize >=   8) && (tid <  4)) {
			curr = sPartials[tid + 4];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();

		if ((blockSize >=   4) && (tid <  2)) {
			curr = sPartials[tid + 2];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();

		if ((blockSize >=   2) && ( tid <  1)) {
			curr = sPartials[tid + 1];
			reduction = update(reduction,op(curr,extraParams),extraParams);
			sPartials[tid] = reduction;
		}

		__syncthreads();
		//__device__ void aggregatePartials(T **sPartialsRef,int tid,T *extraParams) {
		T ** pointerToPartials = (T **)&sPartials;
		aggregatePartials(pointerToPartials,tid,extraParams);

		// write result for this block to global mem
		if (tid == 0 && blockIdx.x == 0) {
			result[blockIdx.x] = postProcess(reduction,n,xInfo->offset,dx,xInfo->elementWiseStride,extraParams,result);
		}
	}


	__syncthreads();




	if(tid == 0) {
		if(!resultScalar) {
			T currResult = extraParams[0];
			for(int i = 0; i < xLength; i++) {
				currResult = update(currResult,op(sPartials[i],extraParams),extraParams);
			}

			result[blockIdx.x] = postProcess(currResult,xLength,xInfo->offset,dx,xInfo->elementWiseStride,extraParams,result);

		}

	}



}



//kernels defined to allow lookup by name: notice extern c
extern "C"
__global__ void transform_double(
		int n
		,double *dx
		,int *xShapeInfo
		,double *extraParams
		,double *result,
		int *resultShapeInfo
		,int *gpuInformation,
		int *dimension,
		int dimensionLength) {
	transform<double>(
			n,
			dx,
			xShapeInfo,
			extraParams,
			result,
			resultShapeInfo,
			gpuInformation,
			dimension,
			dimensionLength);
}


extern "C"
__global__ void transform_float(
		int n
		,float *dx
		,int *xShapeInfo
		,float *extraParams
		,float *result,
		int *resultShapeInfo
		,int *gpuInformation,
		int *dimension,
		int dimensionLength) {
	transform<float>(n,
			dx,
			xShapeInfo,
			extraParams,
			result,
			resultShapeInfo,
			gpuInformation,
			dimension,dimensionLength);
}


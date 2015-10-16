#include <scalar.h>

__device__ double op(double d1,double d2,double *params) {
	if(d1 < d2)
		return 1;
	return 0;
}

extern "C"
__global__ void min_scalar_double(int n, int idx,double dx,double *dy,int incy,double *params,double *result,int blockSize) {
	transform<double>(n,idx,dx,dy,incy,params,result,blockSize);
}


__device__ float op(float d1,float d2,float *params) {
	if(d1 < d2)
		return 1;
	return 0;
}

extern "C"
__global__ void min_scalar_float(int n, int idx,float dx,float *dy,int incy,float *params,float *result,int blockSize) {
	transform<float>(n,idx,dx,dy,incy,params,result,blockSize);
}

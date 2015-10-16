#include <transform.h>

__device__ double op(double d1,double *params) {
	return sqrt(d1);
}

extern "C"
__global__ void sqrt_strided_double(int n,int idx,double *dy,int incy,double *params,double *result,int blockSize) {
	transform<double>(n,idx,dy,incy,params,result,blockSize);

}

__device__ float op(float d1,float *params) {
	return sqrtf(d1);
}

extern "C"
__global__ void sqrt_strided_float(int n,int idx,float *dy,int incy,float *params,float *result,int blockSize) {
	transform<float>(n,idx,dy,incy,params,result,blockSize);

}

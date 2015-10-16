#include <scalar.h>

__device__ double op(double d1,double d2,double *params) {
   return d2 + d1;
}

__device__ float op(float d1,float d2,float *params) {
   return d2 + d1;
}

__global__ void add_scalar_double(int n, int idx,double dx,double *dy,int incx,double *params,double *result,int blockSize) {
       transform<double>(n,idx,dx,dy,incx,params,result,blockSize);
 }


__global__ void add_scalar_float(int n, int idx,float dx,float *dy,int incx,float *params,float *result,int blockSize) {
       transform<float>(n,idx,dx,dy,incx,params,result,blockSize);
 }

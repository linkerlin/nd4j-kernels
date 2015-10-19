#include <transform.h>

template<> __device__ double op<double>(double d1,double *params) {
      return log(1 + exp(d1));
}


__global__ void softplus_strided_double(int n,int idx,double *dy,int incy,double *params,double *result,int blockSize) {
       transform(n,idx,dy,incy,params,result,blockSize);

 }
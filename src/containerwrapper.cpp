#include "containerwrapper.h"

ContainerWrapper::ContainerWrapper() {}



//Container Wrapper structure outline:
//
// -> STRUCT CONTAINERPARAMS
//    Contains all the information needed for building runtime and executing runner.
//
//
//
//   Runtime building (runner-indifferent):
//   Build VFS
//   Handle registry
//   Basically put all subcomponents in place
//   Download or find the actual runners
//
//   Runner execution part.
//   I need to generalize the runner class so it works with all kinds of runners.

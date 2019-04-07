#include "operator/absval.hpp"
#include "static_graph.hpp"
namespace TEngine {
void AbsVal::SetSchema(void)
{
    Input({"input:float32"})
   .Output({"output:float32"})
   .SetDoc(R"DOC(AbsVal: only caffe flavor AbsVal)DOC");
}
} //namespace TEngine

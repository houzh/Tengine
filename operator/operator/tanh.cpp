#include "operator/tanh.hpp"
#include "static_graph.hpp"
namespace TEngine {
void TanH::SetSchema(void)
{
    Input({"input:float32"})
   .Output({"output:float32"})
   .SetDoc(R"DOC(TanH: only caffe flavor TanH)DOC");
}
} //namespace TEngine

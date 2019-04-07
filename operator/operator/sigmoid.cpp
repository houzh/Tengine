#include "operator/sigmoid.hpp"
#include "static_graph.hpp"
namespace TEngine {
void Sigmoid::SetSchema(void)
{
    Input({"input:float32"})
   .Output({"output:float32"})
   .SetDoc(R"DOC(Sigmoid: only caffe flavor sigmoid)DOC");
}
} //namespace TEngine

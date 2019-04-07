#include <iostream>
#include <functional>
#include <stdlib.h>

#include "logger.hpp"
#include "node_ops.hpp"
#include "tensor_mem.hpp"
#include "graph.hpp"
#include "operator/sigmoid.hpp"
#include <math.h>

namespace TEngine
{
namespace SigmoidImpl
{
struct SigmoidOps : public NodeOps
{
    bool Run(Node * node)
    {
        const Tensor * input_tensor=node->GetInputTensor(0);
        Tensor * output_tensor=node->GetOutputTensor(0);
        const TShape&  shape=input_tensor->GetShape();
        const std::vector<int> dims=shape.GetDim();
       int batch_number=dims[0];
        int channel_num=dims[1];
        int channel_size=dims[2]*dims[3];
        int img_size=channel_num*channel_size;
        
        const float * input=(const float *)get_tensor_mem(input_tensor);
        float * output=(float *)get_tensor_mem(output_tensor);

        for(int i=0;i<batch_number;i++)
        {
            for(int c=0;c<channel_num;c++)
            {
                int offset=i*img_size+c*channel_size;
                for(int l=0;l<channel_size;l++)
                {
                    output[offset+l]=1 / (1 + exp(-input[offset+l]));
                }
            }
        }
        return true;
    }
};
} //namespace SigmoidImpl

using namespace SigmoidImpl;
void RegisterSigmoid_NodeExec(void)
{
    SigmoidOps *ops = new SigmoidOps();
    NodeOpsRegistryManager::RegisterOPImplementor("common",
                                                  "Sigmoid", ops);
}
} //namespace TEngine

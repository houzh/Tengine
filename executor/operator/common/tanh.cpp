#include <iostream>
#include <functional>
#include <stdlib.h>

#include "logger.hpp"
#include "node_ops.hpp"
#include "tensor_mem.hpp"
#include "graph.hpp"
#include "operator/tanh.hpp"
#include <math.h>

namespace TEngine
{
namespace TanHImpl
{
struct TanHOps : public NodeOps
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
                    output[offset+l]=tanh(input[offset+l]);
                }
            }
        }
        return true;
    }
};
} //namespace TanHImpl

using namespace TanHImpl;
void RegisterTanH_NodeExec(void)
{
    TanHOps *ops = new TanHOps();
    NodeOpsRegistryManager::RegisterOPImplementor("common",
                                                  "TanH", ops);
}
} //namespace TEngine
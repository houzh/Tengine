
/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * License); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * AS IS BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright (c) 2018, Open AI Lab
 * Author: haitao@openailab.com
 */

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>

#include "tf_serializer.hpp"

#include "data_type.hpp"
#include "tengine_errno.hpp"

#include "operator/batch_norm_param.hpp"
#include "operator/concat_param.hpp"
#include "operator/conv_param.hpp"
#include "operator/eltwise.hpp"
#include "operator/fc_param.hpp"
#include "operator/gemm_param.hpp"
#include "operator/lrn_param.hpp"
#include "operator/pool_param.hpp"
#include "operator/relu_param.hpp"
#include "operator/reshape_param.hpp"
#include "operator/resize_param.hpp"
#include "operator/scale_param.hpp"
#include "operator/softmax_param.hpp"
#include "operator/generic_param.hpp"
#include "operator/lstm_param.hpp"
#include "operator_manager.hpp"
#include "type_name.hpp"

namespace TEngine {

using op_load_t = std::function<bool(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)>;

namespace tf_serializer {
static void CreateInputNode(TFNode* tf_node, StaticGraph* graph);
static bool LoadConstTensor(TFNode* tf_node, StaticGraph* graph);
static void GetTensorContentAndDim(const tensorflow::TensorProto& tf_tensor, std::vector<int>& dim, void** mem_ptr,
                                   std::string& layout);

}    // namespace tf_serializer

void TFSerializer::DumpTFGraph(TFGraph& tf_graph)
{
    int node_number = tf_graph.seq_nodes.size();

    LOG_INFO() << "total node number: " << node_number << "\n";

    for(int i = 0; i < node_number; i++)
    {
        TFNode* node = tf_graph.seq_nodes[i];

        LOG_INFO() << i << "\t" << node->name << " OP: " << node->op << " IN: " << node->inputs.size()
                   << " OUT: " << node->outputs.size() << " PB_DEFS: " << node->pb_defs.size() << "\n";

        for(unsigned int j = 0; j < node->inputs.size(); j++)
        {
            TFNode* input = node->inputs[j];
            LOG_INFO() << "\tI" << j << ": " << input->name << "  " << input->op << "\n";
        }

        for(unsigned int j = 0; j < node->outputs.size(); j++)
        {
            TFNode* output = node->outputs[j];
            LOG_INFO() << "\tO" << j << ": " << output->name << "  " << output->op << "\n";
        }
    }
}

bool TFSerializer::LoadModel(const std::vector<std::string>& file_list, StaticGraph* graph)
{
    tensorflow::GraphDef tf_net;

    if(    //! LoadTextFile(file_list[0].c_str(), tf_net) &&
        !LoadBinaryFile(file_list[0].c_str(), tf_net))
        return false;

    SetGraphSource(graph, file_list[0]);
    SetGraphSourceFormat(graph, "tensorflow");
    SetGraphConstTensorFile(graph, file_list[0]);

    return LoadGraph(tf_net, graph);
}

bool TFSerializer::LoadTextFile(const char* fname, tensorflow::GraphDef& tf_net)
{
    std::ifstream is(fname, std::ios::in);

    if(!is.is_open())
    {
        LOG_ERROR() << "cannot open file: " << fname << "\n";
        return false;
    }

    google::protobuf::io::IstreamInputStream input_stream(&is);
    bool ret = google::protobuf::TextFormat::Parse(&input_stream, &tf_net);

    is.close();

    return ret;
}

bool TFSerializer::LoadBinaryFile(const char* fname, tensorflow::GraphDef& tf_net)
{
    std::ifstream is(fname, std::ios::in | std::ios::binary);

    if(!is.is_open())
    {
        LOG_ERROR() << "cannot open file: " << fname << "\n";
        set_tengine_errno(ENOENT);
        return false;
    }

    google::protobuf::io::IstreamInputStream input_stream(&is);
    google::protobuf::io::CodedInputStream coded_input(&input_stream);

    coded_input.SetTotalBytesLimit(512 << 20, 64 << 20);

    bool ret = tf_net.ParseFromCodedStream(&coded_input);

    is.close();

    if(!ret)
    {
        LOG_ERROR() << "parse file: " << fname << " failed\n";
        set_tengine_errno(EINVAL);
    }

    return ret;
}

int TFSerializer::FindRNNScope(TFGraph& tf_graph, std::string& rnn_scope)
{
    std::string rnn_node;

    std::string::size_type while_pos;

    int rnn_type = -1;

    for(unsigned int i = 0; i < tf_graph.seq_nodes.size(); i++)
    {
        TFNode* node = tf_graph.seq_nodes.at(i);
        std::string& name = node->name;

        while_pos = name.find("while");

        if(while_pos == std::string::npos)
            continue;

        std::string::size_type cell_pos = name.find("lstm_cell", while_pos);

        if(cell_pos != std::string::npos)
        {
            rnn_node = node->name;
            rnn_type = TF_RNN_LSTM;
            break;
        }

        cell_pos = name.find("gru", while_pos);

        if(cell_pos != std::string::npos)
        {
            rnn_node = node->name;
            rnn_type = TF_RNN_GRU;
            break;
        }

        cell_pos = name.find("basic_lstm_cell", while_pos);

        if(cell_pos != std::string::npos)
        {
            rnn_node = node->name;
            rnn_type = TF_RNN_BASIC_LSTM;
            break;
        }
    }

    if(rnn_node.empty())
        return -1;

    std::string rnn_layer = rnn_node.substr(0, while_pos - 1);
    std::string::size_type up_pos = rnn_layer.rfind("/");

    rnn_scope = rnn_layer.substr(0, up_pos + 1);

    return rnn_type;
}

void TFSerializer::ParseLSTMGraph(TFGraph& tf_graph, LSTMNode* lstm_node, std::set<TFNode*>& rnn_graph)
{
    /* parse input node */

    for(unsigned int i = 0; i < lstm_node->inputs.size(); i++)
    {
        TFNode* node = lstm_node->inputs[i];

        if(node->op != "Const")
            continue;

        // node->no_static_node=true; //do not automatically create Static Node

        if(node->name.find("lstm_cell/kernel") != std::string::npos)
        {
            lstm_node->kernel = node;
        }
        else if(node->name.find("lstm_cell/bias") != std::string::npos)
        {
            lstm_node->bias = node;
        }
        else if(node->name.find("lstm_cell/w_f_diag") != std::string::npos)
        {
            lstm_node->w_f_diag = node;
        }
        else if(node->name.find("lstm_cell/w_o_diag") != std::string::npos)
        {
            lstm_node->w_o_diag = node;
        }
        else if(node->name.find("lstm_cell/w_i_diag") != std::string::npos)
        {
            lstm_node->w_i_diag = node;
        }
        else if(node->name.find("lstm_cell/projection/kernel") != std::string::npos)
        {
            lstm_node->projection = node;
        }
    }

    auto rnn_ir = rnn_graph.begin();
    auto rnn_ir_end = rnn_graph.end();

    while(rnn_ir != rnn_ir_end)
    {
        TFNode* node = *rnn_ir;
        int name_len = node->name.size();
        std::string zero_name = "LSTMCellZeroState/zeros";
        std::string zero1_name = "LSTMCellZeroState/zeros_1";
        std::string forget_name = "lstm_cell/add/y";

        if(node->name.find(zero_name, name_len - zero_name.size()) != std::string::npos)
            lstm_node->init_c = node;
        else if(node->name.find(zero1_name, name_len - zero1_name.size()) != std::string::npos)
            lstm_node->init_h = node;
        else if(node->name.find(forget_name, name_len - forget_name.size()) != std::string::npos)
            lstm_node->forget_bias = node;

        rnn_ir++;
    }
}

void TFSerializer::StripRNNScope(TFGraph& tf_graph, std::string& rnn_scope, int rnn_type)
{
    LSTMNode* lstm_node = new LSTMNode();

    lstm_node->name = rnn_scope + "lstm";
    lstm_node->op = "LSTM";

    std::set<TFNode*>& rnn_graph = lstm_node->rnn_graph;

    std::set<TFNode*> rnn_inputs;
    std::set<TFNode*> rnn_outputs;

    auto ir = tf_graph.seq_nodes.begin();
    std::string::size_type prefix_len = rnn_scope.size();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* node = *ir;

        if(node->name.find(rnn_scope.c_str(), 0, prefix_len) == std::string::npos)
        {
            ir++;
            continue;
        }

        /* this is a node, inside rnn scope, remove it from graph first */
        ir = tf_graph.seq_nodes.erase(ir);

        rnn_graph.insert(node);
    }

    auto rnn_ir = rnn_graph.begin();
    auto rnn_end = rnn_graph.end();

    while(rnn_ir != rnn_end)
    {
        TFNode* node = *rnn_ir;

        for(unsigned int i = 0; i < node->inputs.size(); i++)
        {
            TFNode* input = node->inputs[i];

            if(!rnn_graph.count(input))
                rnn_inputs.insert(input);
        }

        for(unsigned int i = 0; i < node->outputs.size(); i++)
        {
            TFNode* output = node->outputs[i];

            if(!rnn_graph.count(output))
                rnn_outputs.insert(output);
        }

        rnn_ir++;
    }

    // insert lstm node
    auto seq_ir = tf_graph.seq_nodes.begin();

    while(seq_ir != tf_graph.seq_nodes.end())
    {
        TFNode* node = *seq_ir;

        if(rnn_inputs.count(node))
        {
            tf_graph.seq_nodes.insert(seq_ir, lstm_node);
            break;
        }

        seq_ir++;
    }

    // connect inputs and outputs
    auto set_ir = rnn_inputs.begin();
    auto set_ir_end = rnn_inputs.end();

    while(set_ir != set_ir_end)
    {
        TFNode* input_node = *set_ir;

        for(unsigned int j = 0; j < input_node->outputs.size(); j++)
        {
            TFNode* child_node = input_node->outputs[j];

            if(rnn_graph.count(child_node))
                input_node->outputs[j] = lstm_node;
        }

        lstm_node->inputs.push_back(input_node);

        if(input_node->op == "Identity")
        {
            TFNode* parent_node = input_node->inputs[0];

            MergeChildNode(parent_node, input_node);
        }

        set_ir++;
    }

    set_ir = rnn_outputs.begin();
    set_ir_end = rnn_outputs.end();

    while(set_ir != set_ir_end)
    {
        TFNode* output_node = *set_ir;

        for(unsigned int j = 0; j < output_node->inputs.size(); j++)
        {
            TFNode* parent_node = output_node->inputs[j];

            if(rnn_graph.count(parent_node))
                output_node->inputs[j] = lstm_node;
        }

        lstm_node->outputs.push_back(output_node);
        set_ir++;
    }

    // collect attributes according to rnn_type

    if(rnn_type == TF_RNN_LSTM)
    {
        ParseLSTMGraph(tf_graph, lstm_node, rnn_graph);
    }

    // cleanup zero in/zero out node
    seq_ir = tf_graph.seq_nodes.begin();

    while(seq_ir != tf_graph.seq_nodes.end())
    {
        TFNode* node = *seq_ir;

        if(node->inputs.size() == 0 && node->outputs.size() == 0)
        {
            delete node;
            seq_ir = tf_graph.seq_nodes.erase(seq_ir);
        }
        else
        {
            seq_ir++;
        }
    }
}

bool TFSerializer::OptimizeRNN(tensorflow::GraphDef& tf_net, TFGraph& tf_graph)
{
    while(1)
    {
        std::string rnn_scope;

        int rnn_type = FindRNNScope(tf_graph, rnn_scope);

        if(rnn_scope.empty())
            break;

        StripRNNScope(tf_graph, rnn_scope, rnn_type);
    }

    return true;
}

bool TFSerializer::LoadGraph(tensorflow::GraphDef& tf_net, StaticGraph* graph)
{
    TFGraph tf_graph;

    // step 1: construct whole graph

    if(!ConstructGraph(tf_net, tf_graph))
        return false;

    if(!OptimizeRNN(tf_net, tf_graph))
        return false;

    // step 2: scanning and fusing nodes

    if(!OptimizeGraph(tf_graph))
        return false;

    // step 3: create static graph
    if(!GenerateStaticGraph(tf_graph, graph))
        return false;

    return true;
}

bool TFSerializer::ConstructGraph(tensorflow::GraphDef& tf_net, TFGraph& tf_graph)
{
    int node_number = tf_net.node_size();
    std::unordered_map<std::string, TFNode*> node_map;

    /* first scan, setup all nodes */

    for(int i = 0; i < node_number; i++)
    {
        const tensorflow::NodeDef& node_param = tf_net.node(i);

        TFNode* tf_node = new TFNode();

        tf_node->idx = i;
        tf_node->name = node_param.name();
        tf_node->op = node_param.op();
        tf_node->pb_defs.push_back(&tf_net.node(i));

        tf_graph.seq_nodes.push_back(tf_node);

        node_map[tf_node->name] = tf_node;
    }

    /* the second scan, setup connections */
    for(int i = 0; i < node_number; i++)
    {
        const tensorflow::NodeDef& node_param = tf_net.node(i);
        const std::string& name = node_param.name();

        TFNode* cur_node = node_map[name];

        for(int j = 0; j < node_param.input_size(); j++)
        {
            const std::string& input_name = node_param.input(j);

            std::string::size_type pos = input_name.find(":");
            std::string cleanup_name;

            if(pos == std::string::npos)
                pos = input_name.size();

            if(input_name[0] == '^')
                cleanup_name = input_name.substr(1, pos);
            else
                cleanup_name = input_name.substr(0, pos);

            TFNode* input_node = node_map[cleanup_name];

            if(input_node == nullptr)
            {
                XLOG_ERROR() << "cannot find input: " << input_name << " for node: " << name << "\n";
                return false;
            }

            cur_node->inputs.push_back(input_node);
            input_node->outputs.push_back(cur_node);
        }
    }

    return true;
}

void TFSerializer::DisconnectNode(TFNode* cur_node)
{
    TFNode* input_node;

    for(unsigned int i = 0; i < cur_node->inputs.size(); i++)
    {
        input_node = cur_node->inputs[i];

        auto ir = input_node->outputs.begin();

        while(ir != input_node->outputs.end())
        {
            if(*ir != cur_node)
                ir++;
            else
                break;
        }

        if(ir == input_node->outputs.end())
        {
            XLOG_ERROR() << "ERROR on node connection!!\n";
        }

        input_node->outputs.erase(ir);
    }

    cur_node->inputs.clear();

    TFNode* output_node;

    for(unsigned int i = 0; i < cur_node->outputs.size(); i++)
    {
        output_node = cur_node->outputs[i];

        auto ir = output_node->inputs.begin();

        while(ir != output_node->inputs.end())
        {
            if(*ir != cur_node)
                ir++;
            else
                break;
        }

        if(ir == output_node->inputs.end())
        {
            XLOG_ERROR() << "ERROR on node connection!!\n";
        }

        output_node->inputs.erase(ir);
    }

    cur_node->outputs.clear();
}

bool TFSerializer::MergeParentNode(TFNode* base_node, TFNode* parent_node)
{
    /* remove the input for parent node */

    auto input_ir = base_node->inputs.begin();

    while(input_ir != base_node->inputs.end())
    {
        if(*input_ir == parent_node)
            break;

        input_ir++;
    }

    base_node->inputs.erase(input_ir);

    /* connect parent's input node and base node */

    base_node->inputs.insert(base_node->inputs.end(), parent_node->inputs.begin(), parent_node->inputs.end());

    for(auto node : parent_node->inputs)
    {
        for(unsigned int i = 0; i < node->outputs.size(); i++)
        {
            if(node->outputs[i] == parent_node)
            {
                node->outputs[i] = base_node;
                break;
            }
        }
    }

    /* bridge parent's output*/

    auto output_ir = parent_node->outputs.begin();

    while(output_ir != parent_node->outputs.end())
    {
        TFNode* node = *output_ir;

        if(node != base_node)
        {
            base_node->outputs.push_back(node);

            for(unsigned int i = 0; i < node->inputs.size(); i++)
            {
                if(node->inputs[i] == parent_node)
                {
                    node->inputs[i] = base_node;
                    break;
                }
            }
        }

        output_ir++;
    }

    /* handle TF definitions */

    base_node->pb_defs.insert(base_node->pb_defs.end(), parent_node->pb_defs.begin(), parent_node->pb_defs.end());

    // std::cout<<"base node: "<<base_node->name<<" merge parent: "<<parent_node->name<<"\n";

    parent_node->inputs.clear();
    parent_node->outputs.clear();

    return true;
}

bool TFSerializer::CheckComposedBNAdd(TFNode* cur_node)
{
    if(cur_node->op != "Add")
        return false;

    TFNode* input0 = cur_node->inputs[0];
    TFNode* input1 = cur_node->inputs[1];

    if(input0->op != "Mul" || input1->op != "Sub")
        return false;

    /* further check: /add_1 int name */
    if(cur_node->name.find("/add_1") != std::string::npos)
    {
        if(input0->name.find("/mul_1") != std::string::npos || input1->name.find("/mul_1") != std::string::npos)
            cur_node->BNAddType = 1;
        else
            cur_node->BNAddType = 0;

        return true;
    }

    return false;
}

void TFSerializer::BNRecursiveInputMerge(TFNode* node)
{
    bool mul_1_node = false;
    bool mul_node = false;
    if(node->name.find("/mul") != std::string::npos)
    {
        if(node->BNAddType == 1)
        {
            if(node->name.find("/mul_1") != std::string::npos)
            {
                mul_1_node = true;
            }
            else if(node->name.find("/mul_2") == std::string::npos)
            {
                // disconnect the connection between mul and mul2
                auto ir = node->outputs.begin();

                if((*ir)->name.find("/mul2") == std::string::npos)
                    ir++;

                TFNode* mul2_node = *ir;

                node->outputs.erase(ir);

                ir = mul2_node->inputs.begin();

                if((*ir)->name.find("/mul") == std::string::npos)
                    ir++;

                mul2_node->inputs.erase(ir);
            }
        }
        else
        {
            if(node->name.find("/mul_1") != std::string::npos)
            {
                // disconnect the connection between add_1 mul_1
                auto ir = node->inputs.begin();

                if((*ir)->name.find("/add_1") == std::string::npos)
                    ir++;

                if((*ir)->name.find("/add_1") != std::string::npos)
                {
                    TFNode* Rsqrt_node = *ir;

                    node->inputs.erase(ir);

                    ir = Rsqrt_node->outputs.begin();

                    if((*ir)->name.find("/mul_1") == std::string::npos)
                        ir++;

                    Rsqrt_node->outputs.erase(ir);
                }
            }
            else
            {
                mul_node = true;
                // printf("name:%s\n",node->name.c_str());
            }
        }
    }

    int orig_input_size = node->inputs.size();
    std::vector<TFNode*> input_cpy = node->inputs;

    for(int i = 0; i < orig_input_size; i++)
    {
        if(mul_node && i == 0)
            continue;
        if(mul_1_node && i == 0)
            continue;

        TFNode* input_node = input_cpy[i];
        input_node->BNAddType = node->BNAddType;
        if(input_node->op == "Const")
            continue;

        BNRecursiveInputMerge(input_node);
        MergeParentNode(node, input_node);
    }
}

void TFSerializer::FuseComposedBN(TFNode* cur_node)
{
    BNRecursiveInputMerge(cur_node);
    cur_node->op = "ComposedBN";

    /* set new name */
    auto pos = cur_node->name.find("/add_1");
    cur_node->name.replace(pos, strlen("/add_1"), "bn.fused");

    /* skip to create static node for add/y */

    for(unsigned int i = 0; i < cur_node->inputs.size(); i++)
    {
        TFNode* node = cur_node->inputs[i];

        if(node->name.find("/add/y") != std::string::npos)
            node->no_static_node = true;
    }
}

bool TFSerializer::MergeChildNode(TFNode* base_node, TFNode* child_node)
{
    auto output_ir = base_node->outputs.begin();

    while(output_ir != base_node->outputs.end())
    {
        if(*output_ir == child_node)
            break;
        output_ir++;
    }

    base_node->outputs.erase(output_ir);
    base_node->outputs.insert(base_node->outputs.end(), child_node->outputs.begin(), child_node->outputs.end());

    for(auto node : child_node->outputs)
    {
        for(unsigned int i = 0; i < node->inputs.size(); i++)
        {
            if(node->inputs[i] == child_node)
            {
                node->inputs[i] = base_node;
                break;
            }
        }
    }

    auto ir = child_node->inputs.begin();

    while(ir != child_node->inputs.end())
    {
        TFNode* node = *ir;

        if(node != base_node)
        {
            base_node->inputs.push_back(node);

            for(unsigned int i = 0; i < node->outputs.size(); i++)
            {
                if(node->outputs[i] == child_node)
                {
                    node->outputs[i] = base_node;
                    break;
                }
            }
        }

        ir++;
    }

    base_node->pb_defs.insert(base_node->pb_defs.end(), child_node->pb_defs.begin(), child_node->pb_defs.end());

    // std::cout<<"base node: "<<base_node->name<<" merge child: "<<child_node->name<<"\n";

    child_node->inputs.clear();
    child_node->outputs.clear();

    return true;
}

void TFSerializer::CleanupResizeNearestNeighbor(TFGraph& tf_graph)
{
    auto ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "ResizeNearestNeighbor")
        {
            TFNode* data_node = cur_node->inputs[0];
            TFNode* data_shape_node = nullptr;

            for(unsigned int i = 0; i < data_node->outputs.size(); i++)
            {
                data_shape_node = data_node->outputs[i];

                if(data_shape_node->op == "Shape")
                    break;
            }

            DisconnectNode(data_shape_node);

            TFNode* mul_node = cur_node->inputs[1];
            TFNode* stride_slice = mul_node->inputs[0];

            DisconnectNode(stride_slice);
            DisconnectNode(mul_node);
        }

        ir++;
    }
}

void TFSerializer::MergeReluMinimum(TFGraph& tf_graph)
{
    for(auto ir = tf_graph.seq_nodes.begin(); ir != tf_graph.seq_nodes.end(); ir++)
    {
        TFNode* cur_node = *ir;

        if(cur_node->inputs.size() == 0)
            continue;

        TFNode* input0 = cur_node->inputs[0];

        if(cur_node->op == "Minimum" && input0->op == "Relu")
        {
            TFNode* const_node = cur_node->inputs[1];

            DisconnectNode(const_node);

            MergeChildNode(input0, cur_node);

            input0->op = "Relu6";
        }
    }
}

bool TFSerializer::OptimizeGraph(TFGraph& tf_graph)
{
    /* first clean up the predictions module of TF */
    auto ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "Reshape")
        {
            /* Reshape should have two inputs */

            TFNode* input_node0 = cur_node->inputs[0];
            TFNode* input_node1 = cur_node->inputs[1];

            if(input_node0->op == "Softmax" || input_node1->op == "Softmax")
            {
                DisconnectNode(cur_node);
                ir = tf_graph.seq_nodes.erase(ir);
                delete cur_node;
                continue;
            }

            TFNode* output_node = cur_node->outputs[0];

            if(output_node->op == "Softmax" || output_node->op == "MatMul")
            {
                TFNode* input_node0 = cur_node->inputs[0];
                TFNode* input_node1 = cur_node->inputs[1];
                TFNode* input_node;

                if(input_node0->op == "Const")
                {
                    DisconnectNode(input_node0);
                    input_node = input_node1;
                }
                else
                {
                    DisconnectNode(input_node1);
                    input_node = input_node0;
                }

                MergeChildNode(input_node, cur_node);

                ir = tf_graph.seq_nodes.erase(ir);
                delete cur_node;
                continue;
            }
        }

        ir++;
    }

    /* remove the squeeze node and identity */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "Squeeze")
        {
            TFNode* softmax_node = nullptr;
            TFNode* shape_node = nullptr;

            for(unsigned int j = 0; j < cur_node->outputs.size(); j++)
            {
                if(cur_node->outputs[j]->op == "Softmax")
                    softmax_node = cur_node->outputs[j];
                else if(cur_node->outputs[j]->op == "Shape")
                    shape_node = cur_node->outputs[j];
            }

            if(softmax_node)
            {
                if(shape_node)
                    DisconnectNode(shape_node);

                TFNode* input_node = cur_node->inputs[0];
                MergeChildNode(input_node, cur_node);
                ir = tf_graph.seq_nodes.erase(ir);
                delete cur_node;
                continue;
            }

            if(cur_node->outputs.size() == 1 && softmax_node == nullptr)
            {
                TFNode* child_node = cur_node->outputs[0];

                MergeParentNode(child_node, cur_node);
                ir = tf_graph.seq_nodes.erase(ir);
                delete cur_node;
                continue;
            }
        }

        if(cur_node->op == "Identity")
        {
            TFNode* input_node = cur_node->inputs[0];
            MergeChildNode(input_node, cur_node);

            ir = tf_graph.seq_nodes.erase(ir);
            delete cur_node;
            continue;
        }

        if(cur_node->op == "ConcatV2")
        {
            TFNode* axis_node = nullptr;

            for(unsigned int i = 0; i < cur_node->inputs.size(); i++)
            {
                TFNode* check_node = cur_node->inputs[i];

                if(check_node->op == "Const")
                {
                    axis_node = check_node;
                    break;
                }
            }

            if(axis_node)
            {
                cur_node->pb_defs.push_back(axis_node->pb_defs[0]);
                DisconnectNode(axis_node);
            }
        }

        ir++;
    }

    /* merge FIFOQueueV2  DequeueManyV2 */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "FIFOQueueV2")
        {
            TFNode* queue_node = cur_node->outputs[0];

            if(queue_node->op == "QueueDequeueManyV2")
            {
                MergeParentNode(queue_node, queue_node->inputs[1]);
            }

            MergeChildNode(cur_node, queue_node);

            break;
        }

        ir++;
    }

    /* remove ExpandDims */
    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "ExpandDims")
        {
            TFNode* input0 = cur_node->inputs[0];
            TFNode* input1 = cur_node->inputs[1];

            if(input0->op == "Constant" && input1->op == "Const")
            {
                TFNode* input1 = cur_node->inputs[1];
                TFNode* child_node = cur_node->outputs[0];

                DisconnectNode(input1);
                DisconnectNode(cur_node);

                child_node->inputs.push_back(input1);
                input1->outputs.push_back(child_node);
            }
            else
            {
                if(input1->op == "Const")
                    DisconnectNode(input1);
                else
                    DisconnectNode(input0);

                TFNode* child_node = cur_node->outputs[0];

                MergeParentNode(child_node, cur_node);
            }

            ir = tf_graph.seq_nodes.erase(ir);
            delete cur_node;

            continue;
        }

        ir++;
    }

    /* merge biasadd and conv */
    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "Conv2D" || cur_node->op == "DepthwiseConv2dNative" || cur_node->op == "MatMul")
        {
            TFNode* output_node = cur_node->outputs[0];

            if(output_node->op == "BiasAdd" || output_node->op == "Add")
            {
                MergeChildNode(cur_node, output_node);
            }
        }

        ir++;
    }

    /* merge composed BatchNormal */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(CheckComposedBNAdd(cur_node))
            FuseComposedBN(cur_node);
        ir++;
    }

    /* cleanup ResizeNearestNeighbor */
    CleanupResizeNearestNeighbor(tf_graph);

    /* merge Minimum and Relu */

    MergeReluMinimum(tf_graph);
    /* merge input node and reshape */
    ir = tf_graph.seq_nodes.begin();
    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;
        if(cur_node->op == "Reshape")
        {
            /* Reshape should have two inputs */
            TFNode* input_node0 = cur_node->inputs[0];
            TFNode* input_node1 = cur_node->inputs[1];

            if(input_node0->op == "Placeholder" || input_node1->op == "Placeholder")
            {
                TFNode* input_node;
                TFNode* const_node;

                if(input_node0->op == "Const")
                {
                    const_node = input_node0;
                    input_node = input_node1;
                }
                else
                {
                    const_node = input_node1;
                    input_node = input_node0;
                }

                DisconnectNode(const_node);
                MergeChildNode(input_node, cur_node);
                input_node->pb_defs.insert(input_node->pb_defs.end(), const_node->pb_defs[0]);

                ir = tf_graph.seq_nodes.erase(ir);
                delete cur_node;
                break;
            }
        }
        ir++;
    }

    /* remove the shape and StrideSlice */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;
        if(cur_node->op == "StridedSlice")
        {
            /* check if input0 is "shape" */
            TFNode* input_node = cur_node->inputs[0];

            if(input_node->op == "Shape")
            {
                /* here we go */
                DisconnectNode(cur_node);
                DisconnectNode(input_node);
                break;
            }
        }

        ir++;
    }

    /* merge pad and conv */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "Conv2D" || cur_node->op == "DepthwiseConv2dNative")
        {
            /* check if input is pad or not */
            TFNode* input_node = cur_node->inputs[0];

            if(input_node->op == "Pad")
            {
                TFNode* padding_args = input_node->inputs[1];

                input_node->pb_defs.push_back(padding_args->pb_defs[0]);

                DisconnectNode(padding_args);
                MergeParentNode(cur_node, input_node);

                /* adjust inputs, as the merged parent's node's input is inserted at last */
                int input_size = cur_node->inputs.size();

                TFNode* new_input = cur_node->inputs[input_size - 1];

                cur_node->inputs.resize(input_size - 1);
                cur_node->inputs.insert(cur_node->inputs.begin(), new_input);
            }
        }
        else if(cur_node->op == "Mean")
        {
            TFNode* indices = cur_node->inputs[1];

            DisconnectNode(indices);

            cur_node->pb_defs.push_back(indices->pb_defs[0]);
        }

        ir++;
    }

    /*remove ArgMax node */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;
        if(cur_node->op == "ArgMax")
        {
            DisconnectNode(cur_node);
            tf_graph.seq_nodes.erase(ir);

            break;
        }

        ir++;
    }

    /* remove last squeeze */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->op == "Squeeze" && cur_node->outputs.empty())
        {
            DisconnectNode(cur_node);
            break;
        }
        ir++;
    }

    /* remove no input and output nodes */

    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->inputs.size() == 0 && cur_node->outputs.size() == 0)
        {
            ir = tf_graph.seq_nodes.erase(ir);
            delete cur_node;
        }
        else
            ir++;
    }

    /* remove no input but not placeholder/const nodes */
    ir = tf_graph.seq_nodes.begin();

    while(ir != tf_graph.seq_nodes.end())
    {
        TFNode* cur_node = *ir;

        if(cur_node->inputs.size() == 0 && cur_node->op != "Const" && cur_node->op != "Placeholder" &&
           cur_node->op != "FIFOQueueV2")
        {
            DisconnectNode(cur_node);
            tf_graph.seq_nodes.erase(ir);
            delete cur_node;

            ir = tf_graph.seq_nodes.begin();    // restart
        }
        else
            ir++;
    }

    return true;
}

bool TFSerializer::GenerateStaticGraph(TFGraph& tf_graph, StaticGraph* graph)
{
    int node_number = tf_graph.seq_nodes.size();
    int i;

    bool debug_graph = false;
    const char* debug_env = std::getenv("DEBUG_G");
    if((debug_env) && (debug_env[0] == '1'))
    {
        debug_graph = true;
    }

    // first: create all tensor node
    for(i = 0; i < node_number; i++)
    {
        TFNode* tf_node = tf_graph.seq_nodes[i];

        if(debug_graph)
        {
            std::cout << i << "\t" << tf_node->op << "\t" << tf_node->name << "\n";
        }

        if(tf_node->no_static_node)
            continue;

        if(tf_node->op == "Const")
        {
            tf_serializer::LoadConstTensor(tf_node, graph);
            continue;
        }

        if(tf_node->op == "Placeholder")
        {
            tf_serializer::CreateInputNode(tf_node, graph);
            continue;
        }

        StaticNode* node = CreateStaticNode(graph, tf_node->name);

        /* create tensor */
        StaticTensor* tensor = CreateStaticTensor(graph, tf_node->name);

        SetTensorDataLayout(tensor, "NCHW");
        SetTensorDataType(tensor, DataType::GetTypeID("float32"));

        AddNodeOutputTensor(node, tensor);

        tf_node->static_node = node;
        tf_node->static_tensor = tensor;
    }

    for(i = 0; i < node_number; i++)
    {
        TFNode* tf_node = tf_graph.seq_nodes[i];

        if(tf_node->op == "Placeholder" || tf_node->op == "Const")
            continue;

        if(!FindOpLoadMethod(tf_node->op))
        {
            LOG_ERROR() << "cannot find load function for operator: " << tf_node->op << "\n";
            break;
        }

        op_load_t op_func = any_cast<op_load_t>(GetOpLoadMethod(tf_node->op));

        if(!op_func(tf_node, tf_graph, graph))
        {
            LOG_ERROR() << "error on load node: " << tf_node->name << " op: " << tf_node->op << "\n";
            break;
        }
    }

    if(i < node_number)
        return false;

    return true;
}

namespace tf_serializer {

/*************************************************/

/*
   AvgPool
   Conv2D
   DepthwiseConv2dNative
   FusedBatchNorm
   Relu6
   Softmax
 */

static bool GetAttrValue(const tensorflow::NodeDef* node, const char* key, tensorflow::AttrValue& value)
{
    const google::protobuf::Map<std::string, tensorflow::AttrValue>& attr = node->attr();

    const google::protobuf::Map<std::string, tensorflow::AttrValue>::const_iterator it = attr.find(key);
    if(it != attr.end())
    {
        value = it->second;
        return true;
    }

    return false;
}

static void CreateInputNode(TFNode* tf_node, StaticGraph* graph)
{
    StaticNode* node = CreateStaticNode(graph, tf_node->name);

    StaticTensor* tensor = CreateStaticTensor(graph, tf_node->name);

    SetTensorDataLayout(tensor, "NCHW");
    SetTensorDataType(tensor, DataType::GetTypeID("float32"));

    // if has shape, set it
    tensorflow::AttrValue shape;

    int pb_defs_cnt = tf_node->pb_defs.size();

    if(pb_defs_cnt == 1)
    {
        if(GetAttrValue(tf_node->pb_defs[0], "shape", shape))
        {
            int dim_size = shape.shape().dim_size();
            std::vector<int> dim;

            if(dim_size == 4)
            {
                dim.resize(4);
                dim[0] = shape.shape().dim(0).size();
                dim[1] = shape.shape().dim(3).size();
                dim[2] = shape.shape().dim(1).size();
                dim[3] = shape.shape().dim(2).size();
            }
            else if(dim_size == 3) /* NHC */
            {
                dim.resize(3);
                dim[0] = shape.shape().dim(0).size();
                dim[2] = shape.shape().dim(1).size();
                dim[1] = shape.shape().dim(2).size();
            }
            else if(dim_size == 2)
            {
                dim.resize(2);
                dim[0] = shape.shape().dim(0).size();
                dim[1] = shape.shape().dim(1).size();
            }
            else if(dim_size == 1)
            {
                dim.resize(1);
                dim[0] = shape.shape().dim(0).size();
            }

            SetTensorDim(tensor, dim);
        }
    }
    else
    {
        tensorflow::AttrValue value;
        const tensorflow::NodeDef* node_def = tf_node->pb_defs[pb_defs_cnt - 1];
        if(GetAttrValue(node_def, "value", value))
        {
            const tensorflow::TensorProto& tf_tensor = value.tensor();

            void* mem_ptr;
            std::vector<int> tf_dims;
            std::string layout;

            tf_serializer::GetTensorContentAndDim(tf_tensor, tf_dims, &mem_ptr, layout);

            std::vector<int> dim;

            int* reshape_dim = ( int* )mem_ptr;

            if(tf_dims[0] == 4)
            {
                dim.push_back(reshape_dim[0]);
                dim.push_back(reshape_dim[3]);
                dim.push_back(reshape_dim[1]);
                dim.push_back(reshape_dim[2]);
            }
            else
            {
                for(int i = 0; i < tf_dims[0]; i++)
                {
                    dim.push_back(reshape_dim[i]);
                }
            }
            for(unsigned int i = 0; i < dim.size(); i++)
            {
                if(dim[i] == -1)
                    dim[i] = 1;
            }
            free(mem_ptr);

            SetTensorDim(tensor, dim);
        }
    }

    AddNodeOutputTensor(node, tensor);

    StaticOp* op = CreateStaticOp(graph, "InputOp");
    SetNodeOp(node, op);

    AddGraphInputNode(graph, node);

    tf_node->static_node = node;
    tf_node->static_tensor = tensor;
}

static void GetTensorContentAndDim(const tensorflow::TensorProto& tf_tensor, std::vector<int>& dim, void** mem_ptr,
                                   std::string& layout)
{
    const tensorflow::TensorShapeProto& shape = tf_tensor.tensor_shape();

    int elem_num = 1;
    int dim_size = shape.dim_size();

    for(int i = 0; i < dim_size; i++)
    {
        elem_num *= shape.dim(i).size();
        dim.push_back(shape.dim(i).size());
    }

    void* mem_buf = nullptr;

    if(tf_tensor.tensor_content().size())
    {
        int content_size = tf_tensor.tensor_content().size();

        mem_buf = malloc(content_size);
        void* src = ( void* )tf_tensor.tensor_content().c_str();
        memcpy(mem_buf, src, content_size);
    }
    else if(tf_tensor.dtype() == tensorflow::DataType::DT_FLOAT)
    {
        // in packed format
        int data_num = tf_tensor.float_val_size();
        mem_buf = malloc(elem_num * sizeof(float));
        float* mem = ( float* )mem_buf;

        if(data_num >= elem_num)
        {
            for(int i = 0; i < elem_num; i++)
            {
                mem[i] = tf_tensor.float_val(i);
            }
        }
        else
        {
            // data_num < elem_num
            for(int i = 0; i < data_num; i++)
            {
                mem[i] = tf_tensor.float_val(i);
            }

            for(int i = data_num; i < elem_num; i++)
            {
                mem[i] = mem[data_num - 1];
            }
        }
    }
    else if(tf_tensor.dtype() == tensorflow::DataType::DT_INT32)
    {
        int data_num = tf_tensor.int_val_size();

        mem_buf = malloc(elem_num * sizeof(int));

        int* mem = ( int* )mem_buf;

        if(data_num >= elem_num)
        {
            for(int i = 0; i < elem_num; i++)
            {
                mem[i] = tf_tensor.int_val(i);
            }
        }
        else
        {
            // data_num < elem_num
            for(int i = 0; i < data_num; i++)
            {
                mem[i] = tf_tensor.int_val(i);
            }

            for(int i = data_num; i < elem_num; i++)
            {
                mem[i] = mem[data_num - 1];
            }
        }
    }

    *mem_ptr = mem_buf;

    switch(dim_size)
    {
        case 0:
            layout = "W";
            break;
        case 1:
            layout = "W";
            break;
        case 2:
            layout = "HW";
            break;
        case 4:
            layout = "NHWC";
            break;
        default:
            break;
    }
}

static void* LoadConstParam(TFNode* tf_node)
{
    tensorflow::AttrValue value;

    const tensorflow::NodeDef* node_def = tf_node->pb_defs[0];

    if(GetAttrValue(node_def, "value", value))
    {
        const tensorflow::TensorProto& tf_tensor = value.tensor();

        void* mem_ptr = nullptr;

        std::vector<int> dims;
        std::string layout;

        GetTensorContentAndDim(tf_tensor, dims, &mem_ptr, layout);

        return mem_ptr;
    }

    return nullptr;
}

static bool LoadConstTensor(TFNode* tf_node, StaticGraph* graph)
{
    StaticNode* node = CreateStaticNode(graph, tf_node->name);
    StaticTensor* tensor = CreateStaticConstTensor(graph, tf_node->name);

    SetTensorDataType(tensor, DataType::GetTypeID("float32"));

    tensorflow::AttrValue value;

    const tensorflow::NodeDef* node_def = tf_node->pb_defs[0];

    if(GetAttrValue(node_def, "value", value))
    {
        const tensorflow::TensorProto& tf_tensor = value.tensor();

        void* mem_ptr;

        std::vector<int> dims;
        std::string layout;

        GetTensorContentAndDim(tf_tensor, dims, &mem_ptr, layout);

        int mem_size = sizeof(float);

        for(unsigned int i = 0; i < dims.size(); i++)
        {
            mem_size *= dims[i];
        }

        SetTensorDim(tensor, dims);
        SetTensorSize(tensor, mem_size);
        SetTensorDataLayout(tensor, layout);
        SetConstTensorBuffer(tensor, mem_ptr);
    }

    SetConstTensorFileLocation(tensor, -1, 0);

    AddNodeOutputTensor(node, tensor);

    StaticOp* const_op = CreateStaticOp(graph, "Const");
    SetNodeOp(node, const_op);

    tf_node->static_node = node;
    tf_node->static_tensor = tensor;

    return true;
}

static bool LoadConv2D(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    /* handle inputs first */
    TFNode* input0 = tf_node->inputs[0]; /* input */
    TFNode* input1 = tf_node->inputs[1]; /* weight */

    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input0->static_tensor);
    AddNodeInputTensor(node, input1->static_tensor);

    if(tf_node->inputs.size() > 2)
    {
        TFNode* input2 = tf_node->inputs[2];
        AddNodeInputTensor(node, input2->static_tensor);
    }

    /* conv param */

    const tensorflow::NodeDef* node_def = tf_node->pb_defs[0];

    ConvParam param = any_cast<ConvParam>(OpManager::GetOpDefParam("Convolution"));

    tensorflow::AttrValue value;

    if(GetAttrValue(node_def, "dilations", value))
    {
        param.dilation_h = value.list().i(1);
        param.dilation_w = value.list().i(2);
    }

    if(GetAttrValue(node_def, "padding", value))
    {
        if(value.s() == "VALID")
        {
            param.pad_h = 0;
            param.pad_w = 0;
        }
        else if(value.s() == "SAME")
        {
            param.pad_h = -1;
            param.pad_w = -1;
        }
    }

    if(GetAttrValue(node_def, "strides", value))
    {
        param.stride_h = value.list().i(1);
        param.stride_w = value.list().i(2);
    }

    int in_channel = 1, out_channel = 1, kernel_h = 0, kernel_w = 0;
    int group = 1;
    // Tensorflow has to get those information from weights

    const tensorflow::NodeDef* weight_def = input1->pb_defs[0];

    if(GetAttrValue(weight_def, "value", value))
    {
        const tensorflow::TensorShapeProto& shape = value.tensor().tensor_shape();

        if(shape.dim_size() == 4)
        {
            kernel_h = shape.dim(0).size();
            kernel_w = shape.dim(1).size();
            in_channel = shape.dim(2).size();
            out_channel = shape.dim(3).size();
        }
        else if(shape.dim_size() == 3)
        {
            kernel_h = 1;
            kernel_w = shape.dim(0).size();
            in_channel = shape.dim(1).size();
            out_channel = shape.dim(2).size();
        }
    }

    if(tf_node->op == "DepthwiseConv2dNative")
    {
        group = in_channel;
        out_channel = in_channel * out_channel;
        in_channel = 1;
    }

    StaticTensor* weight_tensor = input1->static_tensor;

    // reset tensor's shape
    std::vector<int> dims;

    dims.push_back(out_channel);
    dims.push_back(in_channel);
    dims.push_back(kernel_h);
    dims.push_back(kernel_w);

    SetTensorDim(weight_tensor, dims);
    SetTensorDataLayout(weight_tensor, "NCHW");

    param.kernel_h = kernel_h;
    param.kernel_w = kernel_w;
    param.output_channel = out_channel;
    param.group = group;

    StaticOp* op = CreateStaticOp(graph, "Convolution");

    auto saved_param = param;

    SetOperatorParam(op, param);

    SetNodeOp(node, op);

    if(tf_node->op == "DepthwiseConv2dNative")
    {
        in_channel = group;
        out_channel = out_channel / in_channel;
    }

    int elem_size = out_channel * in_channel * kernel_h * kernel_w;
    float* new_weight = ( float* )malloc(sizeof(float) * elem_size);
    float* src = ( float* )GetConstTensorBuffer(weight_tensor);

    // in tensorflow, weight shape is [hwio]
    float* dst = new_weight;

    for(int o = 0; o < out_channel; o++)
        for(int i = 0; i < in_channel; i++)
            for(int h = 0; h < kernel_h; h++)
                for(int w = 0; w < kernel_w; w++)
                {
                    *dst++ = src[h * (kernel_w * in_channel * out_channel) + w * (in_channel * out_channel) +
                                 i * out_channel + o];
                }

    // free src and set dst

    free(src);

    SetConstTensorBuffer(weight_tensor, new_weight);

    /* special handle on merged PAD op */

    int pb_def_num = tf_node->pb_defs.size();

    if(pb_def_num > 1)
    {
        // the last one,
        const tensorflow::NodeDef* node_def = tf_node->pb_defs[pb_def_num - 1];

        /* possible pad */
        if(node_def->op() == "Const")
        {
            tensorflow::AttrValue value;
            if(GetAttrValue(node_def, "value", value) && value.has_tensor())
            {
                const tensorflow::TensorProto& tf_tensor = value.tensor();

                int dim_size = tf_tensor.tensor_shape().dim_size();

                if(dim_size == 2 && tf_tensor.tensor_shape().dim(0).size() == 4 &&
                   tf_tensor.tensor_shape().dim(1).size() == 2)
                {
                    std::vector<int> shape_data(8);

                    if(tf_tensor.tensor_content().size())
                    {
                        int* dst = shape_data.data();
                        memcpy(dst, tf_tensor.tensor_content().c_str(), tf_tensor.tensor_content().size());
                    }
                    else
                    {
                        int data_num = tf_tensor.int_val_size();

                        for(int i = 0; i < data_num; i++)
                        {
                            shape_data[i] = tf_tensor.int_val(i);
                        }
                    }

                    /* update the padding arguments */
                    saved_param.pads.resize(4);

                    /* h pad */
                    saved_param.pads[0] = shape_data[2];
                    saved_param.pads[2] = shape_data[3];
                    /* w pad */
                    saved_param.pads[1] = shape_data[4];
                    saved_param.pads[3] = shape_data[5];

                    SetOperatorParam(op, saved_param);
                }
            }
        }
    }

    return true;
}

static bool LoadPool(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input = tf_node->inputs[0];
    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input->static_tensor);

    PoolParam param = any_cast<PoolParam>(OpManager::GetOpDefParam("Pooling"));

    const tensorflow::NodeDef* node_def = tf_node->pb_defs[0];
    tensorflow::AttrValue value;

    if(GetAttrValue(node_def, "ksize", value))
    {
        param.kernel_h = value.list().i(1);
        param.kernel_w = value.list().i(2);
    }

    if(GetAttrValue(node_def, "strides", value))
    {
        param.stride_h = value.list().i(1);
        param.stride_w = value.list().i(2);
    }

    if(GetAttrValue(node_def, "padding", value))
    {
        if(value.s() == "VALID")
        {
            param.pad_h = 0;
            param.pad_w = 0;
        }
        else if(value.s() == "SAME")
        {
            param.pad_h = -1;
            param.pad_w = -1;
        }
    }

    if(tf_node->op == "AvgPool")
    {
        param.alg = kPoolAvg;
    }
    else if(tf_node->op == "MaxPool")
    {
        param.alg = kPoolMax;
    }

    // convert to onnx format
    param.kernel_shape.resize(2);
    param.kernel_shape[0] = param.kernel_h;
    param.kernel_shape[1] = param.kernel_w;

    param.pads.resize(4);
    param.pads[0] = param.pad_h;
    param.pads[1] = param.pad_w;
    param.pads[2] = param.pad_h;
    param.pads[3] = param.pad_w;

    param.strides.resize(2);
    param.strides[0] = param.stride_h;
    param.strides[1] = param.stride_w;

    StaticOp* op = CreateStaticOp(graph, "Pooling");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    return true;
}

static bool LoadBatchNorm(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input0 = tf_node->inputs[0];
    TFNode* gamma = tf_node->inputs[1];
    TFNode* beta = tf_node->inputs[2];
    TFNode* mean = tf_node->inputs[3];
    TFNode* var = tf_node->inputs[4];

    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input0->static_tensor);
    AddNodeInputTensor(node, gamma->static_tensor);
    AddNodeInputTensor(node, beta->static_tensor);
    AddNodeInputTensor(node, mean->static_tensor);
    AddNodeInputTensor(node, var->static_tensor);

    BatchNormParam param = any_cast<BatchNormParam>(OpManager::GetOpDefParam("BatchNormalization"));

    const tensorflow::NodeDef* node_def = tf_node->pb_defs[0];
    tensorflow::AttrValue value;

    if(GetAttrValue(node_def, "epsilon", value))
    {
        param.eps = value.f();
    }

    StaticOp* op = CreateStaticOp(graph, "BatchNormalization");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    return true;
}

static bool LoadSoftmax(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input = tf_node->inputs[0];
    StaticNode* node = tf_node->static_node;
    AddNodeInputTensor(node, input->static_tensor);

    SoftmaxParam param = any_cast<SoftmaxParam>(OpManager::GetOpDefParam("Softmax"));
    /* it seems tensorflow justs support last dimension */

    StaticOp* op = CreateStaticOp(graph, "Softmax");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    AddGraphOutputNode(graph, node);

    return true;
}

static bool LoadRelu(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input = tf_node->inputs[0];
    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input->static_tensor);

    ReLuParam param = any_cast<ReLuParam>(OpManager::GetOpDefParam("ReLu"));
    param.negative_slope = 0.f;

    StaticOp* op = CreateStaticOp(graph, "ReLu");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    return true;
}

static bool LoadResize(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input = tf_node->inputs[0];
    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input->static_tensor);

    ResizeParam param = any_cast<ResizeParam>(OpManager::GetOpDefParam("Resize"));
    param.scale_h = 2;
    param.scale_w = 2;
    param.type = 0;
    StaticOp* op = CreateStaticOp(graph, "Resize");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    return true;
}

static bool LoadRelu6(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input = tf_node->inputs[0];
    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input->static_tensor);

    StaticOp* op = CreateStaticOp(graph, "ReLu6");
    SetNodeOp(node, op);

    return true;
}

static int nhwc_axis_swap[] = {0, 2, 3, 1};

static bool LoadConcat(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input;
    StaticNode* node = tf_node->static_node;

    for(unsigned int i = 0; i < tf_node->inputs.size(); i++)
    {
        input = tf_node->inputs[i];
        AddNodeInputTensor(node, input->static_tensor);
    }

    ConcatParam param = any_cast<ConcatParam>(OpManager::GetOpDefParam("Concat"));

    const tensorflow::NodeDef* node_def = tf_node->pb_defs[1];
    tensorflow::AttrValue value;

    if(GetAttrValue(node_def, "value", value))
    {
        const tensorflow::TensorProto& tf_tensor = value.tensor();

        int axis = tf_tensor.int_val(0);

        // TF is NHWC, TEngine is NCHW
        param.axis = nhwc_axis_swap[axis];
    }

    StaticOp* op = CreateStaticOp(graph, "Concat");
    SetOperatorParam(op, param);

    SetNodeOp(node, op);

    return true;
}

static EltType MapEltwise(TFNode* tf_node, const std::string& elt_op)
{
    if(elt_op == "Add" || elt_op == "AddN")
        return ELT_SUM;
    else if(elt_op == "Mul")
        return ELT_PROD;
    else if(elt_op == "Sub")
        return ELT_SUB;
    else if(elt_op == "Rsqrt")
        return ELT_RSQRT;
    else if(elt_op == "Minimum")
        return ELT_MIN_SCALAR;
    else
        return ELT_LAST;
}

static bool LoadEltwise(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    // sanity check
    if(tf_node->op == "Add" || tf_node->op == "Mul" || tf_node->op == "Sub" || tf_node->op == "Minimum" ||
       tf_node->op == "AddN")
    {
        if(tf_node->inputs.size() != 2)
            return false;
    }
    else if(tf_node->op == "Rsqrt")
    {
        if(tf_node->inputs.size() != 1)
            return false;
    }
    else
    {
        XLOG_ERROR() << "Unsupported op: " << tf_node->op << "\n";
        return false;
    }

    StaticNode* node = tf_node->static_node;

    for(unsigned int i = 0; i < tf_node->inputs.size(); i++)
    {
        AddNodeInputTensor(node, tf_node->inputs[i]->static_tensor);
    }

    StaticOp* op = CreateStaticOp(graph, "Eltwise");

    EltwiseParam param = any_cast<EltwiseParam>(OpManager::GetOpDefParam("Eltwise"));

    param.type = MapEltwise(tf_node, tf_node->op);

    SetOperatorParam(op, param);

    SetNodeOp(node, op);

    return true;
}

static void CreatePresetNode(StaticGraph* graph, StaticNode* node, const char* name, const char* layout,
                             std::vector<int>& dims, float val)
{
    std::string new_tensor_name = node->name;
    auto pos = new_tensor_name.find("bn.fused");
    new_tensor_name = new_tensor_name.replace(pos, strlen("bn.fused"), name);
    StaticTensor* tensor = CreateStaticConstTensor(graph, new_tensor_name);
    SetTensorDim(tensor, dims);
    SetTensorDataType(tensor, DataType::GetTypeID("float32"));
    SetTensorDataLayout(tensor, layout);

    int elem_size = 1;

    for(unsigned int i = 0; i < dims.size(); i++)
    {
        elem_size *= dims[i];
    }

    SetTensorSize(tensor, elem_size * sizeof(float));

    float* ptr = ( float* )std::malloc(elem_size * sizeof(float));

    for(int i = 0; i < elem_size; i++)
        ptr[i] = val;

    SetConstTensorBuffer(tensor, ptr);
    SetConstTensorFileLocation(tensor, -1, 0);

    StaticNode* new_node = CreateStaticNode(graph, new_tensor_name);

    StaticOp* const_op = CreateStaticOp(graph, "Const");

    SetNodeOp(new_node, const_op);

    AddNodeOutputTensor(new_node, tensor);

    AddNodeInputTensor(node, tensor);
}

static bool LoadComposedBN(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    int i = 0;
    TFNode* input0 = tf_node->inputs[i++];
    StaticNode* node = tf_node->static_node;
    AddNodeInputTensor(node, input0->static_tensor);
    // add gamma node
    if(tf_node->BNAddType == 1)
    {
        TFNode* gamma = tf_node->inputs[i++];
        AddNodeInputTensor(node, gamma->static_tensor);
    }
    else
    {
        std::vector<int> dims = tf_node->inputs[i]->static_tensor->dims;    // dims of var
        CreatePresetNode(graph, node, "gamma", "W", dims, 1.0f);
    }
    TFNode* var = tf_node->inputs[i++];
    TFNode* add_y = tf_node->inputs[i++];
    TFNode* beta = tf_node->inputs[i++];
    TFNode* mean = tf_node->inputs[i++];
    AddNodeInputTensor(node, beta->static_tensor);
    AddNodeInputTensor(node, mean->static_tensor);
    AddNodeInputTensor(node, var->static_tensor);

    BatchNormParam param = any_cast<BatchNormParam>(OpManager::GetOpDefParam("BatchNormalization"));

    /* add_y is epison in deed */

    float* eps_ptr = ( float* )LoadConstParam(add_y);

    param.eps = eps_ptr[0];

    free(eps_ptr);

    // printf("eps=%.20f\n",param.eps);

    StaticOp* op = CreateStaticOp(graph, "BatchNormalization");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    return true;
}

static bool LoadMean(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    int use_pool = false;

    /* use global average pool, if reduce axis is h/w and keep_dims */
    const tensorflow::NodeDef* node_def = tf_node->pb_defs[1];
    tensorflow::AttrValue value;

    if(GetAttrValue(node_def, "value", value))
    {
        if(value.has_tensor())
        {
            const tensorflow::TensorProto& tf_tensor = value.tensor();
            // const tensorflow::TensorShapeProto& shape = tf_tensor.tensor_shape();

            std::vector<int> axis(2);

            if(tf_tensor.tensor_content().size())
            {
                int* mem = axis.data();
                void* src = ( void* )tf_tensor.tensor_content().c_str();
                memcpy(mem, src, tf_tensor.tensor_content().size());
            }
            else if(tf_tensor.dtype() == tensorflow::DataType::DT_INT32)
            {
                for(int i = 0; i < 2; i++)
                    axis[i] = tf_tensor.int_val(i);
            }

            if(axis[0] == 1 && axis[1] == 2)
                use_pool = true;
        }
    }

    if(!use_pool)
    {
        XLOG_ERROR() << "failed to load Mean who is not pool\n";
        return false;
    }

    TFNode* input = tf_node->inputs[0];
    StaticNode* node = tf_node->static_node;
    AddNodeInputTensor(node, input->static_tensor);

    PoolParam param = any_cast<PoolParam>(OpManager::GetOpDefParam("Pooling"));
    param.alg = kPoolAvg;
    param.global = 1;

    StaticOp* op = CreateStaticOp(graph, "Pooling");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    return true;
}

static bool LoadFIFOQueue(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    StaticNode* node = tf_node->static_node;

    /* get shape */
    const tensorflow::NodeDef* node_def = tf_node->pb_defs[0];

    tensorflow::AttrValue value;

    if(GetAttrValue(node_def, "shapes", value))
    {
        if(value.has_list())
        {
            const tensorflow::TensorShapeProto& shapes = value.list().shape(0);
            int dim_size = shapes.dim_size();

            std::vector<int> dims(dim_size);

            for(int i = 0; i < dim_size; i++)
            {
                dims[i] = shapes.dim(i).size();
            }

            SetTensorDim(tf_node->static_tensor, dims);
        }
    }

    StaticOp* op = CreateStaticOp(graph, "InputOp");
    SetNodeOp(node, op);

    AddGraphInputNode(graph, node);

    return true;
}

static bool LoadReshape(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input0 = tf_node->inputs[0];
    TFNode* input1 = tf_node->inputs[1];

    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input0->static_tensor);
    AddNodeInputTensor(node, input1->static_tensor);

    ReshapeParam param = any_cast<ReshapeParam>(OpManager::GetOpDefParam("Reshape"));
    int* dims = ( int* )LoadConstParam(input1);
    std::vector<int> out_shape;
    if(input1->static_tensor->dims[0] == 4)
    {
        param.dim_0 = dims[0];
        out_shape.push_back(dims[0]);
        param.dim_1 = dims[3];
        out_shape.push_back(dims[3]);
        param.dim_2 = dims[1];
        out_shape.push_back(dims[1]);
        param.dim_3 = dims[2];
        out_shape.push_back(dims[2]);
    }
    else if(input1->static_tensor->dims[0] == 3)
    {
        param.dim_0 = dims[0];
        param.dim_1 = dims[1];
        param.dim_2 = dims[2];
        out_shape.push_back(dims[0]);
        out_shape.push_back(dims[1]);
        out_shape.push_back(dims[2]);
    }
    else if(input1->static_tensor->dims[0] == 2)
    {
        param.dim_0 = dims[0];
        param.dim_1 = dims[1];
        out_shape.push_back(dims[0]);
        out_shape.push_back(dims[1]);
    }
    else
    {
        return false;
    }

    SetTensorDim(tf_node->static_tensor, out_shape);

    free(dims);

    StaticOp* op = CreateStaticOp(graph, "Reshape");
    SetOperatorParam(op, param);
    SetNodeOp(node, op);

    return true;
}

static bool LoadGemm(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    TFNode* input0 = tf_node->inputs[0];
    TFNode* input1 = tf_node->inputs[1];

    if(input0->op == "Const")
        std::swap(input0, input1);

    StaticNode* node = tf_node->static_node;

    AddNodeInputTensor(node, input0->static_tensor);
    AddNodeInputTensor(node, input1->static_tensor);

    GemmParam param = any_cast<GemmParam>(OpManager::GetOpDefParam("Gemm"));

    const tensorflow::NodeDef* node_def = tf_node->pb_defs[0];
    tensorflow::AttrValue value;
    if(GetAttrValue(node_def, "transpose_a", value))
    {
        param.transA = value.b();
    }
    if(GetAttrValue(node_def, "transpose_b", value))
    {
        param.transB = value.b();
    }
    param.alpha = 1;
    param.beta = 1;

    StaticTensor* weight_tensor = FindTensor(graph, input1->name);
    SetTensorDataLayout(weight_tensor, "HW");

    if(tf_node->inputs.size() > 2)
    {
        TFNode* bias = tf_node->inputs[2];
        AddNodeInputTensor(node, bias->static_tensor);
        StaticTensor* bias_tensor = FindTensor(graph, bias->name);
        SetTensorDataLayout(bias_tensor, "W");
    }

    if(param.transA)
    {
        StaticOp* op = CreateStaticOp(graph, "Gemm");

        SetOperatorParam(op, param);

        SetNodeOp(node, op);

        return true;
    }

    // create fc instead
    if(!param.transB)
    {
        // swap shape
        int k = weight_tensor->dims[0];
        int n = weight_tensor->dims[1];

        weight_tensor->dims[0] = n;
        weight_tensor->dims[1] = k;

        float* tmp = ( float* )malloc(k * n * sizeof(float));
        float* data = ( float* )GetConstTensorBuffer(weight_tensor);

        for(int i = 0; i < n; i++)
            for(int j = 0; j < k; j++)
            {
                tmp[i * k + j] = data[j * n + i];
            }

        memcpy(data, tmp, n * k * sizeof(float));

        free(tmp);
    }

    StaticOp* op = CreateStaticOp(graph, "FullyConnected");

    FCParam fc_param = any_cast<FCParam>(OpManager::GetOpDefParam("FullyConnected"));

    fc_param.num_output = weight_tensor->dims[0];

    SetOperatorParam(op, fc_param);

    SetNodeOp(node, op);

    return true;
}

static bool LoadGeneric(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    StaticNode* node = tf_node->static_node;

    StaticOp* op = CreateStaticOp(graph, "Generic");

    GenericParam generic_param = any_cast<GenericParam>(OpManager::GetOpDefParam("Generic"));

    /* a little bit ugly, any better solution? */
    std::string* saved_name = new std::string(tf_node->op);
    std::shared_ptr<std::string> for_free(saved_name);
    AddOperatorAttr(op, "For_Free_Generic_OP", for_free);

    generic_param.op_name = saved_name->c_str();
    generic_param.max_input_num = tf_node->inputs.size();
    generic_param.max_output_num = tf_node->outputs.size();

    SetOperatorParam(op, generic_param);

    SetNodeOp(node, op);

    for(unsigned int i = 0; i < tf_node->inputs.size(); i++)
    {
        TFNode* input = tf_node->inputs[i];
        AddNodeInputTensor(node, input->static_tensor);
    }

    return true;
}

static bool LoadLSTMInitState(LSTMNode* lstm_node, TFNode* init_node, StaticGraph* graph)
{
    /* load const value */
    TFNode* const_val_node;
    TFNode* concat_node;

    if(init_node->inputs[0]->op == "Const")
    {
        const_val_node = init_node->inputs[0];
        concat_node = init_node->inputs[1];
    }
    else
    {
        const_val_node = init_node->inputs[1];
        concat_node = init_node->inputs[0];
    }

    int* const_ptr = ( int* )LoadConstParam(const_val_node);
    float const_val = const_ptr[0];

    free(const_ptr);

    int* dim0_ptr = ( int* )LoadConstParam(concat_node->inputs[0]);
    int* dim1_ptr = ( int* )LoadConstParam(concat_node->inputs[1]);

    std::vector<int> dims(2);

    dims[0] = dim0_ptr[0];
    dims[1] = dim1_ptr[0];

    free(dim0_ptr);
    free(dim1_ptr);

    float* mem_ptr = ( float* )malloc(dims[0] * dims[1] * sizeof(float));

    for(int i = 0; i < dims[0] * dims[1]; i++)
    {
        mem_ptr[i] = const_val;
    }

    /* create node and tensor */

    std::string const_node_name;

    if(init_node == lstm_node->init_c)
        const_node_name = lstm_node->name + "/init_c";
    else
        const_node_name = lstm_node->name + "/init_h";

    StaticNode* const_node = CreateStaticNode(graph, const_node_name);
    StaticTensor* const_tensor = CreateStaticConstTensor(graph, const_node_name);

    SetTensorDataType(const_tensor, DataType::GetTypeID("float32"));
    SetTensorDim(const_tensor, dims);
    SetTensorSize(const_tensor, dims[0] * dims[1] * sizeof(float));
    SetTensorDataLayout(const_tensor, "W");
    SetConstTensorBuffer(const_tensor, mem_ptr);
    SetConstTensorFileLocation(const_tensor, -1, 0);

    AddNodeOutputTensor(const_node, const_tensor);

    StaticOp* const_op = CreateStaticOp(graph, "Const");
    SetNodeOp(const_node, const_op);

    AddNodeInputTensor(lstm_node->static_node, const_tensor);

    return true;
}

static bool LoadLSTM(TFNode* tf_node, TFGraph& tf_graph, StaticGraph* graph)
{
    StaticNode* node = tf_node->static_node;

    LSTMNode* lstm_node = dynamic_cast<LSTMNode*>(tf_node);
    LSTMParam param = any_cast<LSTMParam>(OpManager::GetOpDefParam("LSTM"));

    // those two are mandatory
    AddNodeInputTensor(node, tf_node->inputs[0]->static_tensor);
    AddNodeInputTensor(node, lstm_node->kernel->static_tensor);

    // optional tensors
    if(lstm_node->bias)
    {
        param.has_bias = 1;
        AddNodeInputTensor(node, lstm_node->bias->static_tensor);
    }

    if(lstm_node->w_f_diag)
    {
        param.has_peephole = 1;
        AddNodeInputTensor(node, lstm_node->w_f_diag->static_tensor);
    }

    if(lstm_node->w_i_diag)
        AddNodeInputTensor(node, lstm_node->w_i_diag->static_tensor);

    if(lstm_node->w_o_diag)
        AddNodeInputTensor(node, lstm_node->w_o_diag->static_tensor);

    if(lstm_node->projection)
    {
        param.has_projection = 1;
        AddNodeInputTensor(node, lstm_node->projection->static_tensor);
    }

    if(lstm_node->init_h)
    {
        param.has_init_state = 1;
        LoadLSTMInitState(lstm_node, lstm_node->init_c, graph);
        LoadLSTMInitState(lstm_node, lstm_node->init_h, graph);
    }

    /* forget bias */
    if(lstm_node->forget_bias)
    {
        float* f_ptr = ( float* )LoadConstParam(lstm_node->forget_bias);
        param.forget_bias = f_ptr[0];
        free(f_ptr);
    }
    else
    {
        /* tensorflow defaults is 1.0 */
        param.forget_bias = 1.0;
    }

    /* calculate and set other paremeters*/
    const std::vector<int>& kernel_dims = GetTensorDim(lstm_node->kernel->static_tensor);

    int data_size = kernel_dims[0];
    int cell_size = kernel_dims[1] / 4;

    param.cell_size = cell_size;

    if(lstm_node->projection)
    {
        const std::vector<int>& proj_dims = GetTensorDim(lstm_node->projection->static_tensor);
        param.hidden_size = proj_dims[1];
    }
    else
    {
        param.hidden_size = param.cell_size;
    }

    param.input_size = data_size - param.hidden_size;

    StaticOp* op = CreateStaticOp(graph, "LSTM");
    SetOperatorParam(op, param);

    SetNodeOp(node, op);

    return true;
}

}    // namespace tf_serializer

using namespace tf_serializer;

bool TFSerializerRegisterOpLoader(void)
{
    SerializerPtr serializer;

    if(!SerializerManager::SafeGet("tensorflow", serializer))
        return false;

    TFSerializer* p_tf = dynamic_cast<TFSerializer*>(serializer.get());

    p_tf->RegisterOpLoadMethod("AvgPool", op_load_t(LoadPool));
    p_tf->RegisterOpLoadMethod("MaxPool", op_load_t(LoadPool));
    p_tf->RegisterOpLoadMethod("Conv2D", op_load_t(LoadConv2D));
    p_tf->RegisterOpLoadMethod("DepthwiseConv2dNative", op_load_t(LoadConv2D));
    p_tf->RegisterOpLoadMethod("FusedBatchNorm", op_load_t(LoadBatchNorm));
    p_tf->RegisterOpLoadMethod("Relu6", op_load_t(LoadRelu6));
    p_tf->RegisterOpLoadMethod("Relu", op_load_t(LoadRelu));
    p_tf->RegisterOpLoadMethod("Softmax", op_load_t(LoadSoftmax));
    p_tf->RegisterOpLoadMethod("ConcatV2", op_load_t(LoadConcat));
    p_tf->RegisterOpLoadMethod("Add", op_load_t(LoadEltwise));
    p_tf->RegisterOpLoadMethod("Sub", op_load_t(LoadEltwise));
    p_tf->RegisterOpLoadMethod("Mul", op_load_t(LoadEltwise));
    p_tf->RegisterOpLoadMethod("Minimum", op_load_t(LoadEltwise));
    p_tf->RegisterOpLoadMethod("Rsqrt", op_load_t(LoadEltwise));
    p_tf->RegisterOpLoadMethod("ResizeNearestNeighbor", op_load_t(LoadResize));
    p_tf->RegisterOpLoadMethod("ComposedBN", op_load_t(LoadComposedBN));
    p_tf->RegisterOpLoadMethod("Reshape", op_load_t(LoadReshape));
    p_tf->RegisterOpLoadMethod("MatMul", op_load_t(LoadGemm));
    p_tf->RegisterOpLoadMethod("AddN", op_load_t(LoadEltwise));
    p_tf->RegisterOpLoadMethod("FIFOQueueV2", op_load_t(LoadFIFOQueue));
    p_tf->RegisterOpLoadMethod("Mean", op_load_t(LoadMean));
    p_tf->RegisterOpLoadMethod("DecodeWav", op_load_t(LoadGeneric));
    p_tf->RegisterOpLoadMethod("AudioSpectrogram", op_load_t(LoadGeneric));
    p_tf->RegisterOpLoadMethod("Mfcc", op_load_t(LoadGeneric));
    p_tf->RegisterOpLoadMethod("LSTM", op_load_t(LoadLSTM));

    return true;
}

void test_tfserializer(void)
{
    std::vector<std::string> file_list;

    const char* model_fname =
        "/home/haitao/workshop/Tengine_models/mobilenet/tensorflow/frozen_mobilenet_v1_224.prototxt";
    // const char * model_fname="/home/haitao/workshop/Tengine_models/mobilenet/tensorflow/frozen_mobilenet_v1_224.pb";
    // const char *
    // model_fname="/home/haitao/github/tensorflow/tensorflow/examples/label_image/data/inception_v3_2016_08_28_frozen.pb";

    file_list.push_back(model_fname);

    /* test */

    SerializerPtr p_tf;

    SerializerManager::SafeGet("tensorflow", p_tf);
    StaticGraph* graph = CreateStaticGraph("test");

    if(!p_tf->LoadModel(file_list, graph))
    {
        LOG_ERROR() << "Load model failed\n";
        return;
    }

    LOG_INFO() << "Load model successfully\n";

    DumpStaticGraph(graph);

    if(CheckGraphIntegraity(graph))
        LOG_INFO() << "check passed\n";
}

}    // namespace TEngine
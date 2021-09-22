// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/tensorprotoutils.h"
#include "core/graph/graph_utils.h"
#include "core/optimizer/initializer.h"
#include "core/optimizer/matmul_add_fusion.h"
#include "onnx/defs/attr_proto_util.h"
#include <deque>

using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::common;
namespace onnxruntime {

Status MatMulAddFusion::ApplyImpl(Graph& graph, bool& modified, int graph_level, const logging::Logger& logger) const {
  GraphViewer graph_viewer(graph);
  const auto& node_topology_list = graph_viewer.GetNodesInTopologicalOrder();

  for (auto node_index : node_topology_list) {
    auto* node_ptr = graph.GetNode(node_index);
    if (!node_ptr)
      continue;  // node was removed

    auto& node = *node_ptr;

    ORT_RETURN_IF_ERROR(Recurse(node, modified, graph_level, logger));


    bool is_supported_matmul = graph_utils::IsSupportedOptypeVersionAndDomain(
        node, "MatMul", {1, 9, 13});
    is_supported_matmul =
        is_supported_matmul || graph_utils::IsSupportedOptypeVersionAndDomain(
                                   node, "FusedMatMul", {1}, kMSDomain);
    if (!is_supported_matmul ||
        !graph_utils::IsSupportedProvider(node,
                                          GetCompatibleExecutionProviders()) ||
        node.GetOutputEdgesCount() != 1) {
      continue;
    }
    // std::cout << "pengwa ckpt 55555555555555555555555555" << std::endl;
    if (graph.NodeProducesGraphOutput(node)) {
      continue;
    }

    auto next_node_itr = node.OutputNodesBegin();
    if (next_node_itr == node.OutputNodesEnd()) {
      continue;
    }

    const Node& next_node = (*next_node_itr);
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(next_node, "Add", {7, 13, 14}) ||
        next_node.GetExecutionProviderType() != node.GetExecutionProviderType()) {
      continue;
    }

    Node& matmul_node = node;
    Node& add_node = const_cast<Node&>(next_node);
    std::vector<NodeArg> input_args;
    std::vector<NodeArg> output_args;
    auto matmul_input_defs = matmul_node.MutableInputDefs();
    auto add_input_defs = add_node.MutableInputDefs();

    // Gemm requires that inputs be the same data type and both floating point (float32/float16).
    auto matmul_type = matmul_input_defs[0]->Type();
    auto add_type = add_input_defs[0]->Type();
    if ((*matmul_type) != (*add_type)) {
      continue;
    }
    if ((*matmul_type) != "tensor(float)" && (*matmul_type) != "tensor(float16)" && (*matmul_type) != "tensor(bfloat16)") {
      continue;
    }

    // Gemm only support Matrix, need to check the shape of MatMul and Add
    auto matmul_a_shape = matmul_input_defs[0]->Shape();
    auto matmul_b_shape = matmul_input_defs[1]->Shape();
    if (nullptr == matmul_a_shape || nullptr == matmul_b_shape) {
      continue;
    }

    bool is_gemm = false;
    if (2 == matmul_a_shape->dim_size() && 2 == matmul_b_shape->dim_size()) {
      // Gemm only support Matrix
      is_gemm = true;
    } else if (matmul_node.InputDefs().size() > 2) {
      // skip if FusedMatMul has 3 inputs already.
      continue;
    }

    const auto& matmul_output = *matmul_node.OutputDefs()[0];

    auto matmul_output_name = matmul_output.Name();
    auto gemm_input_defs = matmul_input_defs;
    if (matmul_output_name == add_input_defs[0]->Name()) {
      // matmul output as Add_A, should use Add_B as input C for gemm
      gemm_input_defs.push_back(add_input_defs[1]);
    } else {
      // matmul output as Add_B, should use Add_A as input C for gemm
      gemm_input_defs.push_back(add_input_defs[0]);
    }

    if (!gemm_input_defs.back()->Shape()) {
      continue;
    }
    const auto &bias_shape = *gemm_input_defs.back()->Shape();
    if (is_gemm) {
      const auto &M = matmul_output.Shape()->dim()[0];
      const auto &N = matmul_output.Shape()->dim()[1];
      auto dim_has_value_1 = [](const TensorShapeProto_Dimension &dim) {
        return dim.has_dim_value() && dim.dim_value() == 1;
      };

      // valid bias_shapes are (N) or (1, N) or (M, 1) or (M, N) as
      // GEMM only supports unidirectional broadcast on the bias input C
      bool valid =
          ((bias_shape.dim_size() == 1 && bias_shape.dim()[0] == N) ||
           (bias_shape.dim_size() == 2 &&
            dim_has_value_1(bias_shape.dim()[0]) && bias_shape.dim()[1] == N) ||
           (bias_shape.dim_size() == 2 && bias_shape.dim()[0] == M &&
            (dim_has_value_1(bias_shape.dim()[1]) ||
             bias_shape.dim()[1] == N)));
      if (!valid) {
        continue;
      }
      Node &gemm_node = graph.AddNode(
          graph.GenerateNodeName("gemm"), "Gemm",
          "fused Matmul and Add " + add_node.OpType(), gemm_input_defs, {});

      // Assign provider to this new node. Provider should be same as the
      // provider for old node.
      gemm_node.SetExecutionProviderType(
          matmul_node.GetExecutionProviderType());

      // move output definitions and edges from act_node to gemm_node. delete
      // gemm_node and act_node.
      graph_utils::FinalizeNodeFusion(graph, {matmul_node, add_node},
                                      gemm_node);

      modified = true;
    } else {
      auto output_len = matmul_output.Shape()->dim_size();
      const auto &M = matmul_output.Shape()->dim()[output_len - 2];
      const auto &N = matmul_output.Shape()->dim()[output_len - 1];
      auto dim_has_value_1 = [](const TensorShapeProto_Dimension &dim) {
        return dim.has_dim_value() && dim.dim_value() == 1;
      };

      // valid bias_shapes are (N) or (1, N) or (M, 1) or (M, N) as
      // GEMM only supports unidirectional broadcast on the bias input C
      bool valid =
          ((bias_shape.dim_size() == 1 && bias_shape.dim()[0] == N) ||
           (bias_shape.dim_size() == 2 &&
            dim_has_value_1(bias_shape.dim()[0]) && bias_shape.dim()[1] == N) ||
           (bias_shape.dim_size() == 2 && bias_shape.dim()[0] == M &&
            (dim_has_value_1(bias_shape.dim()[1]) ||
             bias_shape.dim()[1] == N)));
      if (!valid) {
        continue;
      }

      auto alpha_attr = ONNX_NAMESPACE::MakeAttribute("beta", float(1.0f));
      NodeAttributes attributes;
      attributes.reserve(1);
      attributes[alpha_attr.name()] = alpha_attr;

      Node &gemm_node = graph.AddNode(
          graph.GenerateNodeName("FusedMatMulBias"), "FusedMatMul",
          "fused FusedMatMul and Add " + add_node.OpType(), gemm_input_defs, {},
          &attributes, kMSDomain);

      // Assign provider to this new node. Provider should be same as the
      // provider for old node.
      gemm_node.SetExecutionProviderType(
          matmul_node.GetExecutionProviderType());

      // move output definitions and edges from act_node to gemm_node. delete
      // gemm_node and act_node.
      graph_utils::FinalizeNodeFusion(graph, {matmul_node, add_node},
                                      gemm_node);
      modified = true;
    }
  }

  return Status::OK();
}
}  // namespace onnxruntime

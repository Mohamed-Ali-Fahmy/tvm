
/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <tvm/ir/transform.h>
#include <tvm/relay/attrs/nn.h>
#include <tvm/relay/expr_functor.h>
#include <tvm/relay/transform.h>
#include <tvm/tir/builtin.h>
#include <tvm/tir/expr.h>
#include <tvm/tir/function.h>
#include <tvm/tir/op.h>
#include <tvm/tir/stmt_functor.h>

#include "../../../qnn/utils.h"
#include "../../../transforms/pattern_utils.h"

namespace tvm {
namespace relay {
namespace contrib {
namespace cmsisnn {

class RelayToTIRVisitor : public MixedModeMutator {
 public:
  explicit RelayToTIRVisitor(IRModule ir_module, Target target)
      : ir_module_(ir_module), target_(target) {
    context_buffer_id_ = 0;
  }

  IRModule Mutate() {
    GlobalVar main_global_var = ir_module_->GetGlobalVar("main");
    BaseFunc main = ir_module_->Lookup(main_global_var);
    Function main_func = GetRef<Function>(main.as<FunctionNode>());

    // Copy everything across and mutate the body
    Function mutated_main =
        Function(main_func->params, VisitExpr(main_func->body), main_func->ret_type,
                 main_func->type_params, main_func->attrs, main_func->span);

    ir_module_->Update(main_global_var, mutated_main);

    return ir_module_;
  }

 private:
  inline IntImm ToArg(int32_t value) { return IntImm(DataType::Int(32), value); }

  void CreatePrimFuncForExtern(const GlobalVar& global_var, Array<tir::Var> func_signature,
                               tvm::Array<PrimExpr> call_extern_args,
                               std::string context_buffer_name = "NULL",
                               int context_buffer_size = 0) {
    Map<String, ObjectRef> dict_attrs;
    dict_attrs.Set(tvm::attr::kGlobalSymbol, global_var->name_hint);
    dict_attrs.Set(tvm::attr::kTarget, target_);
    dict_attrs.Set("tir.noalias", Bool(true));

    tir::Stmt body = tir::Evaluate(
        tvm::tir::Call(DataType::Int(8), tir::builtin::call_extern(), call_extern_args));

    if (context_buffer_size) {
      tir::Var buffer_var(context_buffer_name,
                          PointerType(PrimType(DataType::Int(8)), "global.workspace"));
      body = tir::Allocate(buffer_var, DataType::Int(8), {context_buffer_size}, tir::const_true(),
                           body);
      body =
          tir::AttrStmt(PrimExpr(), tvm::tir::attr::device_type, target_->kind->device_type, body);
      body = tir::AttrStmt(PrimExpr(), tvm::tir::attr::device_id, 0, body);
    }

    tir::PrimFunc replacement_func(func_signature, body, VoidType(), Map<tir::Var, tir::Buffer>(),
                                   DictAttrs(dict_attrs));

    ir_module_->Add(global_var, replacement_func);
  }

  Array<PrimExpr> CMSISNNDimensions(const Array<PrimExpr>& shape) {
    ICHECK(shape.size() == 4) << "Supports only CMSIS-NN shapes of dimension 4.";
    return Array<PrimExpr>{ToArg(qnn::get_const_int(shape[0])), ToArg(qnn::get_const_int(shape[1])),
                           ToArg(qnn::get_const_int(shape[2])),
                           ToArg(qnn::get_const_int(shape[3]))};
  }

  void EmitConv2D(const GlobalVar& global_var, const Expr& expr) {
    const CallNode* clip_call = nullptr;
    const CallNode* requantize_call = nullptr;
    const CallNode* bias_add_call = nullptr;
    const CallNode* conv2d_call = nullptr;
    const CallNode* final_call = expr.as<CallNode>();
    const OpNode* final_op = final_call->op.as<OpNode>();
    if (final_op->name == "clip") {
      clip_call = final_call;
      requantize_call = clip_call->args[0].as<CallNode>();
    } else {
      requantize_call = final_call;
    }
    const CallNode* requantize_input = requantize_call->args[0].as<CallNode>();
    const OpNode* requantize_input_op = requantize_input->op.as<OpNode>();
    if (requantize_input_op->name == "nn.bias_add") {
      bias_add_call = requantize_input;
      conv2d_call = bias_add_call->args[0].as<CallNode>();
    } else {
      conv2d_call = requantize_input;
    }

    // TIR variables are created in the order they appear in the Relay partitioned function
    // %1 = qnn.conv2d(%input, %weight_const_0, input_zero_point_scalar,
    //                 %cmsisnn_multiplier_const_1, %input_scale_scalar, %weight_scale_const_2)
    // %2 = nn.bias_add(%1, %bias_const_3, axis=3)
    // %3 = qnn.requantize(%2, %input_scale_const_4, %cmsisnn_shift_const_5,
    //                     %output_scale_scalar, %output_zero_point_scalar)
    // clip(%3, a_min=%min_scalar, a_max=%max_scalar)
    tir::Var input("input", DataType::Handle(8));
    tir::Var filter("filter", DataType::Handle(8));
    tir::Var multiplier("multiplier", DataType::Handle(32));
    tir::Var filter_scale("filter_scale", DataType::Handle(32));
    tir::Var bias("bias", DataType::Handle(32));
    tir::Var input_scale("input_scale", DataType::Handle(32));
    tir::Var shift("shift", DataType::Handle(32));
    tir::Var output("output", DataType::Handle(8));

    // Individual arguments to the structs arguments of the CMSIS-NN API are filled into call_extern
    // https://github.com/ARM-software/CMSIS_5/blob/def6f800f95661eb3451d317f7d0dde504f6020d/CMSIS/NN/Source/ConvolutionFunctions/arm_convolve_wrapper_s8.c#L50

    // prepare cmsis_nn_conv_params
    const Conv2DAttrs* conv2d_attrs = conv2d_call->attrs.as<Conv2DAttrs>();
    int32_t input_offset = -GetScalarFromConstant<int32_t>(conv2d_call->args[2]);
    int32_t output_offset = GetScalarFromConstant<int32_t>(requantize_call->args[4]);
    int32_t stride_w = qnn::get_const_int(conv2d_attrs->strides[1]);
    int32_t stride_h = qnn::get_const_int(conv2d_attrs->strides[0]);
    int32_t padding_w = qnn::get_const_int(conv2d_attrs->padding[1]);
    int32_t padding_h = qnn::get_const_int(conv2d_attrs->padding[0]);
    int32_t dilation_w = qnn::get_const_int(conv2d_attrs->dilation[1]);
    int32_t dilation_h = qnn::get_const_int(conv2d_attrs->dilation[0]);
    int32_t clip_min, clip_max;
    if (clip_call) {
      const ClipAttrs* clip_attrs = clip_call->attrs.as<ClipAttrs>();
      clip_min = clip_attrs->a_min;
      clip_max = clip_attrs->a_max;
    } else {
      clip_min = -128;
      clip_max = 127;
    }

    tvm::Array<PrimExpr> call_ext_args = {tir::StringImm("arm_convolve_wrapper_s8"), input, filter,
                                          multiplier};
    if (bias_add_call) {
      call_ext_args.push_back(bias);
    }
    call_ext_args.push_back(shift);
    call_ext_args.push_back(output);

    tvm::Array<PrimExpr> scalar_args = {ToArg(input_offset), ToArg(output_offset), ToArg(stride_w),
                                        ToArg(stride_h),     ToArg(padding_w),     ToArg(padding_h),
                                        ToArg(dilation_w),   ToArg(dilation_h),    ToArg(clip_min),
                                        ToArg(clip_max)};

    // cmsis_nn_dims *input_dims (NHWC)
    Array<PrimExpr> input_shape = conv2d_call->args[0]->type_as<TensorTypeNode>()->shape;
    Array<PrimExpr> input_dims = CMSISNNDimensions(input_shape);

    // cmsis_nn_dims *filter_dims (OHWI)
    Array<PrimExpr> filter_shape = conv2d_call->args[1]->type_as<TensorTypeNode>()->shape;
    Array<PrimExpr> filter_dims = CMSISNNDimensions(filter_shape);

    // cmsis_nn_dims *bias_dims (1,1,1,output_channels)
    Array<PrimExpr> bias_shape{1, 1, 1, filter_shape[0]};
    Array<PrimExpr> bias_dims = CMSISNNDimensions(bias_shape);

    // cmsis_nn_dims *output_dims (NHWC)
    Array<PrimExpr> output_shape = conv2d_call->type_as<TensorTypeNode>()->shape;
    Array<PrimExpr> output_dims = CMSISNNDimensions(output_shape);

    // https://github.com/ARM-software/CMSIS_5/blob/d788fd583984388553391de18afd8b4d2a146868/CMSIS/NN/Source/ConvolutionFunctions/arm_convolve_s8.c#L367
    std::string context_buffer_name = "NULL";
    size_t context_buffer_size =
        (2 * qnn::get_const_int(input_shape[3]) * qnn::get_const_int(filter_shape[2]) *
         qnn::get_const_int(filter_shape[1]) * (int32_t)sizeof(int16_t));
    if (context_buffer_size) {
      context_buffer_name = "context_buffer_" + std::to_string(context_buffer_id_++);
    }
    tvm::Array<PrimExpr> context_buffer_args = {tir::StringImm(context_buffer_name),
                                                ToArg(context_buffer_size)};

    scalar_args = tvm::runtime::Concat(context_buffer_args, scalar_args);
    scalar_args = tvm::runtime::Concat(scalar_args, input_dims);
    scalar_args = tvm::runtime::Concat(scalar_args, filter_dims);
    scalar_args = tvm::runtime::Concat(scalar_args, bias_dims);
    scalar_args = tvm::runtime::Concat(scalar_args, output_dims);
    call_ext_args = tvm::runtime::Concat(call_ext_args, scalar_args);

    Array<tir::Var> func_signature{input, filter, multiplier, filter_scale};
    if (bias_add_call) {
      func_signature.push_back(bias);
    }
    func_signature.push_back(input_scale);
    func_signature.push_back(shift);
    func_signature.push_back(output);

    CreatePrimFuncForExtern(global_var, func_signature, call_ext_args, context_buffer_name,
                            context_buffer_size);
  }

  void EmitSoftMax(const GlobalVar& global_var, const Expr& expr) {
    const CallNode* quantize_call = expr.as<CallNode>();
    const CallNode* softmax_call = quantize_call->args[0].as<CallNode>();
    const CallNode* dequant_call = softmax_call->args[0].as<CallNode>();
    const float quant_scale = GetScalarFromConstant<float>(dequant_call->args[1]);

    // assuming layout as NHWC
    auto shape = quantize_call->type_as<TensorTypeNode>()->shape;
    int trailing_dim = shape.size() - 1;
    int row_size = shape[trailing_dim].as<tir::IntImmNode>()->value;
    int num_rows = 1;
    for (int i = 0; i < trailing_dim; ++i) {
      num_rows *= shape[i].as<tir::IntImmNode>()->value;
    }

    // calculate multiplier and shift for CMSIS-NN softmax API
    // Note: TensorFlow Lite Micro assumptions
    // Output zero point and scale are fixed to -128 and 1 / 256
    // kScaledDiffIntegerBits, kInputBits, kBeta are described on the following github page
    // https://github.com/tensorflow/tflite-micro/blob/d97cd0908d8cf5021e9d86f05a49888bee28c2a4/tensorflow/lite/micro/kernels/softmax_common.cc#L47
    double beta_multiplier = (kBeta * quant_scale * (1 << (31 - kInputBits)));
    beta_multiplier = std::min<double>(beta_multiplier, (1ll << 31) - 1.0);
    auto mult_shift_pair = tvm::relay::qnn::GetFixedPointMultiplierShift(beta_multiplier);
    int32_t mult = std::get<0>(mult_shift_pair);
    int32_t shift = std::get<1>(mult_shift_pair);
    int32_t diff_min = (1 << kScaledDiffIntegerBits) - 1;
    diff_min <<= (31 - kScaledDiffIntegerBits);
    diff_min >>= shift;
    diff_min *= -1;

    auto in_var = tir::Var("input", DataType::Handle(8));
    auto out_var = tir::Var("output", DataType::Handle(8));

    Array<tir::Var> func_signature{in_var, out_var};

    tvm::Array<PrimExpr> args = {
        tir::StringImm("arm_softmax_s8"),
        in_var,
        ToArg(num_rows),
        ToArg(row_size),
        ToArg(mult),
        ToArg(shift),
        ToArg(diff_min),
        out_var,
    };

    CreatePrimFuncForExtern(global_var, func_signature, args);
  }

  void EmitMul(const GlobalVar& global_var, const Expr& expr) {
    auto* mul_call = expr.as<CallNode>();

    const float input_0_scale = GetScalarFromConstant<float>(mul_call->args[2]);
    const int32_t input_0_zero_point = GetScalarFromConstant<int32_t>(mul_call->args[3]);
    const float input_1_scale = GetScalarFromConstant<float>(mul_call->args[4]);
    const int32_t input_1_zero_point = GetScalarFromConstant<int32_t>(mul_call->args[5]);
    const float output_scale = GetScalarFromConstant<float>(mul_call->args[6]);
    const int32_t output_zero_point = GetScalarFromConstant<int32_t>(mul_call->args[7]);

    double quantized_multiplier = static_cast<double>(input_0_scale) *
                                  static_cast<double>(input_1_scale) /
                                  static_cast<double>(output_scale);
    auto mult_shift_pair = tvm::relay::qnn::GetFixedPointMultiplierShift(quantized_multiplier);
    int32_t output_multiplier = std::get<0>(mult_shift_pair);
    int32_t output_shift = std::get<1>(mult_shift_pair);

    PrimExpr tensor_size = mul_call->type_as<TensorTypeNode>()->Size();

    tir::Var input_0("input_0", DataType::Handle(8));
    tir::Var input_1("input_1", DataType::Handle(8));
    tir::Var output("output", DataType::Handle(8));

    Array<tir::Var> func_signature{input_0, input_1, output};

    tvm::Array<PrimExpr> args = {
        tir::StringImm("arm_elementwise_mul_s8"),
        input_0,
        input_1,
        ToArg(-input_0_zero_point),
        ToArg(-input_1_zero_point),
        output,
        ToArg(output_zero_point),
        ToArg(output_multiplier),
        ToArg(output_shift),
        ToArg(std::numeric_limits<int8_t>::min()),
        ToArg(std::numeric_limits<int8_t>::max()),
        tensor_size,
    };

    CreatePrimFuncForExtern(global_var, func_signature, args);
  }

  void EmitAdd(const GlobalVar& global_var, const Expr& expr) {
    auto* add_call = expr.as<CallNode>();

    const float input_0_scale = GetScalarFromConstant<float>(add_call->args[2]);
    const int32_t input_0_zero_point = GetScalarFromConstant<int32_t>(add_call->args[3]);
    const float input_1_scale = GetScalarFromConstant<float>(add_call->args[4]);
    const int32_t input_1_zero_point = GetScalarFromConstant<int32_t>(add_call->args[5]);
    const float output_scale = GetScalarFromConstant<float>(add_call->args[6]);
    const int32_t output_zero_point = GetScalarFromConstant<int32_t>(add_call->args[7]);

    const int32_t left_shift = 20;
    const int32_t input_0_offset = -input_0_zero_point;
    const int32_t input_1_offset = -input_1_zero_point;

    const float max_input_scale = std::max(input_0_scale, input_1_scale);
    const double twice_max_input_scale = 2 * static_cast<double>(max_input_scale);
    const double scaled_input_0_scale = static_cast<double>(input_0_scale) / twice_max_input_scale;
    const double scaled_input_1_scale = static_cast<double>(input_1_scale) / twice_max_input_scale;
    const double scaled_output_scale =
        twice_max_input_scale / ((1 << left_shift) * static_cast<double>(output_scale));

    auto input_0_mult_shift_pair =
        tvm::relay::qnn::GetFixedPointMultiplierShift(scaled_input_0_scale);
    int32_t input_0_multiplier = std::get<0>(input_0_mult_shift_pair);
    int32_t input_0_shift = std::get<1>(input_0_mult_shift_pair);

    auto input_1_mult_shift_pair =
        tvm::relay::qnn::GetFixedPointMultiplierShift(scaled_input_1_scale);
    int32_t input_1_multiplier = std::get<0>(input_1_mult_shift_pair);
    int32_t input_1_shift = std::get<1>(input_1_mult_shift_pair);

    auto output_mult_shift_pair =
        tvm::relay::qnn::GetFixedPointMultiplierShift(scaled_output_scale);
    int32_t output_multiplier = std::get<0>(output_mult_shift_pair);
    int32_t output_shift = std::get<1>(output_mult_shift_pair);

    PrimExpr tensor_size = add_call->type_as<TensorTypeNode>()->Size();

    tir::Var input_0("input_0", DataType::Handle(8));
    tir::Var input_1("input_1", DataType::Handle(8));
    tir::Var output("output", DataType::Handle(8));

    Array<tir::Var> func_signature{input_0, input_1, output};

    tvm::Array<PrimExpr> args = {
        tir::StringImm("arm_elementwise_add_s8"),
        input_0,
        input_1,
        ToArg(input_0_offset),
        ToArg(input_0_multiplier),
        ToArg(input_0_shift),
        ToArg(input_1_offset),
        ToArg(input_1_multiplier),
        ToArg(input_1_shift),
        ToArg(left_shift),
        output,
        ToArg(output_zero_point),
        ToArg(output_multiplier),
        ToArg(output_shift),
        ToArg(std::numeric_limits<int8_t>::min()),
        ToArg(std::numeric_limits<int8_t>::max()),
        tensor_size,
    };

    CreatePrimFuncForExtern(global_var, func_signature, args);
  }

  Expr Rewrite_(const CallNode* pre, const Expr& post) override {
    if (const CallNode* call = post.as<CallNode>()) {
      auto* func = call->op.as<FunctionNode>();
      if (func == nullptr) {
        return post;
      }

      auto codegen_name = func->GetAttr<String>(attr::kCompiler);
      if (codegen_name.defined() && codegen_name == "cmsis-nn") {
        const CallNode* inner_call = func->body.as<CallNode>();
        const FunctionNode* composite_func = inner_call->op.as<FunctionNode>();
        auto comp_name = composite_func->GetAttr<String>(attr::kComposite);
        auto func_name = func->GetAttr<String>(::tvm::attr::kGlobalSymbol);

        GlobalVar new_global_var(func_name.value());
        new_global_var->checked_type_ = composite_func->checked_type();

        if (comp_name == "cmsis-nn.qnn_softmax") {
          EmitSoftMax(new_global_var, composite_func->body);
        }
        if (comp_name == "cmsis-nn.qnn_mul") {
          EmitMul(new_global_var, composite_func->body);
        }
        if (comp_name == "cmsis-nn.qnn_add") {
          EmitAdd(new_global_var, composite_func->body);
        }
        if (comp_name == "cmsis-nn.qnn_conv2d") {
          EmitConv2D(new_global_var, composite_func->body);
        }

        Array<Expr> args;
        for (const auto& arg : call->args) {
          args.push_back(VisitExpr(arg));
        }

        return Call(new_global_var, args, call->attrs, call->type_args, call->span);
      }
    }

    return post;
  }

 private:
  static constexpr int32_t kScaledDiffIntegerBits = 5;
  static constexpr int32_t kInputBits = 5;
  static constexpr double kBeta = 1.0;
  int32_t context_buffer_id_;
  IRModule ir_module_;
  Target target_;
};

tvm::transform::Pass RelayToTIR() {
  runtime::TypedPackedFunc<IRModule(IRModule, transform::PassContext)> pass_func =
      [=](IRModule ir_module, transform::PassContext pass_context) {
        auto relay_to_tir = RelayToTIRVisitor(ir_module, Target("cmsis-nn"));
        return relay_to_tir.Mutate();
      };
  return tvm::transform::CreateModulePass(pass_func, 0, "RelayToTIR", {});
}

}  // namespace cmsisnn
}  // namespace contrib
}  // namespace relay
}  // namespace tvm

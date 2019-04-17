#include "oneflow/core/operator/layer_norm_param_grad_op.h"

namespace oneflow {

namespace {

class LayerNormParamGradDataParallelSbpSignatureRule final : public ParallelSbpSignatureRule {
 public:
  OF_DISALLOW_COPY_AND_MOVE(LayerNormParamGradDataParallelSbpSignatureRule);
  ~LayerNormParamGradDataParallelSbpSignatureRule() override = default;

  explicit LayerNormParamGradDataParallelSbpSignatureRule(const Operator* op)
      : ParallelSbpSignatureRule(op) {}

  const std::string Description() const override {
    return op().op_name() + ": (S(0), B) -> (S(0), P)";
  }

  const SbpSigMatchResult MatchByIbnHint(
      const std::function<const SbpInferHint&(const std::string&)>& SbpInferHint4BnInOp,
      const ParallelDesc& parallel_desc) const override {
    if (parallel_desc.policy() == kDataParallel) { return MakeSbpSigMatchSuccess(); }
    return MakeSbpSigMatchParallelPolicyError(parallel_desc.policy(), kDataParallel);
  }

  void GenerateSignature(
      const std::function<const SbpInferHint&(const std::string&)>& SbpInferHint4BnInOp,
      SbpSignature* sbp_signature) const override {
    auto* bn2sbp = sbp_signature->mutable_bn_in_op2sbp_parallel();
    for (const std::string& ibn : op().input_bns()) {
      if (ibn == "gamma") {
        (*bn2sbp)[ibn].mutable_broadcast_parallel();
      } else {
        (*bn2sbp)[ibn].mutable_split_parallel()->set_axis(0);
      }
    }
    for (const std::string& obn : op().output_bns()) {
      if (obn == "beta_diff" || obn == "gamma_diff") {
        (*bn2sbp)[obn].mutable_partial_sum_parallel();
      } else {
        (*bn2sbp)[obn].mutable_split_parallel()->set_axis(0);
      }
    }
  }
};

}  // namespace

void LayerNormParamGradOp::InitFromOpConf() {
  CHECK(op_conf().has_layer_norm_param_grad_conf());
  const LayerNormParamGradOpConf& conf = op_conf().layer_norm_param_grad_conf();
  CHECK(conf.has_beta_diff() || conf.has_gamma_diff() || conf.has_normalized_diff());
  EnrollInputBn("dy", false);
  if (conf.has_beta_diff()) { EnrollOutputBn("beta_diff", false); }
  if (conf.has_gamma_diff()) {
    EnrollInputBn("normalized", false);
    EnrollOutputBn("gamma_diff", false);
  }
  if (conf.has_beta_diff() || conf.has_gamma_diff()) { EnrollFwBufBn("reduce_buf"); }
  if (conf.has_normalized_diff()) { EnrollOutputBn("normalized_diff", false); }
  if (conf.has_normalized_diff() || conf.has_gamma_diff()) { CHECK(conf.has_gamma()); }
  if (conf.has_gamma()) { EnrollInputBn("gamma", false); }
}

void LayerNormParamGradOp::InferBlobDescs(
    std::function<BlobDesc*(const std::string&)> GetBlobDesc4BnInOp,
    const ParallelContext* parallel_ctx) const {
  CHECK(parallel_ctx->policy() != kModelParallel);
  const LayerNormParamGradOpConf& conf = op_conf().layer_norm_param_grad_conf();
  const BlobDesc* dy = GetBlobDesc4BnInOp("dy");
  if (conf.has_beta_diff() || conf.has_gamma_diff()) {
    BlobDesc* reduce_buf = GetBlobDesc4BnInOp("reduce_buf");
    reduce_buf->set_data_type(dy->data_type());
    reduce_buf->mut_shape() = dy->shape();
  }
  const int64_t begin_params_axis = conf.begin_params_axis() < 0
                                        ? dy->shape().NumAxes() + conf.begin_params_axis()
                                        : conf.begin_params_axis();
  CHECK_GE(begin_params_axis, 1);
  CHECK_LT(begin_params_axis, dy->shape().NumAxes());
  std::vector<int64_t> param_shape_dim_vec;
  param_shape_dim_vec.insert(param_shape_dim_vec.end(),
                             dy->shape().dim_vec().cbegin() + begin_params_axis,
                             dy->shape().dim_vec().cend());
  if (param_shape_dim_vec.empty()) { param_shape_dim_vec.push_back(1); }
  const Shape param_shape(param_shape_dim_vec);
  if (conf.has_beta_diff()) {
    BlobDesc* beta_diff = GetBlobDesc4BnInOp("beta_diff");
    beta_diff->mut_shape() = param_shape;
    beta_diff->set_data_type(dy->data_type());
  }
  if (conf.has_gamma_diff()) {
    const BlobDesc* normalized = GetBlobDesc4BnInOp("normalized");
    CHECK_EQ(normalized->data_type(), dy->data_type());
    CHECK_EQ(normalized->shape(), dy->shape());
    BlobDesc* gamma_diff = GetBlobDesc4BnInOp("gamma_diff");
    gamma_diff->mut_shape() = param_shape;
    gamma_diff->set_data_type(dy->data_type());
  }
  if (conf.has_normalized_diff()) { *GetBlobDesc4BnInOp("normalized_diff") = *dy; }
  if (conf.has_gamma()) {
    const BlobDesc* gamma = GetBlobDesc4BnInOp("gamma");
    CHECK_EQ(gamma->data_type(), dy->data_type());
    CHECK_EQ(gamma->shape(), param_shape);
  }
}

void LayerNormParamGradOp::GetSbpSignatureRules(
    const std::function<const SbpInferHint&(const std::string&)>& SbpInferHint4Ibn,
    std::vector<std::unique_ptr<const SbpSignatureRule>>* rules) const {
  rules->emplace_back(new LayerNormParamGradDataParallelSbpSignatureRule(this));
}

REGISTER_OP(OperatorConf::kLayerNormParamGradConf, LayerNormParamGradOp);

}  // namespace oneflow

/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObLongTextType);
    } else {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ObNullType == types_stack[DIM_IDX].get_type()) {
        // a null dimension means "not specified"
      } else if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    if (OB_SUCC(ret)) {
      type.set_varchar();
      type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      type.set_collation_level(CS_LEVEL_COERCIBLE);
    }
  }
  return ret;
}

int ObExprAIEmbed::read_arg_string(ObEvalCtx &ctx,
                                   common::ObIAllocator &allocator,
                                   const ObExpr &expr,
                                   const int64_t arg_idx,
                                   ObDatum &datum,
                                   common::ObString &str)
{
  int ret = OB_SUCCESS;
  ObExpr *arg = expr.args_[arg_idx];
  if (OB_ISNULL(arg)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ob_is_text_tc(arg->datum_meta_.type_)) {
    // text/lob values may be stored out of row, read the real string data first
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, datum,
                                                          arg->datum_meta_,
                                                          arg->obj_meta_.has_lob_header(), str))) {
    }
  } else {
    str = datum.get_string();
  }
  return ret;
}

int ObExprAIEmbed::check_dimension(const int64_t dimension)
{
  int ret = OB_SUCCESS;
  if (dimension <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  }
  return ret;
}

int ObExprAIEmbed::build_embed_config(common::ObIAllocator &allocator,
                                      const int64_t dimension,
                                      common::ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *dim_json = nullptr;
  if (dimension <= 0) {
    config = nullptr;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim_json))) {
  } else if (OB_FAIL(config_obj->add("dimensions", dim_json))) {
  } else {
    config = config_obj;
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (OB_ISNULL(arg_model_id) || arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (OB_ISNULL(arg_content) || arg_content->is_null()) {
    // a null input propagates as a null embedding, it is never sent to the service
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    int64_t dim = 0;
    ObJsonObject *config = nullptr;
    if (model_id.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
      res.set_null();
    } else if (OB_FAIL(read_arg_string(ctx, tmp_alloc_g.get_allocator(), expr,
                                       CONTENT_IDX, *arg_content, content))) {
    } else if (content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
      res.set_null();
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (OB_FAIL(check_dimension(dim))) {
      } else if (OB_FAIL(build_embed_config(temp_allocator, dim, config))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      } else {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr,
                                       ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *results = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObDatumVector model_ids;
  ObDatumVector contents;
  ObDatumVector dims;
  if (expr.arg_cnt_ == 3) {
    ret = expr.eval_batch_param_value(ctx, skip, batch_size, model_ids, contents, dims);
  } else {
    ret = expr.eval_batch_param_value(ctx, skip, batch_size, model_ids, contents);
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(results)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString uniform_model_id;
    int64_t uniform_dim = 0;
    bool first_found = false;
    bool uniform_request = true;
    // first pass: drop null inputs and check whether all rows can share one request
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum *model_id_datum = model_ids.at(i);
      ObDatum *content_datum = contents.at(i);
      if (OB_ISNULL(model_id_datum) || OB_ISNULL(content_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (model_id_datum->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
      } else if (content_datum->is_null()) {
        // null rows produce null results and never enter the request
        results[i].set_null();
        eval_flags.set(i);
      } else {
        int64_t dim = 0;
        if (expr.arg_cnt_ == 3) {
          ObDatum *dim_datum = dims.at(i);
          if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
            dim = dim_datum->get_int();
            if (OB_FAIL(check_dimension(dim))) {
            }
          }
        }
        if (OB_SUCC(ret)) {
          ObString model_id = model_id_datum->get_string();
          if (!first_found) {
            uniform_model_id = model_id;
            uniform_dim = dim;
            first_found = true;
          } else if (uniform_dim != dim
                     || uniform_model_id.length() != model_id.length()
                     || 0 != MEMCMP(uniform_model_id.ptr(), model_id.ptr(), model_id.length())) {
            uniform_request = false;
          }
        }
      }
    }

    if (OB_FAIL(ret) || !first_found) {
    } else {
      ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
      batch_info_guard.set_batch_size(batch_size);
      if (uniform_request) {
        // one request carries the inputs of all rows
        ObArray<ObString> content_array;
        ObArray<int64_t> row_idxs;
        ObArray<ObString> result_strs;
        for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
          if (skip.at(i) || eval_flags.at(i)) {
            continue;
          }
          ObDatum *content_datum = contents.at(i);
          ObString content;
          if (OB_ISNULL(content_datum)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(read_arg_string(ctx, tmp_alloc_g.get_allocator(), expr,
                                             CONTENT_IDX, *content_datum, content))) {
          } else if (content.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
          } else if (OB_FAIL(content_array.push_back(content))) {
          } else if (OB_FAIL(row_idxs.push_back(i))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(eval_ai_embed_batch_request(expr, ctx, temp_allocator,
                                                       uniform_model_id, uniform_dim,
                                                       content_array, result_strs))) {
        } else if (result_strs.count() != row_idxs.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < result_strs.count(); ++i) {
            batch_info_guard.set_batch_idx(row_idxs.at(i));
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, results[row_idxs.at(i)],
                                                         result_strs.at(i)))) {
            } else {
              eval_flags.set(row_idxs.at(i));
            }
          }
        }
      } else {
        // rows use different models or dimensions, fall back to one request per row
        for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
          if (skip.at(i) || eval_flags.at(i)) {
            continue;
          }
          ObDatum *model_id_datum = model_ids.at(i);
          ObDatum *content_datum = contents.at(i);
          ObString content;
          ObString result;
          int64_t dim = 0;
          if (OB_ISNULL(model_id_datum) || OB_ISNULL(content_datum)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (expr.arg_cnt_ == 3 && OB_NOT_NULL(dims.at(i)) && !dims.at(i)->is_null()) {
            dim = dims.at(i)->get_int();
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(read_arg_string(ctx, tmp_alloc_g.get_allocator(), expr,
                                             CONTENT_IDX, *content_datum, content))) {
          } else if (content.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
          } else if (OB_FAIL(eval_ai_embed_single_request(expr, ctx, temp_allocator,
                                                          model_id_datum->get_string(),
                                                          content, dim, result))) {
          } else {
            batch_info_guard.set_batch_idx(i);
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, results[i], result))) {
            } else {
              eval_flags.set(i);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch_request(const ObExpr &expr,
                                               ObEvalCtx &ctx,
                                               common::ObIAllocator &allocator,
                                               const common::ObString &model_id,
                                               const int64_t dimension,
                                               common::ObArray<common::ObString> &contents,
                                               common::ObArray<common::ObString> &result_strs)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  UNUSED(expr);
  UNUSED(ctx);
  if (contents.empty()) {
  } else if (OB_FAIL(build_embed_config(allocator, dimension, config))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                 model_id, allocator, resolved_endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, resolved_endpoint);
    if (OB_FAIL(model.call_dense_embedding_vector(contents, config, result_strs))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_single_request(const ObExpr &expr,
                                                ObEvalCtx &ctx,
                                                common::ObIAllocator &allocator,
                                                const common::ObString &model_id,
                                                const common::ObString &content,
                                                const int64_t dimension,
                                                common::ObString &result)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  UNUSED(expr);
  UNUSED(ctx);
  if (OB_FAIL(build_embed_config(allocator, dimension, config))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                 model_id, allocator, resolved_endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, resolved_endpoint);
    ObString input_str = content;
    if (OB_FAIL(model.call_dense_embedding(input_str, config, result))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

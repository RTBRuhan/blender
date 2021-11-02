/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2006 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup cmpnodes
 */

#include "node_composite_util.hh"

/* ******************* Color Balance ********************************* */

namespace blender::nodes {

static void cmp_node_colorbalance_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Fac").default_value(1.0f).min(0.0f).max(1.0f).subtype(PROP_FACTOR);
  b.add_input<decl::Color>("Image").default_value({1.0f, 1.0f, 1.0f, 1.0f});
  b.add_output<decl::Color>("Image");
}

}  // namespace blender::nodes

/* Sync functions update formula parameters for other modes, such that the result is comparable.
 * Note that the results are not exactly the same due to differences in color handling
 * (sRGB conversion happens for LGG),
 * but this keeps settings comparable.
 */

void ntreeCompositColorBalanceSyncFromLGG(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeColorBalance *n = (NodeColorBalance *)node->storage;

  for (int c = 0; c < 3; c++) {
    n->slope[c] = (2.0f - n->lift[c]) * n->gain[c];
    n->offset[c] = (n->lift[c] - 1.0f) * n->gain[c];
    n->power[c] = (n->gamma[c] != 0.0f) ? 1.0f / n->gamma[c] : 1000000.0f;
  }
}

void ntreeCompositColorBalanceSyncFromCDL(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeColorBalance *n = (NodeColorBalance *)node->storage;

  for (int c = 0; c < 3; c++) {
    float d = n->slope[c] + n->offset[c];
    n->lift[c] = (d != 0.0f ? n->slope[c] + 2.0f * n->offset[c] / d : 0.0f);
    n->gain[c] = d;
    n->gamma[c] = (n->power[c] != 0.0f) ? 1.0f / n->power[c] : 1000000.0f;
  }
}

static void node_composit_init_colorbalance(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeColorBalance *n = (NodeColorBalance *)MEM_callocN(sizeof(NodeColorBalance),
                                                        "node colorbalance");

  n->lift[0] = n->lift[1] = n->lift[2] = 1.0f;
  n->gamma[0] = n->gamma[1] = n->gamma[2] = 1.0f;
  n->gain[0] = n->gain[1] = n->gain[2] = 1.0f;

  n->slope[0] = n->slope[1] = n->slope[2] = 1.0f;
  n->offset[0] = n->offset[1] = n->offset[2] = 0.0f;
  n->power[0] = n->power[1] = n->power[2] = 1.0f;
  node->storage = n;
}

static int node_composite_gpu_colorbalance(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData *UNUSED(execdata),
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  NodeColorBalance *n = (NodeColorBalance *)node->storage;

  if (node->custom1 == 0) {
    return GPU_stack_link(mat,
                          node,
                          "node_composite_color_balance_lgg",
                          in,
                          out,
                          GPU_uniform(n->lift),
                          GPU_uniform(n->gamma),
                          GPU_uniform(n->gain));
  }

  return GPU_stack_link(mat,
                        node,
                        "node_composite_color_balance_asc_cdl",
                        in,
                        out,
                        GPU_uniform(n->offset),
                        GPU_uniform(n->power),
                        GPU_uniform(n->slope),
                        GPU_uniform(&n->offset_basis));
}

void register_node_type_cmp_colorbalance(void)
{
  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_COLORBALANCE, "Color Balance", NODE_CLASS_OP_COLOR, 0);
  ntype.declare = blender::nodes::cmp_node_colorbalance_declare;
  node_type_size(&ntype, 400, 200, 400);
  node_type_init(&ntype, node_composit_init_colorbalance);
  node_type_storage(
      &ntype, "NodeColorBalance", node_free_standard_storage, node_copy_standard_storage);
  node_type_gpu(&ntype, node_composite_gpu_colorbalance);

  nodeRegisterType(&ntype);
}

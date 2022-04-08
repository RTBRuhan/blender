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

#include "UI_interface.h"
#include "UI_resources.h"

#include "GPU_material.h"

#include "VPC_compositor_execute.hh"

#include "node_composite_util.hh"

/* ******************* channel Difference Matte ********************************* */

namespace blender::nodes::node_composite_diff_matte_cc {

static void cmp_node_diff_matte_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>(N_("Image 1"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_input<decl::Color>(N_("Image 2"))
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(1);
  b.add_output<decl::Color>(N_("Image"));
  b.add_output<decl::Color>(N_("Matte"));
}

static void node_composit_init_diff_matte(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeChroma *c = MEM_cnew<NodeChroma>(__func__);
  node->storage = c;
  c->t1 = 0.1f;
  c->t2 = 0.1f;
}

static void node_composit_buts_diff_matte(uiLayout *layout, bContext *UNUSED(C), PointerRNA *ptr)
{
  uiLayout *col;

  col = uiLayoutColumn(layout, true);
  uiItemR(
      col, ptr, "tolerance", UI_ITEM_R_SPLIT_EMPTY_NAME | UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
  uiItemR(col, ptr, "falloff", UI_ITEM_R_SPLIT_EMPTY_NAME | UI_ITEM_R_SLIDER, nullptr, ICON_NONE);
}

using namespace blender::viewport_compositor;

class DifferenceMatteGPUMaterialNode : public GPUMaterialNode {
 public:
  using GPUMaterialNode::GPUMaterialNode;

  void compile(GPUMaterial *material) override
  {
    GPUNodeStack *inputs = get_inputs_array();
    GPUNodeStack *outputs = get_outputs_array();

    const float tolerance = get_tolerance();
    const float falloff = get_falloff();

    GPU_stack_link(material,
                   &node(),
                   "node_composite_difference_matte",
                   inputs,
                   outputs,
                   GPU_uniform(&tolerance),
                   GPU_uniform(&falloff));
  }

  NodeChroma *get_node_chroma()
  {
    return static_cast<NodeChroma *>(node().storage);
  }

  float get_tolerance()
  {
    return get_node_chroma()->t1;
  }

  float get_falloff()
  {
    return get_node_chroma()->t2;
  }
};

static GPUMaterialNode *get_compositor_gpu_material_node(DNode node)
{
  return new DifferenceMatteGPUMaterialNode(node);
}

}  // namespace blender::nodes::node_composite_diff_matte_cc

void register_node_type_cmp_diff_matte()
{
  namespace file_ns = blender::nodes::node_composite_diff_matte_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_DIFF_MATTE, "Difference Key", NODE_CLASS_MATTE);
  ntype.declare = file_ns::cmp_node_diff_matte_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_diff_matte;
  ntype.flag |= NODE_PREVIEW;
  node_type_init(&ntype, file_ns::node_composit_init_diff_matte);
  node_type_storage(&ntype, "NodeChroma", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_gpu_material_node = file_ns::get_compositor_gpu_material_node;

  nodeRegisterType(&ntype);
}

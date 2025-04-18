/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_hash.h"
#include "BLI_time.h"

#include "DNA_object_types.h"

#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "paint_intern.hh"
#include "paint_mask.hh"
#include "sculpt_face_set.hh"
#include "sculpt_hide.hh"
#include "sculpt_intern.hh"
#include "sculpt_islands.hh"
#include "sculpt_undo.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::mask {

enum class InitMode {
  Random,
  FaceSet,
  Island,
};

void write_mask_mesh(const Depsgraph &depsgraph,
                     Object &object,
                     const IndexMask &node_mask,
                     FunctionRef<void(MutableSpan<float>, Span<int>)> write_fn)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

  undo::push_nodes(depsgraph, object, node_mask, undo::Type::Mask);

  bke::SpanAttributeWriter mask = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);
  if (!mask) {
    return;
  }
  threading::EnumerableThreadSpecific<Vector<int>> all_index_data;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    Vector<int> &index_data = all_index_data.local();
    write_fn(mask.span, hide::node_visible_verts(nodes[i], hide_vert, index_data));
    bke::pbvh::node_update_mask_mesh(mask.span, nodes[i]);
  });
  pbvh.tag_masks_changed(node_mask);
  mask.finish();
}

static void init_mask_grids(
    Main &bmain,
    Scene &scene,
    Depsgraph &depsgraph,
    Object &object,
    const IndexMask &node_mask,
    FunctionRef<void(const BitGroupVector<> &, int, MutableSpan<float>)> write_fn)
{
  MultiresModifierData *mmd = BKE_sculpt_multires_active(&scene, &object);
  BKE_sculpt_mask_layers_ensure(&depsgraph, &bmain, &object, mmd);

  SculptSession &ss = *object.sculpt;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  MutableSpan<float> masks = subdiv_ccg.masks;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;

  undo::push_nodes(depsgraph, object, node_mask, undo::Type::Mask);

  node_mask.foreach_index(GrainSize(1), [&](const int i) {
    for (const int grid : nodes[i].grids()) {
      write_fn(grid_hidden, grid, masks.slice(bke::ccg::grid_range(key, grid)));
    }
    bke::pbvh::node_update_mask_grids(key, masks, nodes[i]);
  });
  pbvh.tag_masks_changed(node_mask);
  BKE_subdiv_ccg_average_grids(subdiv_ccg);
}

static wmOperatorStatus sculpt_mask_init_exec(bContext *C, wmOperator *op)
{
  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }
  const Scene &scene = *CTX_data_scene(C);
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);

  BKE_sculpt_update_object_for_edit(&depsgraph, &ob, false);

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  if (node_mask.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  undo::push_begin(scene, ob, op);

  const InitMode mode = InitMode(RNA_enum_get(op->ptr, "mode"));
  const int seed = BLI_time_now_seconds();

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      switch (mode) {
        case InitMode::Random:
          write_mask_mesh(depsgraph, ob, node_mask, [&](MutableSpan<float> mask, Span<int> verts) {
            for (const int vert : verts) {
              mask[vert] = BLI_hash_int_01(vert + seed);
            }
          });
          break;
        case InitMode::FaceSet: {
          const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
          const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
          const bke::AttributeAccessor attributes = mesh.attributes();
          const VArraySpan face_sets = *attributes.lookup_or_default<int>(
              ".sculpt_face_set", bke::AttrDomain::Face, 1);

          write_mask_mesh(depsgraph, ob, node_mask, [&](MutableSpan<float> mask, Span<int> verts) {
            for (const int vert : verts) {
              const int face_set = face_set::vert_face_set_get(vert_to_face_map, face_sets, vert);
              mask[vert] = BLI_hash_int_01(face_set + seed);
            }
          });
          break;
        }
        case InitMode::Island:
          islands::ensure_cache(ob);
          write_mask_mesh(depsgraph, ob, node_mask, [&](MutableSpan<float> mask, Span<int> verts) {
            for (const int vert : verts) {
              const int island = islands::vert_id_get(ss, vert);
              mask[vert] = BLI_hash_int_01(island + seed);
            }
          });
          break;
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      Main &bmain = *CTX_data_main(C);
      Scene &scene = *CTX_data_scene(C);
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      switch (mode) {
        case InitMode::Random: {
          init_mask_grids(bmain,
                          scene,
                          depsgraph,
                          ob,
                          node_mask,
                          [&](const BitGroupVector<> &grid_hidden,
                              const int grid_index,
                              MutableSpan<float> grid_masks) {
                            const int verts_start = grid_index * key.grid_area;
                            BKE_subdiv_ccg_foreach_visible_grid_vert(
                                key, grid_hidden, grid_index, [&](const int i) {
                                  grid_masks[i] = BLI_hash_int_01(verts_start + i + seed);
                                });
                          });
          break;
        }
        case InitMode::FaceSet: {
          const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
          const bke::AttributeAccessor attributes = mesh.attributes();
          const VArraySpan face_sets = *attributes.lookup_or_default<int>(
              ".sculpt_face_set", bke::AttrDomain::Face, 1);
          const Span<int> grid_to_face = subdiv_ccg.grid_to_face_map;
          init_mask_grids(
              bmain,
              scene,
              depsgraph,
              ob,
              node_mask,
              [&](const BitGroupVector<> &grid_hidden,
                  const int grid_index,
                  MutableSpan<float> grid_masks) {
                const int face_set = face_sets[grid_to_face[grid_index]];
                const float value = BLI_hash_int_01(face_set + seed);
                BKE_subdiv_ccg_foreach_visible_grid_vert(
                    key, grid_hidden, grid_index, [&](const int i) { grid_masks[i] = value; });
              });
          break;
        }
        case InitMode::Island: {
          islands::ensure_cache(ob);
          init_mask_grids(bmain,
                          scene,
                          depsgraph,
                          ob,
                          node_mask,
                          [&](const BitGroupVector<> &grid_hidden,
                              const int grid_index,
                              MutableSpan<float> grid_masks) {
                            const int verts_start = grid_index * key.grid_area;
                            BKE_subdiv_ccg_foreach_visible_grid_vert(
                                key, grid_hidden, grid_index, [&](const int i) {
                                  const int island = islands::vert_id_get(ss, verts_start + i);
                                  grid_masks[i] = BLI_hash_int_01(island + seed);
                                });
                          });
          break;
        }
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      const int offset = CustomData_get_offset_named(&ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
      undo::push_nodes(depsgraph, ob, node_mask, undo::Type::Mask);
      node_mask.foreach_index(GrainSize(1), [&](const int i) {
        for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(&nodes[i])) {
          if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
            continue;
          }
          switch (mode) {
            case InitMode::Random:
              BM_ELEM_CD_SET_FLOAT(vert, offset, BLI_hash_int_01(BM_elem_index_get(vert) + seed));
              break;
            case InitMode::FaceSet: {
              BM_ELEM_CD_SET_FLOAT(vert, offset, 0.0f);
              break;
            }
            case InitMode::Island:
              BM_ELEM_CD_SET_FLOAT(
                  vert,
                  offset,
                  BLI_hash_int_01(islands::vert_id_get(ss, BM_elem_index_get(vert)) + seed));
              break;
          }
        }
        bke::pbvh::node_update_mask_bmesh(offset, nodes[i]);
      });
      pbvh.tag_masks_changed(node_mask);
      break;
    }
  }

  undo::push_end(ob);

  SCULPT_tag_update_overlays(C);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_mask_init(wmOperatorType *ot)
{
  ot->name = "Init Mask";
  ot->description = "Creates a new mask for the entire mesh";
  ot->idname = "SCULPT_OT_mask_init";

  ot->exec = sculpt_mask_init_exec;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  static EnumPropertyItem modes[] = {
      {int(InitMode::Random), "RANDOM_PER_VERTEX", 0, "Random per Vertex", ""},
      {int(InitMode::FaceSet), "RANDOM_PER_FACE_SET", 0, "Random per Face Set", ""},
      {int(InitMode::Island), "RANDOM_PER_LOOSE_PART", 0, "Random per Loose Part", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_enum(ot->srna, "mode", modes, int(InitMode::Random), "Mode", "");
}

}  // namespace blender::ed::sculpt_paint::mask

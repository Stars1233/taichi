#include "taichi_grid.h"
#include <taichi/visual/texture.h>
#include <taichi/system/threading.h>
#include <taichi/util.h>
#include <taichi/math/svd.h>
#include <experimental/filesystem>
#include <taichi/visualization/particle_visualization.h>
#include <taichi/visual/gui.h>
#include "stencil.h"

TC_NAMESPACE_BEGIN

real gravity = 100;
real buoyancy = 300;
real temperature_decay = 1;
Vector2i cam_res(720, 1280);
int zero = 0;

constexpr int smoothing_fusion = 2;
constexpr bool debug = false;
constexpr bool use_preconditioner = false;

struct BlockFlags : public bit::Bits<32> {
  using Base = bit::Bits<32>;
  // TC_BIT_FIELD(uint8, num_effective_neighbours, 0);
  TC_BIT_FIELD(bool, has_effective_cell, 8);
};

struct NodeFlags : public bit::Bits<32> {
  using Base = bit::Bits<32>;
  TC_BIT_FIELD(uint8, num_effective_neighbours, 0);
  TC_BIT_FIELD(bool, effective, 8);
};

struct Node {
  constexpr static int num_channels = 16;
  using element_type = real;
  /*
  NodeFlags &flags() {
    return bit::reinterpret_bits<NodeFlags>(channels[15]);
  }
  */
};

template <>
constexpr bool is_SOA<Node>() {
  return true;
}

struct Particle {
  Vector3 pos;
};

Vector3 hsv2rgb(Vector3 hsv) {
  real h = hsv.x;
  real s = hsv.y;
  real v = hsv.z;
  int j = (int)floor(h * 6);
  real f = h * 6 - j;
  real p = v * (1 - s);
  real q = v * (1 - f * s);
  real t = v * (1 - (1 - f) * s);
  real r, g, b;
  switch (j % 6) {
    case 0:
      r = v, g = t, b = p;
      break;
    case 1:
      r = q, g = v, b = p;
      break;
    case 2:
      r = p, g = v, b = t;
      break;
    case 3:
      r = p, g = q, b = v;
      break;
    case 4:
      r = t, g = p, b = v;
      break;
    default:  // 5, actually
      r = v, g = p, b = q;
      break;
  }
  return Vector3(r, g, b);
}

constexpr int per_block_particles = 0; // 2048

using Block = TBlock<Node, Particle, TSize3D<8>, 0, per_block_particles, BlockFlags>;

class MGPCGSmoke {
 public:
  static constexpr auto dim = Block::dim;
  using Vector = TVector<real, dim>;
  using VectorP = TVector<real, dim + 1>;
  using Matrix = TMatrix<real, dim>;
  using Grid = TaichiGrid<Block>;
  std::string folder;

  const int n = 128;
  int mg_lv = log2int(n) - 2;
  std::vector<std::unique_ptr<Grid>> grids;
  const bool reflection = false;

  enum {
    CH_R,
    CH_Z,
    CH_X,
    CH_B,
    CH_P,
    CH_MG_U,
    CH_MG_V,  // V  is a copy of U, for smoothing iterations
    CH_MG_B,
    CH_MG_R,
    CH_VX,
    CH_VY,
    CH_VZ,
    CH_RHO,
  };

  using VectorI = Vector3i;
  using Vectori = VectorI;
  using GridScratchPad = TGridScratchPad<Block>;
  using GridScratchPadCh = TGridScratchPad<Block, real>;
  using GridScratchPadCh2 = TGridScratchPad<Block, real, 2>;
  using GridScratchPadCh4 = TGridScratchPad<Block, real, 4>;

  std::shared_ptr<Camera> cam;
  real current_t;
  int step_count;
  real dt = 3e-3_f, dx = 1.0_f / n, inv_dx = 1.0_f / dx;

  std::unique_ptr<ParticleRenderer> renderer;

  MGPCGSmoke() {
    renderer = create_instance_unique<ParticleRenderer>("shadow_map");
    auto radius = 1.0_f;

    Dict cam_dict;
    cam_dict.set("origin", Vector(0, radius * 0.3, radius))
        .set("look_at", Vector(0, 0, 0))
        .set("up", Vector(0, 1, 0))
        .set("fov", 70)
        .set("res", cam_res);
    cam = create_instance<Camera>("pinhole", cam_dict);

    Dict dict;
    dict.set("shadow_map_resolution", 0.005_f)
        .set("alpha", 0.6_f)
        .set("shadowing", 0.01_f)
        .set("ambient_light", 0.3_f)
        .set("light_direction", Vector(1, 2, 0.5));

    renderer->initialize(dict);
    renderer->set_camera(cam);
    current_t = 0;
    step_count = 0;
    // Span a region in
    if (!use_preconditioner) {
      mg_lv = 1;
    }
    grids.resize(mg_lv);
    for (int i = 0; i < mg_lv; i++) {
      grids[i] = std::make_unique<Grid>();
    }
    TC_ASSERT(mg_lv >= 1);
    TC_ASSERT_INFO(bit::is_power_of_two(n), "Only POT grid sizes supported");
    Region3D active_region(VectorI(-n, -n * 2, -n), VectorI(n, n * 2, n));
    for (auto ind : active_region) {
      if (grids[0]->inside(ind)) {
        grids[0]->touch(ind);
        // grids[0]->node(ind).flags().set_effective(true);
        grids[0]->get_block_if_exist(ind)->meta.set_has_effective_cell(true);
      }
    }
    if (use_preconditioner)
      set_up_hierechy();
    Region3D buffer_region(VectorI(-n, -n * 2, -n),
                           VectorI(n, n * 2, n) + VectorI(1));
    // Touch extra faces for u, v, w
    for (auto ind : buffer_region) {
      if (grids[0]->inside(ind))
        grids[0]->touch(ind);
    }
  }

  void test_pcg() {
    {
      TC_PROFILER("Initialize")
      Region3D active_region(VectorI(-n, -n * 2, -n), VectorI(n, n * 2, n));
      for (auto &ind : active_region) {
        if (ind.get_ipos() == VectorI(0)) {
          grids[0]->node(ind.get_ipos())[CH_B] = 1;
        }
      }
    }
    poisson_solve();
  }

  void set_up_hierechy() {
    std::size_t total_blocks = grids[0]->num_active_blocks();
    for (int i = 0; i < mg_lv - 1; i++) {
      grids[i]->coarsen_to(*grids[i + 1],
                           [&](Block &b, Grid::PyramidAncestors &an) {
                             for (auto ind : b.local_region()) {
                               // b.node_local(ind.get_ipos()).flags().set_effective(true);
                             }
                             b.meta.set_has_effective_cell(true);
                           });
      total_blocks /= 8;
      TC_ASSERT(grids[i + 1]->num_active_blocks() == total_blocks);
    }
  }

  void residual(int level, int U, int B, int R) {
    TC_PROFILER("residual");
    grids[level]->advance(
        [&](Block &b, Grid::Ancestors &an) {
          if (!b.meta.get_has_effective_cell())
            return;
          GridScratchPadCh scratchU(an, sizeof(real) * U);
          GridScratchPadCh scratchB(an, sizeof(real) * B);
          constexpr int ChU = 0, ChB = 1;
          using namespace stencilang;
          // clang-format off
          auto sum =
              (input<ChU, Offset<0, 0, 1>> + input<ChU, Offset<0, 0, -1>>) +
              (input<ChU, Offset<0, 1, 0>> + input<ChU, Offset<0, -1, 0>>) +
              (input<ChU, Offset<1, 0, 0>> + input<ChU, Offset<-1, 0, 0>>);
          auto out = input<ChB, Offset<0, 0, 0>> + sum -
              ratio<6, 1> * input<ChU, Offset<0, 0, 0>>;
          // clang-format on
          map_block(b, R, out, scratchU, scratchB);
        },
        false, false, false, true);
  }

  void multiply(int channel_out, int channel_in) {
    TC_PROFILER("multiply");
    // TODO: this supports zero-Dirichlet BC only!
    grids[0]->advance(
        [&](Block &b, Grid::Ancestors &an) {
          if (!b.meta.get_has_effective_cell())
            return;
          GridScratchPadCh scratch(an, sizeof(real) * channel_in);
          constexpr int ChU = 0;
          using namespace stencilang;
          // clang-format off
          auto sum =
              (input<ChU, Offset<0, 0, 1>> + input<ChU, Offset<0, 0, -1>>) +
              (input<ChU, Offset<0, 1, 0>> + input<ChU, Offset<0, -1, 0>>) +
              (input<ChU, Offset<1, 0, 0>> + input<ChU, Offset<-1, 0, 0>>);
          auto out = ratio<6, 1> * input<ChU, Offset<0, 0, 0>> - sum;
          // clang-format on
          map_block(b, channel_out, out, scratch);
        },
        false, false, false, true);
  }

  // out += a + scale * b
  void saxpy(int channel_out, int channel_a, int channel_b, real scale) {
    grids[0]->map([&](Block &b) {
      for (int i = 0; i < b.num_nodes; i++) {
        b.nodes[channel_out][i] =
            b.nodes[channel_a][i] + scale * b.nodes[channel_b][i];
      }
    });
  }

  // out += a + scale * b
  void copy(int channel_out, int channel_a) {
    TC_PROFILER("copy")
    grids[0]->map([&](Block &b) {
      for (int i = 0; i < b.num_nodes; i++) {
        b.nodes[channel_out][i] = b.nodes[channel_a][i];
      }
    });
  }

  float64 dot_product(int channel_a, int channel_b) {
    return grids[0]->reduce([&](Block &b) -> float64 {
      float64 sum = 0;
      for (int i = 0; i < b.num_nodes; i++) {
        sum += b.nodes[channel_a][i] * b.nodes[channel_b][i];
      }
      return sum;
    });
  }

  void smooth(int level, int U, int Uout, int B) {
    TC_STATIC_ASSERT(4 % smoothing_fusion == 0);
    TC_PROFILER("smoothing")
    // TODO: this supports zero-Dirichlet BC only!
    grids[level]->advance(
        [&](Grid::Block &b, Grid::Ancestors &an) {
          if (!b.meta.get_has_effective_cell())
            return;

          using Scratch = TGridScratchPad<Block, real, smoothing_fusion>;

          Scratch scratchB(an, B * sizeof(real));
          Scratch scratchU(an, U * sizeof(real));
          Scratch scratchV;
          Scratch scratchM;  // mask
          scratchM.set_as_mask(an);

          TC_STATIC_ASSERT(sizeof(real) == 4);
          TC_STATIC_ASSERT(Block::size[2] == 8);

          using namespace stencilang;
          constexpr int ChU = 0;
          constexpr int ChB = 1;
          constexpr int ChM = 2;

          // clang-format off
          auto sum =
              (input<ChU, Offset<0, 0, 1>> + input<ChU, Offset<0, 0, -1>>) +
              (input<ChU, Offset<0, 1, 0>> + input<ChU, Offset<0, -1, 0>>) +
              (input<ChU, Offset<1, 0, 0>> + input<ChU, Offset<-1, 0, 0>>) +
              input<ChB>;
          auto jacobi = sum * ratio<1, 6>;
          auto original = input<ChU>;
          auto damped_jacobi = (original + ratio<2, 3> * (jacobi - original));
          auto damped_jacobi_mask = damped_jacobi * input<ChM>;

          // clang-format on
          if (smoothing_fusion == 1) {
            map_block(b, U, damped_jacobi, scratchU, scratchB);
          } else {
            if (smoothing_fusion >= 4) {
            }

            if (smoothing_fusion >= 2) {
              map(scratchV, damped_jacobi_mask,
                  Region3D(Vector3i(-1), Vector3i(Block::size) + Vector3i(1)),
                  scratchU, scratchB, scratchM);
            }

            map_block(b, Uout, damped_jacobi, scratchV, scratchB);
          }
        },
        false, level == 0, true,
        true);  // carry particles only if on finest level
  }

  void clear(int level, int channel) {
    grids[level]->for_each_node(
        [&](const Block::NodeAccessorType &n) { n[channel] = 0; });
  }

  // B[level + 1] = coarsened(R[level])
  void restrict(int level, int R_in, int B_out) {
    TC_PROFILER("restriction");
    // NOTE: supports POT grids only
    // sum residual
    clear(level + 1, B_out);
    grids[level]->coarsen_to(
        *grids[level + 1], [&](Block &block, Grid::PyramidAncestors &an) {
          if (!block.meta.get_has_effective_cell())
            return;
          for (auto ind : Region3D(Vector3i(0), Vector3i(2))) {
            if (!an[ind.get_ipos()]) {
              TC_NOT_IMPLEMENTED
              continue;
            }
            Block &ab = *an[ind.get_ipos()];
            for (auto j : ab.local_region()) {
              auto coarse_coord = div_floor(
                  ind.get_ipos() * Vector3i(Block::size) + j.get_ipos(),
                  Vector3i(2));
              block.node_local(coarse_coord)[B_out] +=
                  ab.node_local(j.get_ipos())[R_in];
            }
          }
        });
  }

  // U[level] += refined(U]level + 1]);
  void prolongate(int level, int U) {
    TC_PROFILER("prolongation");
    real scale = 0.5_f;
    // upsample and apply correction
    grids[level]->refine_from(
        *grids[level + 1], [&](Block &block, Block &ancestor) {
          if (!block.meta.get_has_effective_cell())
            return;
          for (auto ind : block.global_region()) {
            auto correction =
                scale *
                ancestor.node_global(div_floor(ind.get_ipos(), Vector3i(2)))[U];
            block.node_global(ind.get_ipos())[U] += correction;
          }
        });
  }

  real norm(int channel) {
    return (real)std::sqrt(dot_product(channel, channel));
  }

  void V_cycle(int channel_in,
               int channel_out,
               bool use_as_preconditioner = true) {
    copy(CH_MG_B, channel_in);
    constexpr int U = CH_MG_U, V = CH_MG_V, B = CH_MG_B, R = CH_MG_R;
    constexpr int smoothing_iters = 8 / smoothing_fusion,
                  bottom_smoothing_iter = 120 / smoothing_fusion;
    TC_STATIC_ASSERT(smoothing_iters % 2 == 0);
    TC_STATIC_ASSERT(bottom_smoothing_iter % 2 == 0);
    if (use_as_preconditioner) {
      clear(0, U);
    }
    for (int i = 0; i < mg_lv - 1; i++) {
      // pre-smoothing
      if (true) {
        for (int j = 0; j < smoothing_iters / 2; j++) {
          smooth(i, U, V, B);
          smooth(i, V, U, B);
        }
      } else {
        for (;;) {
          TC_TIME(smooth(i, U, V, B));
        }
      }
      residual(i, U, B, R);
      restrict(i, R, B);
      clear(i + 1, U);
    }

    {
      TC_PROFILER("bottom smoothing");
      for (int j = 0; j < bottom_smoothing_iter / 2; j++) {
        smooth(mg_lv - 1, U, V, B);
        smooth(mg_lv - 1, V, U, B);
      }
    }

    for (int i = mg_lv - 2; i >= 0; i--) {
      prolongate(i, U);
      // post-smoothing
      for (int j = 0; j < smoothing_iters / 2; j++) {
        smooth(i, U, V, B);
        smooth(i, V, U, B);
      }
    }
    copy(channel_out, CH_MG_U);
  }

  void run() {
    for (int i = 0; i < 1000000; i++) {
      V_cycle(CH_B, CH_X, false);
      residual(0, CH_X, CH_B, CH_R);
      auto residual_l2 = norm(CH_R);
      TC_TRACE("iter {}, residual {}", i, residual_l2);
      if (residual_l2 < 1e-7) {
        break;
      }
    }
  }

  // https://en.wikipedia.org/wiki/Conjugate_gradient_method
  void poisson_solve() {
    TC_PROFILER("Poisson Solve");
    constexpr real tolerance = 1e-3_f;
    real initial_residual_norm = norm(CH_B);
    TC_P(initial_residual_norm);
    if (initial_residual_norm < 1e-30_f) {
      return;
    }
    clear(0, CH_X);
    // r = b - Ax
    residual(0, CH_X, CH_B, CH_R);
    // z = M^-1 r
    if (use_preconditioner) {
      V_cycle(CH_R, CH_Z);
    } else {
      copy(CH_Z, CH_R);
    }
    // p = z
    copy(CH_P, CH_Z);
    auto old_zr = dot_product(CH_Z, CH_R);
    for (int i = 0; i < 100000; i++) {
      TC_PROFILE("Multiply", multiply(CH_Z, CH_P));
      real alpha = old_zr / dot_product(CH_P, CH_Z);

      saxpy(CH_X, CH_X, CH_P, alpha);
      saxpy(CH_R, CH_R, CH_Z, -alpha);

      auto residual_l2 = norm(CH_R);
      TC_TRACE("iter {}, residual {}", i, residual_l2);
      if (residual_l2 < tolerance * initial_residual_norm) {
        break;
      }

      if (use_preconditioner) {
        TC_PROFILE("V_cycle", V_cycle(CH_R, CH_Z));
      } else {
        copy(CH_Z, CH_R);
      }

      auto new_zr = dot_product(CH_Z, CH_R);
      auto beta = new_zr / old_zr;
      old_zr = new_zr;
      saxpy(CH_P, CH_Z, CH_P, beta);
    }
  }

  TC_FORCE_INLINE Vector3 storage_offset(int axis) {
    return axis < 3 ? (Vector3(1) - Vector3::axis(axis)) * 0.5_f
                    : Vector3(0.5_f);
  }

  void advect() {
    grids[0]->advance(
        [&](Block &b, Grid::Ancestors &an) {
          TC_STATIC_ASSERT(Block::size[0] == 8);
          auto scale = Vector(inv_dx);
          auto corner = b.base_coord.template cast<real>() -
                        VectorI(Block::size).template cast<real>();

          // Unfortunately, ux, uy, uz and density are not collocated...
          LerpField<real, TSize3D<24>> u(scale, corner + storage_offset(0));
          LerpField<real, TSize3D<24>> v(scale, corner + storage_offset(1));
          LerpField<real, TSize3D<24>> w(scale, corner + storage_offset(2));
          LerpField<real, TSize3D<24>> rho(scale, corner + storage_offset(3));
          LerpField<real, TSize3D<24>> T(scale, corner + storage_offset(3));

          for (auto ind : Region3D(VectorI(0), VectorI(24))) {
            auto ab = an[VectorI(ind) / VectorI(Block::size) - VectorI(1)];
            if (!ab) {
              u.node(ind) = 0;
              v.node(ind) = 0;
              w.node(ind) = 0;
              rho.node(ind) = 0;
              T.node(ind) = 0;
            } else {
              auto node = ab->node_local(VectorI(ind) % VectorI(Block::size));
              u.node(ind) = node[CH_VX];
              v.node(ind) = node[CH_VY];
              w.node(ind) = node[CH_VZ];
              rho.node(ind) = node[CH_RHO];
            }
          }

          //#define sample_velocity(pos) Vector3(u.sample(pos), v.sample(pos),
          // w.sample(pos))
          auto sample_velocity = [&](Vector3 pos) -> Vector3 {
            return Vector3(u.sample(pos), v.sample(pos), w.sample(pos));
          };
          auto backtrace = [&](Vector3 pos) {
            // RK2
            return pos -
                   dt * sample_velocity(pos -
                                        (dt * 0.5_f) * sample_velocity(pos));
            /*
            return pos - dt * sample_velocity(pos);
        */
          };
          auto offset = Vector3i(Block::size);
          for (auto ind : b.local_region()) {
            const auto &node = b.node_local(ind);
            node[CH_VX] = u.sample(backtrace(u.node_pos(ind + offset)));
            node[CH_VY] = v.sample(backtrace(v.node_pos(ind + offset)));
            node[CH_VZ] = w.sample(backtrace(w.node_pos(ind + offset)));
            node[CH_RHO] = rho.sample(backtrace(rho.node_pos(ind + offset)));
          }

          if (b.meta.get_has_effective_cell()) {
            Vector particle_range[] = {
                b.base_coord.template cast<real>() * dx,
                (b.base_coord + VectorI(Block::size)).template cast<real>() *
                    dx};
            // Gather and move particles
            for (auto ab : an.data) {
              if (ab) {
                for (std::size_t i = 0; i < ab->particle_count; i++) {
                  // Copy
                  auto p = ab->particles[i];
                  if (particle_range[0] < p.pos && p.pos < particle_range[1]) {
                    p.pos += sample_velocity(p.pos) * dt;
                    b.add_particle(p);
                  }
                }
              }
            }
          }
        },
        false);
  }

  void compute_b(bool debug = false) {
    grids[0]->advance(
        [&](Block &b, Grid::Ancestors &an) {
          GridScratchPadCh scratchX(an, sizeof(real) * CH_VX);
          GridScratchPadCh scratchY(an, sizeof(real) * CH_VY);
          GridScratchPadCh scratchZ(an, sizeof(real) * CH_VZ);
          for (auto &ind : b.local_region()) {
            auto center = VectorI(ind);
            auto div = 0.0_f;
            div += scratchX.node(center) -
                   scratchX.node(center + VectorI::axis(0));
            div += scratchY.node(center) -
                   scratchY.node(center + VectorI::axis(1));
            div += scratchZ.node(center) -
                   scratchZ.node(center + VectorI::axis(2));
            b.node_local(ind)[CH_B] = div;
            if (debug && std::abs(div) > 1e-5_f) {
              TC_P(b.base_coord + ind);
              TC_P(div);
            }
          }
        },
        false, true);
  }

  void project() {
    // Compute divergence
    compute_b();
    real before_projection = norm(CH_B);
    TC_P(before_projection);
    // Solve Poisson
    poisson_solve();
    // Apply pressure
    real scale = 1 + (step_count + 1) % 2 * (int)reflection;
    TC_P(scale);
    grids[0]->advance(
        [&](Block &b, Grid::Ancestors &an) {
          GridScratchPadCh scratch(an, sizeof(real) * CH_X);
          for (auto &ind : b.local_region()) {
            auto center = VectorI(ind);
            for (int k = 0; k < dim; k++) {
              b.node_local(ind)[CH_VX + k] -=
                  scale * (scratch.node(center) -
                           scratch.node(center - VectorI::axis(k)));
            }
          }

        },
        false, true);
    if (true) {
      compute_b(true);
      real after_projection = norm(CH_B);
      // if (after_projection > 1e-4_f) {
      TC_WARN("Before projection: {}", before_projection);
      TC_WARN("After projection: {}", after_projection);
      //}
    }
  }

  void enforce_boundary_condition() {
    grids[0]->map([&](Block &b) {
      if (current_t == 0) {
        for (auto &ind : b.local_region()) {
          b.node_local(ind)[CH_VX] = 0;
          b.node_local(ind)[CH_VY] = 0;
          b.node_local(ind)[CH_VZ] = 0;
          b.node_local(ind)[CH_RHO] = 0;
        }
      }
      if (b.base_coord == VectorI(0, -n * 2 + 8, 0)) {
        // Sample some particles
        if (per_block_particles) {
          for (int i = 0; i < 24e5 * dt; i++) {
            auto r = Vector::rand();
            if (length(r - Vector(0.5_f)) > 0.5) {
              continue;
            }
            Vector pos = (b.base_coord.template cast<real>() +
                          r * VectorI(Block::size).template cast<real>()) *
                         dx;
            pos[3] = current_t;
            b.add_particle(Particle{pos});
          }
        }
        // if (current_t == 0) {
        for (auto ind : b.local_region()) {
          b.node_local(ind)[CH_VX] = std::sin(current_t * 50);
          b.node_local(ind)[CH_VY] = 0;
          b.node_local(ind)[CH_VZ] = 0;
          b.node_local(ind)[CH_RHO] = 1;
        }
      }
      for (auto ind : b.local_region()) {
        b.node_local(ind)[CH_VY] += (b.node_local(ind)[CH_RHO] * buoyancy) * dt;
      }
    });
  }

  void render(Canvas &canvas) {
    auto res = canvas.img.get_res();
    Array2D<Vector3> image(Vector2i(res), Vector3(1) - Vector3(0.0_f));
    std::vector<RenderParticle> particles;
    auto raw_particles = grids[0]->gather_particles();
    static int counter = 0;
    write_to_binary_file(raw_particles,
                         fmt::format("{}/{:06d}.tcb", folder, counter));
    counter += 1;
    for (auto &p : grids[0]->gather_particles()) {
      auto t = p.pos[3];
      auto color = hsv2rgb(Vector(fract(t / 4) * 2, 0.7_f, 0.9_f));
      particles.push_back(
          RenderParticle(p.pos * Vector(0.16_f), Vector4(color, 1.0_f)));
    }
    // write_to_binary_file(particles, fmt::format("outputs/{:06d}.tcb",
    // counter));
    renderer->render(image, particles);
    for (auto &ind : image.get_region()) {
      canvas.img[ind] = Vector4(image[ind]);
    }
  }

  void step() {
    TC_PROFILE("BC", enforce_boundary_condition());
    TC_PROFILE("Advection", advect());
    TC_PROFILE("Project", project());
    current_t += dt;
    step_count += 1;
  }

  Array2D<Vector3> render_density_field() {
    Array2D<Vector3> img;
    img.initialize(Vector2i(n * 2, n * 4));
    grids[0]->for_each_block([&](Block &b) {
      if (b.base_coord.z != 0 || !b.meta.get_has_effective_cell()) {
        return;
      }
      for (int i = 0; i < b.size[0]; i++) {
        for (int j = 0; j < b.size[1]; j++) {
          auto node = b.node_local(Vector3i(i, j, 0));
          auto vel = Vector3(node[CH_RHO]);
          img[Vector2i(b.base_coord.x, b.base_coord.y) +
              Vector2i(n + i, n * 2 + j)] = vel * Vector3(1);
        }
      }
    });
    return img;
  }

  Array2D<Vector3> render_velocity_field() {
    Array2D<Vector3> img;
    img.initialize(Vector2i(n * 2, n * 4));
    grids[0]->for_each_block([&](Block &b) {
      if (b.base_coord.z != 0 || !b.meta.get_has_effective_cell()) {
        return;
      }
      for (int i = 0; i < b.size[0]; i++) {
        for (int j = 0; j < b.size[1]; j++) {
          auto node = b.node_local(Vector3i(i, j, 0));
          auto vel = Vector3(node[CH_VX], node[CH_VY], node[CH_VZ]);
          img[Vector2i(b.base_coord.x, b.base_coord.y) +
              Vector2i(n + i, n * 2 + j)] = vel * Vector3(1) + Vector3(0.5_f);
        }
      }
    });
    return img;
  }

  Array2D<Vector3> render_pressure_field() {
    Array2D<Vector3> img;
    img.initialize(Vector2i(n * 2, n * 4));
    grids[0]->for_each_block([&](Block &b) {
      if (b.base_coord.z != 0 || !b.meta.get_has_effective_cell()) {
        return;
      }
      for (int i = 0; i < b.size[0]; i++) {
        for (int j = 0; j < b.size[1]; j++) {
          auto node = b.node_local(Vector3i(i, j, 0));
          auto vel = Vector3(node[CH_X]);
          img[Vector2i(b.base_coord.x, b.base_coord.y) +
              Vector2i(n + i, n * 2 + j)] = vel * Vector3(1) + Vector3(0.5_f);
        }
      }
    });
    return img;
  }

  template <int dilation>
  void test_gather() {
    grids[0]->advance(
        [&](Grid::Block &b, Grid::Ancestors &an) {
          if (!b.meta.get_has_effective_cell())
            return;
          using Scratch = TGridScratchPad<Block, real, dilation>;
          Scratch scratchB(an, 0 * sizeof(real));
          trash(scratchB.linearized_data[zero]);
          Scratch scratchU(an, 1 * sizeof(real));
          trash(scratchU.linearized_data[zero]);
          // Scratch scratchV;
          Scratch scratchM;  // mask
          scratchM.set_as_mask(an);
        },
        false, false, false,
        true);  // carry particles only if on finest level
  }

  MGPCGSmoke(bool T) {
    // initialize as grid for benchmarking
    int mg_lv = 1;
    int n = 128;
    grids.resize(mg_lv);
    for (int i = 0; i < mg_lv; i++) {
      grids[i] = std::make_unique<Grid>();
    }
    TC_ASSERT(mg_lv >= 1);
    TC_ASSERT_INFO(bit::is_power_of_two(n), "Only POT grid sizes supported");
    Region3D active_region(VectorI(-n, -n * 2, -n), VectorI(n, n * 2, n));
    TC_INFO("# cells: {} M", 2.0_f * 2 * 4 * pow<3>(n) / pow<2>(1024));
    for (auto ind : active_region) {
      if (grids[0]->inside(ind))
        grids[0]->touch(ind);
    }
  }

  void benchmark_advance() {
    residual(0, CH_B, CH_B, CH_B);
  }

  void benchmark_map() {
    grids[0]->map([&](Grid::Block &b) {
      for (auto &ind : b.local_region()) {
        b.node_local(ind)[0] += 1;
      }
    });
  }

  void benchmark_reduce() {
    auto ret = grids[0]->reduce([&](Grid::Block &b) {
      real sum = 0;
      for (auto &ind : b.local_region()) {
        sum += b.node_local(ind)[0];
      }
      return sum;
    });
    TC_INFO("Sum = {}", ret);
  }
};

auto gather_scratch = [](const std::vector<std::string> &params) {
  // ThreadedTaskManager::TbbParallelismControl _(1);
  std::unique_ptr<MGPCGSmoke> mgpcg;
  mgpcg = std::make_unique<MGPCGSmoke>();
  while (true) {
    TC_PROFILE("0", mgpcg->test_gather<0>());
    /*
    TC_PROFILE("1", mgpcg->test_gather<1>());
    TC_PROFILE("2", mgpcg->test_gather<2>());
    TC_PROFILE("3", mgpcg->test_gather<3>());
    TC_PROFILE("4", mgpcg->test_gather<4>());
     */
    print_profile_info();
  }
};

TC_REGISTER_TASK(gather_scratch);

auto mgpcg = [](const std::vector<std::string> &params) {
  // ThreadedTaskManager::TbbParallelismControl _(1);
  std::unique_ptr<MGPCGSmoke> mgpcg;
  mgpcg = std::make_unique<MGPCGSmoke>();
  while (true) {
    TC_TIME(mgpcg->test_pcg());
    print_profile_info();
  }
};

TC_REGISTER_TASK(mgpcg);

auto benchmark_pangu = []() {
  std::unique_ptr<MGPCGSmoke> mgpcg;
  mgpcg = std::make_unique<MGPCGSmoke>(true);
  for (int i = 0;; i++) {
    TC_TIME(mgpcg->benchmark_map());
    TC_TIME(mgpcg->benchmark_reduce());
    TC_TIME(mgpcg->benchmark_advance());
    TC_INFO("# Active blocks {}", mgpcg->grids[0]->num_active_blocks());
    TC_INFO("Sum should be {}", (i + 1) * 32 * 1024 * 1024.0);
  }
};

TC_REGISTER_TASK(benchmark_pangu);

auto smoke = [](const std::vector<std::string> &params) {
  TC_PROFILER("smoke");
  // ThreadedTaskManager::TbbParallelismControl _(1);
  std::unique_ptr<MGPCGSmoke> smoke;
  smoke = std::make_unique<MGPCGSmoke>();
  GUI gui("MGPCG Smoke", cam_res);
  // GUI gui2("Velocity", 256, 512);
  // GUI gui3("Density", 256, 512);
  TC_ASSERT(!params.empty());
  auto folder = params[0];
  std::experimental::filesystem::create_directory(folder);
  smoke->folder = folder;

  for (int frame = 0;; frame++) {
    TC_PROFILE("step", smoke->step());
    TC_PROFILE("render", smoke->render(gui.get_canvas()));
    gui.update();
    // gui.get_canvas().img.write_as_image(fmt::format("tmp/{:05d}.png",
    // frame));
    {
      /*
      TC_PROFILER("debug");
      auto img = smoke->render_velocity_field();
      for (auto ind : gui2.get_canvas().img.get_region()) {
        gui2.get_canvas().img[ind] = Vector3(img[Vector2i(ind) / Vector2i(4)]);
      }
      gui2.update();
      img = smoke->render_density_field();
      for (auto ind : gui3.get_canvas().img.get_region()) {
        gui3.get_canvas().img[ind] = Vector3(img[Vector2i(ind) / Vector2i(4)]);
      }
      gui3.update();
      */
    }
    print_profile_info();
  }
};

TC_REGISTER_TASK(smoke);

auto render_smoke = [](const std::vector<std::string> &params) {
  TC_PROFILER("smoke");
  GUI gui("MGPCG Smoke", cam_res);

  TC_ASSERT(!params.empty());
  auto folder = params[0];

  auto renderer = create_instance_unique<ParticleRenderer>("shadow_map");
  auto radius = 1.0_f;

  Dict cam_dict;
  cam_dict.set("origin", Vector3(0, radius * 0.3_f, radius * 1_f))
      .set("look_at", Vector3(0, 0, 0))
      .set("up", Vector3(0, 1, 0))
      .set("fov", 70)
      .set("res", cam_res);
  auto cam = create_instance<Camera>("pinhole", cam_dict);

  Dict dict;
  dict.set("shadow_map_resolution", 0.005_f)
      .set("alpha", 0.3_f)
      .set("shadowing", 0.0008_f)
      .set("ambient_light", 0.2_f)
      .set("light_direction", Vector3(1, 0.5, 0.5));

  renderer->initialize(dict);
  renderer->set_camera(cam);

  auto &canvas = gui.get_canvas();
  for (int frame = 1;; frame++) {
    auto res = canvas.img.get_res();
    Array2D<Vector3> image(Vector2i(res), Vector3(1) - Vector3(0.0_f));
    std::vector<Particle> particles;
    read_from_binary_file(particles,
                          fmt::format("{}/{:06d}.tcb", folder, frame));
    std::vector<RenderParticle> render_particles;
    for (auto &p : particles) {
      auto color = hsv2rgb(Vector3(fract(p.pos[3] / 4) * 2, 0.7_f, 0.9_f));
      render_particles.push_back(
          RenderParticle(p.pos * Vector3(0.16_f), Vector4(color, 1.0_f)));
    }

    renderer->render(image, render_particles);
    for (auto &ind : image.get_region()) {
      canvas.img[ind] = Vector4(image[ind]);
    }
    gui.update();
    auto render_dir = fmt::format("{}_rendered", folder);
    std::experimental::filesystem::create_directory(render_dir);
    gui.get_canvas().img.write_as_image(
        fmt::format("{}/{:05d}.png", render_dir, frame));
  }
};

TC_REGISTER_TASK(render_smoke)

TC_NAMESPACE_END

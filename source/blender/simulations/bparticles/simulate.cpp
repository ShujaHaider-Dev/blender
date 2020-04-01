
#include "BLI_timeit.h"
#include "BLI_array_cxx.h"
#include "BLI_vector_adaptor.h"
#include "BLI_parallel.h"

#include "BKE_collision.h"
#include "BKE_modifier.h"

#include "FN_cpp_type.h"

#include "simulate.hpp"

namespace BParticles {

using BLI::ScopedVector;
using BLI::Vector;
using BLI::VectorAdaptor;
using FN::CPPType;

static bool point_inside_tri(const float mat[3][3],
                             const float point[3],
                             std::array<float3, 3> &cur_tri_points)
{
  float mat_coords[3][2];
  float mat_new_pos[2];
  mul_v2_m3v3(mat_new_pos, mat, point);

  for (int i = 0; i < 3; i++) {
    mul_v2_m3v3(mat_coords[i], mat, cur_tri_points[i]);
  }

  return isect_point_tri_v2(mat_new_pos, mat_coords[0], mat_coords[1], mat_coords[2]);
}

/*
axis_dominant_v3_to_m3(mat, new_norm);

*/

/************************************************
 * Collisions (taken from the old particle_system.c)
 *
 * The algorithm is roughly:
 *  1. Use a BVH tree to search for faces that a particle may collide with.
 *  2. Use Newton's method to find the exact time at which the collision occurs.
 *     https://en.wikipedia.org/wiki/Newton's_method
 *
 ************************************************/
#define COLLISION_MIN_RADIUS 0.001f     // TODO check if this is needed
#define COLLISION_MIN_DISTANCE 0.0001f  // TODO check if this is needed
#define COLLISION_ZERO 0.00001f
static float nr_signed_distance_to_plane(float3 &p,
                                         std::array<float3, 3> &cur_tri_points,
                                         float3 &nor)
{
  float3 p0, e1, e2;
  float d;

  sub_v3_v3v3(e1, cur_tri_points[1], cur_tri_points[0]);
  sub_v3_v3v3(e2, cur_tri_points[2], cur_tri_points[0]);
  sub_v3_v3v3(p0, p, cur_tri_points[0]);

  cross_v3_v3v3(nor, e1, e2);
  normalize_v3(nor);

  d = dot_v3v3(p0, nor);

  return d;
}

static void collision_interpolate_element(std::array<std::pair<float3, float3>, 3> &tri_points,
                                          std::array<float3, 3> &cur_tri_points,
                                          float t)
{
  for (int i = 0; i < tri_points.size(); i++) {
    cur_tri_points[i] = float3::interpolate(tri_points[i].first, tri_points[i].second, t);
  }
}

/* find first root in range [0-1] starting from 0 */
static float collision_newton_rhapson(std::pair<float3, float3> &particle_points,
                                      std::array<std::pair<float3, float3>, 3> &tri_points,
                                      float radius,
                                      float3 &coll_normal)
{
  std::array<float3, 3> cur_tri_points;
  float t0, t1, dt_init, d0, d1, dd;
  float3 p;

  dt_init = 0.001f;
  /* start from the beginning */
  t0 = 0.f;
  collision_interpolate_element(tri_points, cur_tri_points, t0);
  d0 = nr_signed_distance_to_plane(particle_points.first, cur_tri_points, coll_normal);
  t1 = dt_init;
  d1 = 0.f;

  for (int iter = 0; iter < 10; iter++) {  //, itersum++) {
    /* get current location */
    collision_interpolate_element(tri_points, cur_tri_points, t1);
    p = float3::interpolate(particle_points.first, particle_points.second, t1);

    d1 = nr_signed_distance_to_plane(p, cur_tri_points, coll_normal);

    // TODO add radius check
    // if (std::signbit(d0) != std::signbit(d1)) {
    // XXX Just a test return for now
    //  return 1.0f;
    //}

    /* particle already inside face, so report collision */
    if (iter == 0 && d0 < 0.f && d0 > -radius) {
      p = particle_points.first;
      // pce->inside = 1;
      return 0.f;
    }

    /* Zero gradient (no movement relative to element). Can't step from
     * here. */
    if (d1 == d0) {
      /* If first iteration, try from other end where the gradient may be
       * greater. Note: code duplicated below. */
      if (iter == 0) {
        t0 = 1.f;
        collision_interpolate_element(tri_points, cur_tri_points, t0);
        d0 = nr_signed_distance_to_plane(particle_points.second, cur_tri_points, coll_normal);
        t1 = 1.0f - dt_init;
        d1 = 0.f;
        continue;
      }
      else {
        return -1.f;
      }
    }

    dd = (t1 - t0) / (d1 - d0);

    t0 = t1;
    d0 = d1;

    t1 -= d1 * dd;

    /* Particle moving away from plane could also mean a strangely rotating
     * face, so check from end. Note: code duplicated above. */
    if (iter == 0 && t1 < 0.f) {
      t0 = 1.f;
      collision_interpolate_element(tri_points, cur_tri_points, t0);
      d0 = nr_signed_distance_to_plane(particle_points.second, cur_tri_points, coll_normal);
      t1 = 1.0f - dt_init;
      d1 = 0.f;
      continue;
    }
    else if (iter == 1 && (t1 < -COLLISION_ZERO || t1 > 1.f)) {
      return -1.f;
    }

    if (d1 <= COLLISION_ZERO && d1 >= -COLLISION_ZERO) {
      if (t1 >= -COLLISION_ZERO && t1 <= 1.f) {
        /* Do we actually lie inside the triangle? */
        float mat[3][3];
        axis_dominant_v3_to_m3(mat, coll_normal);
        if (point_inside_tri(mat, p, cur_tri_points)) {
          // if (isect_point_tri_prism_v3(p, cur_tri_points[0], cur_tri_points[1],
          // cur_tri_points[2])) { // This is a bit slower than point_inside_tri
          CLAMP(t1, 0.f, 1.f);
          return t1;
        }
        else {
          return -1.f;
        }
      }
      else {
        return -1.f;
      }
    }
  }
  return -1.0;
}

typedef struct RayCastData {
  std::pair<float3, float3> particle_points;
  CollisionModifierData *collmd;
} RayCastData;

BLI_NOINLINE static void raycast_callback(void *userdata,
                                          int index,
                                          const BVHTreeRay *ray,
                                          BVHTreeRayHit *hit)
{
  RayCastData *rd = (RayCastData *)userdata;
  CollisionModifierData *collmd = rd->collmd;

  const MVertTri *vt = &collmd->tri[index];
  MVert *verts = collmd->x;
  const float *v0, *v1, *v2;
  float dist;

  v0 = verts[vt->tri[0]].co;
  v1 = verts[vt->tri[1]].co;
  v2 = verts[vt->tri[2]].co;

  // TODO implement triangle collision width
  // use width + vertex normals to make the triangle thick

  if (collmd->is_static) {

    if (ray->radius == 0.0f) {
      dist = bvhtree_ray_tri_intersection(ray, hit->dist, v0, v1, v2);
    }
    else {
      dist = bvhtree_sphereray_tri_intersection(ray, ray->radius, hit->dist, v0, v1, v2);
    }

    if (dist >= 0.0f && dist < hit->dist) {
      hit->index = index;
      hit->dist = dist;
      madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist);

      normal_tri_v3(hit->no, v0, v1, v2);
    }
    return;
  }

  MVert *new_verts = collmd->xnew;
  const float *v0_new, *v1_new, *v2_new;
  v0_new = new_verts[vt->tri[0]].co;
  v1_new = new_verts[vt->tri[1]].co;
  v2_new = new_verts[vt->tri[2]].co;

  std::array<std::pair<float3, float3>, 3> tri_points;
  float3 coll_normal;

  tri_points[0] = std::pair<float3, float3>(v0, v0_new);
  tri_points[1] = std::pair<float3, float3>(v1, v1_new);
  tri_points[2] = std::pair<float3, float3>(v2, v2_new);

  // Check if we get hit by the moving object
  float coll_time = collision_newton_rhapson(
      rd->particle_points, tri_points, ray->radius, coll_normal);

  dist = float3::distance(rd->particle_points.first, rd->particle_points.second) * coll_time;

  if (coll_time >= 0.f) {
    if (hit->index != -1 && dist >= 0.0f && dist >= hit->dist) {
      /* We have already collided with and other object at closer distance */
      // TODO we should actually not just flat out look at the distance here...
      // we should take the collision with the smallest coll_time for this to be correct
      return;
    }
    // We have a collision!
    hit->index = index;
    hit->dist = dist;
    madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, hit->dist);
    zero_v3(hit->co);
    copy_v3_v3(hit->no, coll_normal);
  }
}

BLI_NOINLINE static void simulate_particle_chunk(SimulationState &UNUSED(simulation_state),
                                                 ParticleAllocator &UNUSED(particle_allocator),
                                                 MutableAttributesRef attributes,
                                                 ParticleSystemInfo &system_info,
                                                 MutableArrayRef<float> remaining_durations,
                                                 float UNUSED(end_time),
                                                 Vector<ColliderCache *> &colliders)
{
  uint amount = attributes.size();
  BLI_assert(amount == remaining_durations.size());

  BufferCache buffer_cache;

  Array<float3> forces(attributes.size(), {0, 0, 0});
  for (Force *force : system_info.forces) {
    force->add_force(attributes, IndexRange(amount), buffer_cache, forces);
  }

  MutableArrayRef<float3> velocities = attributes.get<float3>("Velocity");
  MutableArrayRef<float3> positions = attributes.get<float3>("Position");

  // system_info.collision_objects
  // simulation_state.m_depsgraph;
  // cloth_bvh_collision

  for (uint pindex : IndexRange(amount)) {
    float mass = 1.0f;
    float duration = remaining_durations[pindex];
    bool collided = false;

    // Update the velocities here so that the potential distance traveled is correct in the
    // collision check.
    velocities[pindex] += duration * forces[pindex] / mass;

    // Check if any 'collobjs' collide with the particles here
    if (colliders.size() != 0) {
      // TODO Move this to be upper most loop
      for (auto col : colliders) {
        CollisionModifierData *collmd = col->collmd;

        if (!collmd->bvhtree) {
          continue;
        }

        const int raycast_flag = BVH_RAYCAST_DEFAULT;

        float max_move = (duration * velocities[pindex]).length();

        BVHTreeRayHit hit;
        hit.index = -1;
        hit.dist = max_move;
        float particle_radius = 0.0f;
        float3 start = positions[pindex];
        float3 dir = velocities[pindex].normalized();

        RayCastData rd;

        rd.collmd = collmd;
        rd.particle_points.first = start;
        rd.particle_points.second = start + duration * velocities[pindex];

        BLI_bvhtree_ray_cast_ex(collmd->bvhtree,
                                start,
                                dir,
                                particle_radius,
                                &hit,
                                raycast_callback,
                                &rd,
                                raycast_flag);

        if (hit.index == -1) {
          // We didn't hit anything
          continue;
        }
        // TODO move particle to collision point and do additional collision check in the new
        // direction.
        // update final position of particle
        positions[pindex] = hit.co;
        //
        // dot normal from vt with hit.co - start to see which way to deflect the particle
        velocities[pindex] *= -1;
        positions[pindex] += duration * (1.0f - hit.dist / max_move) * velocities[pindex];
        collided = true;
        break;
      }
    }
    if (!collided) {
      positions[pindex] += duration * velocities[pindex];
    }
  }
}  // namespace BParticles

BLI_NOINLINE static void delete_tagged_particles_and_reorder(ParticleSet &particles)
{
  auto kill_states = particles.attributes().get<bool>("Dead");
  ScopedVector<uint> indices_to_delete;

  for (uint i : kill_states.index_range()) {
    if (kill_states[i]) {
      indices_to_delete.append(i);
    }
  }

  particles.destruct_and_reorder(indices_to_delete.as_ref());
}

BLI_NOINLINE static void simulate_particles_for_time_span(SimulationState &simulation_state,
                                                          ParticleAllocator &particle_allocator,
                                                          ParticleSystemInfo &system_info,
                                                          FloatInterval time_span,
                                                          MutableAttributesRef particle_attributes)
{
  // TODO check if we acutally have a collision node and take settings from that
  ListBase *coll_list = BKE_collider_cache_create(simulation_state.m_depsgraph, NULL, NULL);

  // Convert list to vector for speed, easier debugging, and type safety
  Vector<ColliderCache *> colliders(*coll_list, true);

  BLI::blocked_parallel_for(IndexRange(particle_attributes.size()), 1000, [&](IndexRange range) {
    Array<float> remaining_durations(range.size(), time_span.size());
    simulate_particle_chunk(simulation_state,
                            particle_allocator,
                            particle_attributes.slice(range),
                            system_info,
                            remaining_durations,
                            time_span.end(),
                            colliders);
  });

  BKE_collider_cache_free(&coll_list);
}

BLI_NOINLINE static void simulate_particles_from_birth_to_end_of_step(
    SimulationState &simulation_state,
    ParticleAllocator &particle_allocator,
    ParticleSystemInfo &system_info,
    float end_time,
    MutableAttributesRef particle_attributes)
{
  ArrayRef<float> all_birth_times = particle_attributes.get<float>("Birth Time");

  // TODO check if we acutally have a collision node and take settings from that
  ListBase *coll_list = BKE_collider_cache_create(simulation_state.m_depsgraph, NULL, NULL);

  // Convert list to vector for speed, easier debugging, and type safety
  Vector<ColliderCache *> colliders(*coll_list, true);

  BLI::blocked_parallel_for(IndexRange(particle_attributes.size()), 1000, [&](IndexRange range) {
    ArrayRef<float> birth_times = all_birth_times.slice(range);

    Array<float> remaining_durations(range.size());
    for (uint i : remaining_durations.index_range()) {
      remaining_durations[i] = end_time - birth_times[i];
    }

    simulate_particle_chunk(simulation_state,
                            particle_allocator,
                            particle_attributes.slice(range),
                            system_info,
                            remaining_durations,
                            end_time,
                            colliders);
  });
  BKE_collider_cache_free(&coll_list);
}

BLI_NOINLINE static void simulate_existing_particles(
    SimulationState &simulation_state,
    ParticleAllocator &particle_allocator,
    StringMap<ParticleSystemInfo> &systems_to_simulate)
{
  FloatInterval simulation_time_span = simulation_state.time().current_update_time();

  BLI::parallel_map_items(simulation_state.particles().particle_containers(),
                          [&](StringRef system_name, ParticleSet *particle_set) {
                            ParticleSystemInfo *system_info = systems_to_simulate.lookup_ptr(
                                system_name);
                            if (system_info == nullptr) {
                              return;
                            }

                            simulate_particles_for_time_span(simulation_state,
                                                             particle_allocator,
                                                             *system_info,
                                                             simulation_time_span,
                                                             particle_set->attributes());
                          });
}

BLI_NOINLINE static void create_particles_from_emitters(SimulationState &simulation_state,
                                                        ParticleAllocator &particle_allocator,
                                                        ArrayRef<Emitter *> emitters,
                                                        FloatInterval time_span)
{
  BLI::parallel_for(emitters.index_range(), [&](uint emitter_index) {
    Emitter &emitter = *emitters[emitter_index];
    EmitterInterface interface(simulation_state, particle_allocator, time_span);
    emitter.emit(interface);
  });
}

void simulate_particles(SimulationState &simulation_state,
                        ArrayRef<Emitter *> emitters,
                        StringMap<ParticleSystemInfo> &systems_to_simulate)
{
  // SCOPED_TIMER(__func__);

  // systems_to_simulate.foreach_item([](StringRef name, ParticleSystemInfo &system_info) {
  //  system_info.collision_objects.print_as_lines(
  //      name, [](const CollisionObject &collision_object) {
  //        std::cout << collision_object.object->id.name
  //                  << " - Damping: " << collision_object.damping << " - Location Old: "
  //                  << float3(collision_object.local_to_world_start.values[3])
  //                  << " - Location New: "
  //                  << float3(collision_object.local_to_world_end.values[3]);
  //      });
  //});

  ParticlesState &particles_state = simulation_state.particles();
  FloatInterval simulation_time_span = simulation_state.time().current_update_time();

  StringMultiMap<ParticleSet *> all_newly_created_particles;
  StringMultiMap<ParticleSet *> newly_created_particles;
  {
    ParticleAllocator particle_allocator(particles_state);
    BLI::parallel_invoke(
        [&]() {
          simulate_existing_particles(simulation_state, particle_allocator, systems_to_simulate);
        },
        [&]() {
          create_particles_from_emitters(
              simulation_state, particle_allocator, emitters, simulation_time_span);
        });

    newly_created_particles = particle_allocator.allocated_particles();
    all_newly_created_particles = newly_created_particles;
  }

  while (newly_created_particles.key_amount() > 0) {
    ParticleAllocator particle_allocator(particles_state);

    BLI::parallel_map_items(
        newly_created_particles, [&](StringRef name, ArrayRef<ParticleSet *> new_particle_sets) {
          ParticleSystemInfo *system_info = systems_to_simulate.lookup_ptr(name);
          if (system_info == nullptr) {
            return;
          }

          BLI::parallel_for(new_particle_sets.index_range(), [&](uint index) {
            ParticleSet &particle_set = *new_particle_sets[index];
            simulate_particles_from_birth_to_end_of_step(simulation_state,
                                                         particle_allocator,
                                                         *system_info,
                                                         simulation_time_span.end(),
                                                         particle_set.attributes());
          });
        });

    newly_created_particles = particle_allocator.allocated_particles();
    all_newly_created_particles.add_multiple(newly_created_particles);
  }

  BLI::parallel_map_items(all_newly_created_particles,
                          [&](StringRef name, ArrayRef<ParticleSet *> new_particle_sets) {
                            ParticleSet &main_set = particles_state.particle_container(name);

                            for (ParticleSet *set : new_particle_sets) {
                              main_set.add_particles(*set);
                              delete set;
                            }
                          });

  BLI::parallel_map_keys(systems_to_simulate, [&](StringRef name) {
    ParticleSet &particles = particles_state.particle_container(name);
    delete_tagged_particles_and_reorder(particles);
  });
}

}  // namespace BParticles
/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

/* connect the given light sample with the shading point and calculate its
 * contribution and accumulate it to L */
ccl_device void accum_light_contribution(KernelGlobals *kg,
                                         ShaderData *sd,
                                         ShaderData *emission_sd,
                                         LightSample *ls,
                                         ccl_addr_space PathState *state,
                                         Ray *light_ray,
                                         BsdfEval *L_light,
                                         PathRadiance *L,
                                         bool *is_lamp,
                                         float terminate,
                                         float3 throughput,
                                         float scale)
{
  if (direct_emission(kg, sd, emission_sd, ls, state, light_ray, L_light, is_lamp, terminate)) {
    /* trace shadow ray */
    float3 shadow;

    if (!shadow_blocked(kg, sd, emission_sd, state, light_ray, &shadow)) {
      /* accumulate */
      path_radiance_accum_light(L, state, throughput * scale, L_light, shadow, scale, *is_lamp);
    }
    else {
      path_radiance_accum_total_light(L, state, throughput * scale, L_light);
    }
  }
}

/* The accum_light_tree_contribution() function does the following:
 * 1. Recursive tree traversal using splitting. This picks one or more lights.
 * 2. For each picked light, a position on the light is also chosen.
 * 3. The total contribution of all these light samples are evaluated and
 *    accumulated to L. */
ccl_device void accum_light_tree_contribution(KernelGlobals *kg,
                                              float randu,
                                              float randv,
                                              int offset,
                                              float pdf_factor,
                                              bool can_split,
                                              float3 throughput,
                                              float scale_factor,
                                              PathRadiance *L,
                                              ccl_addr_space PathState *state,
                                              ShaderData *sd,
                                              ShaderData *emission_sd)
{
  float3 P = sd->P_pick;
  float3 N = sd->N_pick;

  float time = sd->time;
  int bounce = state->bounce;

  /* read in first part of node of light tree */
  int right_child_offset, distribution_id, num_emitters;
  update_node(kg, offset, &right_child_offset, &distribution_id, &num_emitters);

  /* found a leaf */
  if (right_child_offset == -1) {

    /* if there are several emitters in this leaf then pick one of them */
    if (num_emitters > 1) {

      /* create and sample CDF without dynamic allocation.
       * see comment in light_tree_sample() for this piece of code */
      float sum = 0.0f;
      for (int i = 0; i < num_emitters; ++i) {
        sum += calc_light_importance(kg, P, N, offset, i);
      }

      if (sum == 0.0f) {
        return;
      }

      float sum_inv = 1.0f / sum;
      float cdf_L = 0.0f;
      float cdf_R = 0.0f;
      float prob = 0.0f;
      int light;
      for (int i = 1; i < num_emitters + 1; ++i) {
        prob = calc_light_importance(kg, P, N, offset, i - 1) * sum_inv;
        cdf_R = cdf_L + prob;
        if (randu < cdf_R) {
          light = i - 1;
          break;
        }

        cdf_L = cdf_R;
      }
      distribution_id += light;
      pdf_factor *= prob;

      /* rescale random number */
      randu = (randu - cdf_L) / (cdf_R - cdf_L);
    }

    /* pick a point on the chosen light(distribution_id) and calculate the
     * probability of picking this point */
    LightSample ls;
    light_point_sample(kg, randu, randv, time, P, bounce, distribution_id, &ls);

    /* combine pdfs */
    ls.pdf *= pdf_factor;

    if (ls.pdf <= 0.0f)
      return;

    /* compute and accumulate the total contribution of this light */
    Ray light_ray;
    BsdfEval L_light;
    bool is_lamp;
    float terminate = path_state_rng_light_termination(kg, state);
    accum_light_contribution(kg,
                             sd,
                             emission_sd,
                             &ls,
                             state,
                             &light_ray,
                             &L_light,
                             L,
                             &is_lamp,
                             terminate,
                             throughput,
                             scale_factor);

    return;
  }
  else {  // Interior node, choose which child(ren) to go down

    int child_offsetL = offset + 4;
    int child_offsetR = 4 * right_child_offset;

    /* choose whether to go down both(split) or only one of the children */
    if (can_split && split(kg, P, offset)) {
      /* go down both child nodes */
      accum_light_tree_contribution(kg,
                                    randu,
                                    randv,
                                    child_offsetL,
                                    pdf_factor,
                                    true,
                                    throughput,
                                    scale_factor,
                                    L,
                                    state,
                                    sd,
                                    emission_sd);
      accum_light_tree_contribution(kg,
                                    randu,
                                    randv,
                                    child_offsetR,
                                    pdf_factor,
                                    true,
                                    throughput,
                                    scale_factor,
                                    L,
                                    state,
                                    sd,
                                    emission_sd);
    }
    else {
      /* go down one of the child nodes */

      /* evaluate the importance of each of the child nodes */
      float I_L = calc_node_importance(kg, P, N, child_offsetL);
      float I_R = calc_node_importance(kg, P, N, child_offsetR);

      if ((I_L == 0.0f) && (I_R == 0.0f)) {
        return;
      }

      /* calculate the probability of going down the left node */
      float P_L = I_L / (I_L + I_R);

      /* choose which node to go down */
      if (randu <= P_L) {  // Going down left node
        /* rescale random number */
        randu = randu / P_L;

        offset = child_offsetL;
        pdf_factor *= P_L;
      }
      else {  // Going down right node
        /* rescale random number */
        randu = (randu * (I_L + I_R) - I_L) / I_R;

        offset = child_offsetR;
        pdf_factor *= 1.0f - P_L;
      }

      accum_light_tree_contribution(kg,
                                    randu,
                                    randv,
                                    offset,
                                    pdf_factor,
                                    false,
                                    throughput,
                                    scale_factor,
                                    L,
                                    state,
                                    sd,
                                    emission_sd);
    }
  }
}

#if defined(__BRANCHED_PATH__) || defined(__SUBSURFACE__) || defined(__SHADOW_TRICKS__) || \
    defined(__BAKING__)
/* branched path tracing: connect path directly to position on one or more lights and add it to L
 */
ccl_device_noinline_cpu void kernel_branched_path_surface_connect_light(
    KernelGlobals *kg,
    ShaderData *sd,
    ShaderData *emission_sd,
    ccl_addr_space PathState *state,
    float3 throughput,
    float num_samples_adjust,
    PathRadiance *L,
    int sample_all_lights)
{
#  ifdef __EMISSION__
  /* sample illumination from lights to find path contribution */
  if (!(sd->flag & SD_BSDF_HAS_EVAL))
    return;

  Ray light_ray;
  BsdfEval L_light;
  bool is_lamp;

#    ifdef __OBJECT_MOTION__
  light_ray.time = sd->time;
#    endif

  bool use_light_tree = kernel_data.integrator.use_light_tree;
  bool use_splitting = kernel_data.integrator.splitting_threshold != 0.0f;
  if (use_light_tree && use_splitting) {

    int index;
    float randu, randv;
    path_state_rng_2D(kg, state, PRNG_LIGHT_U, &randu, &randv);

    /* sample light group distribution */
    int group = light_group_distribution_sample(kg, &randu);
    float group_prob = kernel_tex_fetch(__light_group_sample_prob, group);
    float pdf = 1.0f;

    if (group == LIGHTGROUP_TREE) {
      /* accumulate contribution to L from potentially several lights */
      accum_light_tree_contribution(kg,
                                    randu,
                                    randv,
                                    0,
                                    group_prob,
                                    true,
                                    throughput,
                                    num_samples_adjust,
                                    L,  // todo: is num_samples_adjust correct here?
                                    state,
                                    sd,
                                    emission_sd);

      /* have accumulated all the contributions so return */
      return;
    }
    else if (group == LIGHTGROUP_DISTANT) {
      /* pick a single distant light */
      light_distant_sample(kg, sd->P, &randu, &index, &pdf);
    }
    else if (group == LIGHTGROUP_BACKGROUND) {
      /* pick a single background light */
      light_background_sample(kg, sd->P, &randu, &index, &pdf);
    }
    else {
      kernel_assert(false);
    }

    /* sample a point on the given distant/background light */
    LightSample ls;
    light_point_sample(kg, randu, randv, sd->time, sd->P, state->bounce, index, &ls);

    /* combine pdfs */
    ls.pdf *= group_prob;

    if (ls.pdf <= 0.0f)
      return;

    /* accumulate the contribution of this distant/background light to L */
    float terminate = path_state_rng_light_termination(kg, state);
    accum_light_contribution(kg,
                             sd,
                             emission_sd,
                             &ls,
                             state,
                             &light_ray,
                             &L_light,
                             L,
                             &is_lamp,
                             terminate,
                             throughput,
                             num_samples_adjust);
  }
  else if (sample_all_lights && !use_light_tree) {
    /* lamp sampling */
    for (int i = 0; i < kernel_data.integrator.num_all_lights; i++) {
      if (UNLIKELY(light_select_reached_max_bounces(kg, i, state->bounce)))
        continue;

      int num_samples = ceil_to_int(num_samples_adjust * light_select_num_samples(kg, i));
      float num_samples_inv = num_samples_adjust / num_samples;
      uint lamp_rng_hash = cmj_hash(state->rng_hash, i);

      for (int j = 0; j < num_samples; j++) {
        float light_u, light_v;
        path_branched_rng_2D(
            kg, lamp_rng_hash, state, j, num_samples, PRNG_LIGHT_U, &light_u, &light_v);
        float terminate = path_branched_rng_light_termination(
            kg, lamp_rng_hash, state, j, num_samples);

        LightSample ls;
        if (lamp_light_sample(kg, i, light_u, light_v, sd->P_pick, &ls)) {
          accum_light_contribution(kg,
                                   sd,
                                   emission_sd,
                                   &ls,
                                   state,
                                   &light_ray,
                                   &L_light,
                                   L,
                                   &is_lamp,
                                   terminate,
                                   throughput,
                                   num_samples_inv);
        }
      }
    }

    /* mesh light sampling */
    if (kernel_data.integrator.pdf_triangles != 0.0f) {
      int num_samples = ceil_to_int(num_samples_adjust *
                                    kernel_data.integrator.mesh_light_samples);
      float num_samples_inv = num_samples_adjust / num_samples;

      for (int j = 0; j < num_samples; j++) {
        float light_u, light_v;
        path_branched_rng_2D(
            kg, state->rng_hash, state, j, num_samples, PRNG_LIGHT_U, &light_u, &light_v);
        float terminate = path_branched_rng_light_termination(
            kg, state->rng_hash, state, j, num_samples);

        /* only sample triangle lights */
        if (kernel_data.integrator.num_all_lights)
          light_u = 0.5f * light_u;

        LightSample ls;
        if (light_sample(kg,
                         light_u,
                         light_v,
                         sd->time,
                         sd->P_pick,
                         sd->N_pick,
                         state->bounce,
                         &ls,
                         false)) {
          /* Same as above, probability needs to be corrected since the sampling was forced to select a mesh light. */
          if (kernel_data.integrator.num_all_lights)
            ls.pdf *= 2.0f;

          accum_light_contribution(kg,
                                   sd,
                                   emission_sd,
                                   &ls,
                                   state,
                                   &light_ray,
                                   &L_light,
                                   L,
                                   &is_lamp,
                                   terminate,
                                   throughput,
                                   num_samples_inv);
        }
      }
    }
  }
  else {
    /* sample one light at random */
    float light_u, light_v;
    path_state_rng_2D(kg, state, PRNG_LIGHT_U, &light_u, &light_v);
    float terminate = path_state_rng_light_termination(kg, state);

    LightSample ls;
    if (light_sample(
            kg, light_u, light_v, sd->time, sd->P_pick, sd->N_pick, state->bounce, &ls, false)) {
      accum_light_contribution(kg,
                               sd,
                               emission_sd,
                               &ls,
                               state,
                               &light_ray,
                               &L_light,
                               L,
                               &is_lamp,
                               terminate,
                               throughput,
                               num_samples_adjust);
    }
  }
#  endif
}

/* branched path tracing: bounce off or through surface to with new direction stored in ray */
ccl_device bool kernel_branched_path_surface_bounce(KernelGlobals *kg,
                                                    ShaderData *sd,
                                                    const ShaderClosure *sc,
                                                    int sample,
                                                    int num_samples,
                                                    ccl_addr_space float3 *throughput,
                                                    ccl_addr_space PathState *state,
                                                    PathRadianceState *L_state,
                                                    ccl_addr_space Ray *ray,
                                                    float sum_sample_weight)
{
  /* sample BSDF */
  float bsdf_pdf;
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float3 bsdf_omega_in ccl_optional_struct_init;
  differential3 bsdf_domega_in ccl_optional_struct_init;
  float bsdf_u, bsdf_v;
  path_branched_rng_2D(
      kg, state->rng_hash, state, sample, num_samples, PRNG_BSDF_U, &bsdf_u, &bsdf_v);
  int label;

  label = shader_bsdf_sample_closure(
      kg, sd, sc, bsdf_u, bsdf_v, &bsdf_eval, &bsdf_omega_in, &bsdf_domega_in, &bsdf_pdf);

  if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval))
    return false;

  /* modify throughput */
  path_radiance_bsdf_bounce(kg, L_state, throughput, &bsdf_eval, bsdf_pdf, state->bounce, label);

#  ifdef __DENOISING_FEATURES__
  state->denoising_feature_weight *= sc->sample_weight / (sum_sample_weight * num_samples);
#  endif

  /* modify path state */
  path_state_next(kg, state, label);

  /* setup ray */
  ray->P = ray_offset(sd->P, (label & LABEL_TRANSMIT) ? -sd->Ng : sd->Ng);
  ray->D = normalize(bsdf_omega_in);
  ray->t = FLT_MAX;
#  ifdef __RAY_DIFFERENTIALS__
  ray->dP = sd->dP;
  ray->dD = bsdf_domega_in;
#  endif
#  ifdef __OBJECT_MOTION__
  ray->time = sd->time;
#  endif

#  ifdef __VOLUME__
  /* enter/exit volume */
  if (label & LABEL_TRANSMIT)
    kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
#  endif

  /* branch RNG state */
  path_state_branch(state, sample, num_samples);

  /* set MIS state */
  state->min_ray_pdf = fminf(bsdf_pdf, FLT_MAX);
  state->ray_pdf = bsdf_pdf;
#  ifdef __LAMP_MIS__
  state->ray_t = 0.0f;
#  endif

  return true;
}

#endif

/* path tracing: connect path directly to position on a light and add it to L */
ccl_device_inline void kernel_path_surface_connect_light(KernelGlobals *kg,
                                                         ShaderData *sd,
                                                         ShaderData *emission_sd,
                                                         float3 throughput,
                                                         ccl_addr_space PathState *state,
                                                         PathRadiance *L)
{
  PROFILING_INIT(kg, PROFILING_CONNECT_LIGHT);

#ifdef __EMISSION__
  if (!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL)))
    return;

#  ifdef __SHADOW_TRICKS__
  if (state->flag & PATH_RAY_SHADOW_CATCHER) {
    kernel_branched_path_surface_connect_light(kg, sd, emission_sd, state, throughput, 1.0f, L, 1);
    return;
  }
#  endif

  /* sample illumination from lights to find path contribution */
  float light_u, light_v;
  path_state_rng_2D(kg, state, PRNG_LIGHT_U, &light_u, &light_v);

  Ray light_ray;
  BsdfEval L_light;
  bool is_lamp;

#  ifdef __OBJECT_MOTION__
  light_ray.time = sd->time;
#  endif

  bool has_volume = ((sd->flag & SD_HAS_VOLUME) != 0);
  LightSample ls;
  if (light_sample(kg,
                   light_u,
                   light_v,
                   sd->time,
                   sd->P_pick,
                   sd->N_pick,
                   state->bounce,
                   &ls,
                   has_volume)) {
    float terminate = path_state_rng_light_termination(kg, state);
    accum_light_contribution(kg,
                             sd,
                             emission_sd,
                             &ls,
                             state,
                             &light_ray,
                             &L_light,
                             L,
                             &is_lamp,
                             terminate,
                             throughput,
                             1.0f);
  }
#endif
}

/* path tracing: bounce off or through surface to with new direction stored in ray */
ccl_device bool kernel_path_surface_bounce(KernelGlobals *kg,
                                           ShaderData *sd,
                                           ccl_addr_space float3 *throughput,
                                           ccl_addr_space PathState *state,
                                           PathRadianceState *L_state,
                                           ccl_addr_space Ray *ray)
{
  PROFILING_INIT(kg, PROFILING_SURFACE_BOUNCE);

  /* no BSDF? we can stop here */
  if (sd->flag & SD_BSDF) {
    /* sample BSDF */
    float bsdf_pdf;
    BsdfEval bsdf_eval ccl_optional_struct_init;
    float3 bsdf_omega_in ccl_optional_struct_init;
    differential3 bsdf_domega_in ccl_optional_struct_init;
    float bsdf_u, bsdf_v;
    path_state_rng_2D(kg, state, PRNG_BSDF_U, &bsdf_u, &bsdf_v);
    int label;

    label = shader_bsdf_sample(
        kg, sd, bsdf_u, bsdf_v, &bsdf_eval, &bsdf_omega_in, &bsdf_domega_in, &bsdf_pdf);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval))
      return false;

    /* modify throughput */
    path_radiance_bsdf_bounce(kg, L_state, throughput, &bsdf_eval, bsdf_pdf, state->bounce, label);

    /* set labels */
    if (!(label & LABEL_TRANSPARENT)) {
      state->ray_pdf = bsdf_pdf;
#if defined(__LAMP_MIS__) || defined(__EMISSION__) || defined(__BACKGROUND_MIS__)
      state->ray_t = 0.0f;
#endif
      state->min_ray_pdf = fminf(bsdf_pdf, state->min_ray_pdf);
    }

    /* update path state */
    path_state_next(kg, state, label);

    /* setup ray */
    ray->P = ray_offset(sd->P, (label & LABEL_TRANSMIT) ? -sd->Ng : sd->Ng);
    kernel_update_light_picking(sd, state);
    ray->D = normalize(bsdf_omega_in);

    if (state->bounce == 0)
      ray->t -= sd->ray_length; /* clipping works through transparent */
    else
      ray->t = FLT_MAX;

#ifdef __RAY_DIFFERENTIALS__
    ray->dP = sd->dP;
    ray->dD = bsdf_domega_in;
#endif

#ifdef __VOLUME__
    /* enter/exit volume */
    if (label & LABEL_TRANSMIT)
      kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
#endif
    return true;
  }
#ifdef __VOLUME__
  else if (sd->flag & SD_HAS_ONLY_VOLUME) {
    if (!path_state_volume_next(kg, state)) {
      return false;
    }

    if (state->bounce == 0)
      ray->t -= sd->ray_length; /* clipping works through transparent */
    else
      ray->t = FLT_MAX;

    /* setup ray position, direction stays unchanged */
    ray->P = ray_offset(sd->P, -sd->Ng);
    kernel_update_light_picking(sd, state);

#  ifdef __RAY_DIFFERENTIALS__
    ray->dP = sd->dP;
#  endif

    /* enter/exit volume */
    kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
    return true;
  }
#endif
  else {
    /* no bsdf or volume? */
    return false;
  }
}

CCL_NAMESPACE_END

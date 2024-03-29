#include "physics.hpp"

#include <glm/geometric.hpp>
#include <glm/glm.hpp>

// #include <thread>
// #include <iostream>

PhysicSolver::PhysicSolver(glm::vec2 _screen_size,
                           const float _largest_particle_radius)
    : screen_size(_screen_size), sub_steps(8),
      compute_shader("renderer/shaders/solve_collisions.cs.glsl") {
  // Algorithm won't work if cell width not at least as large as bounding square
  // of largest particle. But if particles are small, a value of 30.0f tends to
  // work better after some trial and error.
  // this->cell_width = std::max(30.0f, 2 * _largest_particle_radius);
  this->cell_width = 2 * _largest_particle_radius;

  this->cell_count_x = std::ceil(_screen_size.x / this->cell_width);
  this->cell_count_y = std::ceil(_screen_size.y / this->cell_width);

  this->grid.resize(cell_count_x * cell_count_y);
}

Particle &PhysicSolver::spawnParticle(glm::vec2 pos, const float radius) {
  this->particles.emplace_back(pos, radius);

  return this->particles.back();
}

void PhysicSolver::update(const float dt) {
  const float step_dt = dt / this->sub_steps;

  for (int i = 0; i < this->sub_steps; i++) {
    this->applyGravity();
    // this->applyAirResistance();
    this->updateParticles(step_dt);
    this->constrainParticlesToBoxContainer(screen_size, screen_size / 2.0f);
    // this->solveParticleCollisionsFixedGrid();
    this->solveParticleCollisionsSpatialHash();
    // this->solveParticleCollisionsBruteForce();
  }
}

void PhysicSolver::applyGravity() {
  const float g = 2000.0f;
  for (Particle &p : this->particles) {
    p.force += glm::vec2(0, -g);
  }
}

void PhysicSolver::applyAirResistance() {
  const float resistance_mag = 500.0f;
  for (Particle &p : this->particles) {
    p.force -= glm::normalize(p.pos - p.prev_pos) * resistance_mag;
  }
}

void PhysicSolver::updateParticles(const float dt) {
  for (uint32_t p_i = 0; p_i < this->particles.size(); p_i++) {
    Particle &p = this->particles[p_i];
    p.update(dt);
  }
}

void PhysicSolver::constrainParticlesToBoxContainer(
    const glm::vec2 box_container_size, const glm::vec2 box_container_center) {
  const float e = 0.25f;
  const glm::vec2 box_container_half_size = box_container_size / 2.0f;

  const float box_left = box_container_center.x - box_container_half_size.x;
  const float box_right = box_container_center.x + box_container_half_size.x;
  const float box_top = box_container_center.y + box_container_half_size.y;
  const float box_bottom = box_container_center.y - box_container_half_size.y;

  for (uint32_t p_i = 0; p_i < this->particles.size(); p_i++) {
    Particle &p = this->particles[p_i];

    if (p.pos.x + p.radius > box_right) {
      const float original_displacement_x = p.pos.x - p.prev_pos.x;
      p.pos.x -= 2 * (p.pos.x + p.radius - box_right);
      p.prev_pos.x = p.pos.x + e * original_displacement_x;
    } else if (p.pos.x - p.radius < box_left) {
      const float original_displacement_x = p.prev_pos.x - p.pos.x;
      p.pos.x += 2 * (box_left - (p.pos.x - p.radius));
      p.prev_pos.x = p.pos.x - e * original_displacement_x;
    }
    if (p.pos.y + p.radius > box_top) {
      const float original_displacement_y = p.pos.y - p.prev_pos.y;
      p.pos.y -= 2 * (p.pos.y + p.radius - box_top);
      p.prev_pos.y = p.pos.y + e * original_displacement_y;
    } else if (p.pos.y - p.radius < box_bottom) {
      const float original_displacement_y = p.prev_pos.y - p.pos.y;
      p.pos.y += 2 * (box_bottom - (p.pos.y - p.radius));
      p.prev_pos.y = p.pos.y - e * original_displacement_y;
    }
  }
}

void PhysicSolver::solveParticleCollisionsBruteForce() {
  for (uint32_t i = 0; i < this->particles.size(); i++) {
    for (uint32_t j = i + 1; j < this->particles.size(); j++) {
      this->collideTwoParticles(i, j);
    }
  }
}

void PhysicSolver::solveParticleCollisionsFixedGrid() {
  this->assignParticlesToFixedGrid();

  this->solveGridCollisionsInRange(0, cell_count_x, 0, cell_count_y);
}

void PhysicSolver::solveGridCollisionsInRange(const uint32_t x_start,
                                              const uint32_t x_end,
                                              const uint32_t y_start,
                                              const uint32_t y_end) {
  for (uint32_t y = y_start; y < y_end; y++) {
    for (uint32_t x = x_start; x < x_end; x++) {
      const uint32_t cell_particle_count = grid[y * cell_count_x + x].size();
      for (uint32_t p1_i = 0; p1_i < cell_particle_count; p1_i++) {
        for (uint32_t p2_i = p1_i + 1; p2_i < cell_particle_count; p2_i++) {
          // p1_i and p2_i store the indices relative to the vector in the cell.
          // But we need to convert the indices relative to the particles array.
          this->collideTwoParticles(
              this->grid[y * this->cell_count_x + x][p1_i],
              this->grid[y * this->cell_count_x + x][p2_i]);
        }
      }
      // Clear for next simulation substep.
      this->grid[y * this->cell_count_x + x].clear();
    }
  }
}

void PhysicSolver::assignParticlesToFixedGrid() {
  uint32_t particle_count = this->particles.size();

  // Assign particles to grid cells.
  for (uint32_t p_i = 0; p_i < particle_count; p_i++) {
    Particle &p = this->particles[p_i];
    // Assign by AABB minimum corner vertex.
    const uint32_t cell_x = (uint32_t)((p.pos.x - p.radius) / this->cell_width);
    const uint32_t cell_y = (uint32_t)((p.pos.y - p.radius) / this->cell_width);

    this->grid[cell_y * this->cell_count_x + cell_x].push_back(p_i);

    bool in_north = cell_y + 1 < this->cell_count_y &&
                    p.pos.y + p.radius >= this->cell_width * (cell_y + 1);
    bool in_east = cell_x + 1 < this->cell_count_x &&
                   p.pos.x + p.radius >= this->cell_width * (cell_x + 1);

    if (in_north) {
      this->grid[(cell_y + 1) * this->cell_count_x + cell_x].push_back(p_i);
      if (in_east) {
        this->grid[(cell_y + 1) * this->cell_count_x + cell_x + 1].push_back(
            p_i);
      }
    }
    if (in_east) {
      this->grid[cell_y * this->cell_count_x + cell_x + 1].push_back(p_i);
    }
  }
}

void PhysicSolver::solveParticleCollisionsSpatialHash() {

  std::vector<int32_t> count_arr(this->cell_count_x * this->cell_count_y + 1);
  std::vector<int32_t> particles_grouped(this->particles.size());

  // Populate cell counts, by particle center.
  for (Particle &p : this->particles) {
    int32_t cell_x = (int32_t)(p.pos.x / this->cell_width);
    int32_t cell_y = (int32_t)(p.pos.y / this->cell_width);
    int32_t h = cell_y * this->cell_count_x + cell_x;

    count_arr[h]++;
  }

  // Compute partial sums.
  for (int32_t i = 1; i < count_arr.size(); i++) {
    count_arr[i] += count_arr[i - 1];
  }

  // Assign to grouped particle array.
  for (int32_t p_i = 0; p_i < this->particles.size(); p_i++) {
    Particle &p = this->particles[p_i];

    int32_t cell_x = (int32_t)(p.pos.x / this->cell_width);
    int32_t cell_y = (int32_t)(p.pos.y / this->cell_width);
    int32_t h = cell_y * this->cell_count_x + cell_x;

    count_arr[h]--;
    particles_grouped[count_arr[h]] = p_i;
  }

  // COMPUTE SHADER STUFF
  // --------------------
  this->compute_shader.use();

  // Create SSBO
  uint32_t positions_ssbo, count_ssbo, particles_grouped_ssbo, meta_ssbo;

  // Create positions array
  std::vector<float> positions_arr(2 * this->particles.size());
  for (int32_t i = 0; i < this->particles.size(); i++) {
    Particle &p = this->particles[i];
    positions_arr[2 * i] = p.pos.x;
    positions_arr[2 * i + 1] = p.pos.y;
  }

  glGenBuffers(1, &positions_ssbo);
  glGenBuffers(1, &count_ssbo);
  glGenBuffers(1, &particles_grouped_ssbo);
  glGenBuffers(1, &meta_ssbo);

  // Positions SSBO
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, positions_ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float) * positions_arr.size(),
               positions_arr.data(), GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, positions_ssbo);

  // Count SSBO
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, count_ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(int32_t) * count_arr.size(),
               count_arr.data(), GL_STATIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, count_ssbo);

  // Particles grouped SSBO
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles_grouped_ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER,
               sizeof(int32_t) * particles_grouped.size(),
               particles_grouped.data(), GL_STATIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, particles_grouped_ssbo);

  // Meta SSBO
  struct MetaSsboContainer {
    float cell_width;
    int cell_count_x;
    int cell_count_y;
    int particle_count;
  };

  MetaSsboContainer meta_ssbo_container;
  meta_ssbo_container.cell_width = this->cell_width;
  meta_ssbo_container.cell_count_x = this->cell_count_x;
  meta_ssbo_container.cell_count_y = this->cell_count_y;
  meta_ssbo_container.particle_count = this->particles.size();

  glBindBuffer(GL_SHADER_STORAGE_BUFFER, meta_ssbo);
  glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(meta_ssbo_container),
               &meta_ssbo_container, GL_STATIC_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, meta_ssbo);

  // Dispatch workers.
  // Dispatch in multiples of 64 due to 'warp size'.
  glDispatchCompute((this->particles.size() + 63) / 64, 1, 1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  // Retrieve recomputed positions back into the positions array.
  float updated_positions_arr[positions_arr.size()];
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, positions_ssbo);
  glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                     sizeof(float) * positions_arr.size(),
                     updated_positions_arr);

  // Update actual positions
  for (int32_t p_i = 0; p_i < this->particles.size(); p_i++) {
    this->particles[p_i].pos.x = updated_positions_arr[2 * p_i];
    this->particles[p_i].pos.y = updated_positions_arr[2 * p_i + 1];
  }
}

void PhysicSolver::collideTwoParticles(const uint32_t p1_i,
                                       const uint32_t p2_i) {
  if (p1_i == p2_i)
    return;

  Particle &p1 = this->particles[p1_i];
  Particle &p2 = this->particles[p2_i];

  const float e = 0.75f;
  const float distanceBetweenCenters = glm::length(p1.pos - p2.pos);
  const float sumOfRadii = p1.radius + p2.radius;

  if (distanceBetweenCenters < sumOfRadii) {
    glm::vec2 collisionAxis = p1.pos - p2.pos;
    glm::vec2 n = collisionAxis / distanceBetweenCenters;
    const float massSum = p1.radius + p2.radius;
    const float massRatio1 = p1.radius / massSum;
    const float massRatio2 = p2.radius / massSum;
    const float delta = e * (sumOfRadii - distanceBetweenCenters);
    p1.pos += massRatio2 * delta * n;
    p2.pos -= massRatio1 * delta * n;
  }
}

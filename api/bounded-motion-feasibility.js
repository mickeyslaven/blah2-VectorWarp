'use strict';

const dot = (a, b) => a.reduce((sum, value, axis) => sum + value * b[axis], 0);
const subtract = (a, b) => a.map((value, axis) => value - b[axis]);
const norm = point => Math.hypot(...point);
const vector = value => Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);

// Orthogonal axes of the full covariance, including its off-diagonal terms.
function ellipsoid(covariance) {
  if (!Array.isArray(covariance) || covariance.length !== 3 || !covariance.every(vector)) return null;
  const matrix = covariance.map(row => [...row]);
  const scale = Math.max(...matrix.flat().map(Math.abs));
  if (!(scale > 0) || matrix.some((row, i) => row.some((value, j) =>
    Math.abs(value - matrix[j][i]) > scale * 1e-10))) return null;
  const axes = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  for (let iteration = 0; iteration < 32; iteration += 1) {
    let p = 0, q = 1;
    for (const [i, j] of [[0, 2], [1, 2]])
      if (Math.abs(matrix[i][j]) > Math.abs(matrix[p][q])) { p = i; q = j; }
    if (Math.abs(matrix[p][q]) < scale * 1e-14) break;
    const angle = .5 * Math.atan2(2 * matrix[p][q], matrix[q][q] - matrix[p][p]);
    const c = Math.cos(angle), s = Math.sin(angle);
    const pp = matrix[p][p], qq = matrix[q][q], pq = matrix[p][q];
    matrix[p][p] = c * c * pp - 2 * s * c * pq + s * s * qq;
    matrix[q][q] = s * s * pp + 2 * s * c * pq + c * c * qq;
    matrix[p][q] = matrix[q][p] = 0;
    for (let i = 0; i < 3; i += 1) {
      if (i !== p && i !== q) {
        const ip = matrix[i][p], iq = matrix[i][q];
        matrix[i][p] = matrix[p][i] = c * ip - s * iq;
        matrix[i][q] = matrix[q][i] = s * ip + c * iq;
      }
      const ip = axes[i][p], iq = axes[i][q];
      axes[i][p] = c * ip - s * iq; axes[i][q] = s * ip + c * iq;
    }
  }
  const values = matrix.map((row, i) => row[i]);
  if (values.some(value => !(value > scale * 1e-12))) return null;
  const coordinates = point => values.map((_, axis) => dot(point, axes.map(row => row[axis])));
  const distanceSquared = point => coordinates(point).reduce((sum, value, axis) => sum + value ** 2 / values[axis], 0);
  return {distanceSquared, project(point, sigma) {
    const local = coordinates(point), distance = Math.sqrt(distanceSquared(point));
    if (distance <= sigma) return point;
    let low = 0, high = values.reduce((sum, value) => sum + value, 0) * (distance / sigma - 1);
    // Euclidean projection onto an ellipsoid: y_i = x_i * d_i/(d_i+lambda).
    for (let iteration = 0; iteration < 40; iteration += 1) {
      const middle = (low + high) / 2;
      const squared = local.reduce((sum, value, axis) =>
        sum + value ** 2 * values[axis] / (values[axis] + middle) ** 2, 0);
      if (squared > sigma ** 2) low = middle; else high = middle;
    }
    return axes.map(row => row.reduce((sum, value, axis) =>
      sum + value * local[axis] * values[axis] / (values[axis] + high), 0));
  }};
}

// Find an explicit trajectory witness inside the measured covariance ellipsoids.
// Cyclic Euclidean projections intersect convex constraints; the finite iteration
// budget can cause false rejections, never acceptance without a checked witness.
// Acceleration uses the same secant-midpoint convention as the existing tracker.
function boundedMotionTrajectory(observations, {maxSpeedKmS = .45, maxAccelerationKmS2 = .08,
  sigma = 3, maxIterations = 128} = {}) {
  if (!Array.isArray(observations) || observations.length < 2 || observations.length > 9 ||
      ![maxSpeedKmS, maxAccelerationKmS2, sigma].every(value => Number.isFinite(value) && value > 0)) return null;
  const shapes = observations.map(item => ellipsoid(item.covariance));
  if (shapes.some(item => !item) || observations.some(item => !vector(item.center) ||
      !vector(item.point ?? item.center) || !Number.isFinite(item.timestamp))) return null;
  const points = observations.map(item => [...(item.point ?? item.center)]);
  const intervals = observations.slice(1).map((item, index) => (item.timestamp - observations[index].timestamp) / 1000);
  if (intervals.some(value => !(value > 0))) return null;
  const constraints = [];
  intervals.forEach((dt, index) => {
    constraints.push({indices: [index, index + 1], coefficients: [-1 / dt, 1 / dt], limit: maxSpeedKmS});
    if (index > 0) {
      const prior = intervals[index - 1];
      constraints.push({indices: [index - 1, index, index + 1],
        coefficients: [1 / prior, -1 / prior - 1 / dt, 1 / dt],
        limit: maxAccelerationKmS2 * (prior + dt) / 2});
    }
  });
  const combination = constraint => [0, 1, 2].map(axis => constraint.indices.reduce((sum, index, i) =>
    sum + constraint.coefficients[i] * points[index][axis], 0));
  const witness = iterations => {
    const distances = points.map((point, index) =>
      Math.sqrt(shapes[index].distanceSquared(subtract(point, observations[index].center))));
    if (distances.some(value => !Number.isFinite(value) || value > sigma + 1e-9) ||
        constraints.some(constraint => norm(combination(constraint)) > constraint.limit)) return null;
    const last = points.length - 1, dt = intervals.at(-1);
    const velocity = subtract(points[last], points[last - 1]).map(value => value / dt);
    return {points, velocity, intervalSeconds: dt, iterations,
      maximumMahalanobisDistance: Math.max(...distances), currentMahalanobisDistance: distances.at(-1)};
  };
  const initial = witness(0);
  if (initial) return initial;
  for (let iteration = 1; iteration <= maxIterations; iteration += 1) {
    for (let index = 0; index < points.length; index += 1) if (!observations[index].fixed) {
      const delta = shapes[index].project(subtract(points[index], observations[index].center), sigma * (1 - 1e-10));
      points[index] = observations[index].center.map((value, axis) => value + delta[axis]);
    }
    for (const constraint of constraints) {
      const value = combination(constraint), magnitude = norm(value);
      if (magnitude <= constraint.limit) continue;
      const denominator = constraint.indices.reduce((sum, index, i) =>
        sum + (observations[index].fixed ? 0 : constraint.coefficients[i] ** 2), 0);
      if (!denominator) return null;
      const scale = (1 - constraint.limit * (1 - 1e-9) / magnitude) / denominator;
      constraint.indices.forEach((index, i) => {
        if (!observations[index].fixed) points[index] = points[index].map((component, axis) =>
          component - constraint.coefficients[i] * scale * value[axis]);
      });
    }
    const result = witness(iteration);
    if (result) return result;
  }
  return null;
}

module.exports = {boundedMotionTrajectory};

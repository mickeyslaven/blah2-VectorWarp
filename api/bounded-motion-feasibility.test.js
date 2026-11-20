'use strict';
const assert = require('assert/strict');
const {boundedMotionTrajectory} = require('./bounded-motion-feasibility');
const covariance = sigma => [[sigma ** 2, 0, 0], [0, sigma ** 2, 0], [0, 0, sigma ** 2]];
const observation = (x, time, sigma = .1) => ({center: [x, 0, 0], timestamp: time * 1000, covariance: covariance(sigma)});
const verify = (observations, witness) => {
  assert.ok(witness, 'A checked bounded-motion witness must exist');
  const velocities = witness.points.slice(1).map((point, index) => {
    const dt = (observations[index + 1].timestamp - observations[index].timestamp) / 1000;
    const velocity = point.map((value, axis) => (value - witness.points[index][axis]) / dt);
    assert.ok(Math.hypot(...velocity) <= .45, 'Every witness secant respects 450 m/s');
    return {velocity, dt};
  });
  velocities.slice(1).forEach((item, index) => {
    const previous = velocities[index], elapsed = (item.dt + previous.dt) / 2;
    assert.ok(Math.hypot(...item.velocity.map((value, axis) => value - previous.velocity[axis])) / elapsed <= .08,
      'Every midpoint acceleration respects 80 m/s squared');
  });
};

const jitter = [-.2, .2, -.2, .2, -.2, .2].map((x, index) => observation(x, index * .5, .15));
const fitted = boundedMotionTrajectory(jitter);
verify(jitter, fitted);
fitted.points.forEach((point, index) => assert.ok(Math.hypot(...point.map((value, axis) =>
  value - jitter[index].center[axis])) <= .45 + 1e-9, 'Every point stays inside its three-sigma ellipsoid'));
assert.ok(fitted.iterations > 0, 'The raw 800 m/s jitter requires covariance-aware fitting');
assert.equal(boundedMotionTrajectory(jitter, {maxIterations: 0}), null,
  'Failure to find a witness within the work budget fails closed');

assert.equal(boundedMotionTrajectory([observation(0, 0, .01), observation(1, .5, .01)]), null,
  'Disjoint regions farther apart than the maximum travel cannot associate');
const impossibleAcceleration = [observation(0, 0, .001), observation(.4, 1, .001), observation(0, 2, .001)];
assert.ok(boundedMotionTrajectory(impossibleAcceleration.slice(0, 2)));
assert.ok(boundedMotionTrajectory(impossibleAcceleration.slice(1)));
assert.equal(boundedMotionTrajectory(impossibleAcceleration), null,
  'Individually feasible pairs cannot invent an incompatible common acceleration trajectory');
const irregular = [observation(0, 0, .00001), observation(-.3825, 1, .00001), observation(0, 10, .00001)];
assert.equal(boundedMotionTrajectory(irregular), null,
  'Unequal intervals use velocity midpoints and retain the physical acceleration bound');

// Large uncertainty along x cannot excuse motion along the tightly known y axis.
const elongated = [[1, 0, 0], [0, 1e-6, 0], [0, 0, 1e-6]];
assert.equal(boundedMotionTrajectory([{...observation(0, 0), covariance: elongated},
  {...observation(0, .5), center: [0, 1, 0], covariance: elongated}]), null);
const angle = .7, c = Math.cos(angle), s = Math.sin(angle);
const rotate = point => [c * point[0] - s * point[1], s * point[0] + c * point[1], point[2]];
const rotatedCovariance = [[c * c + s * s * 1e-6, c * s * (1 - 1e-6), 0],
  [c * s * (1 - 1e-6), s * s + c * c * 1e-6, 0], [0, 0, 1e-6]];
assert.equal(boundedMotionTrajectory([{...observation(0, 0), covariance: rotatedCovariance},
  {...observation(0, .5), center: rotate([0, 1, 0]), covariance: rotatedCovariance}]), null,
  'Rotating a thin ellipsoid preserves rejection; off-diagonal covariance is not discarded');
const longAxis = [{...observation(0, 0), covariance: rotatedCovariance},
  {...observation(0, .5), center: rotate([1, 0, 0]), covariance: rotatedCovariance}];
verify(longAxis, boundedMotionTrajectory(longAxis));

const pinned = [observation(0, 0, 1), observation(.4, 1, 1), observation(0, 2, .001)];
assert.ok(boundedMotionTrajectory(pinned));
assert.equal(boundedMotionTrajectory(pinned.map((item, index) => ({...item, fixed: index < 2}))), null,
  'A fixed historical boundary cannot be rewritten to rescue later impossible motion');
for (const invalid of [[[1, 0, 0], [0, -1, 0], [0, 0, 1]], [[1, 1, 0], [0, 1, 0], [0, 0, 1]],
  [[1, 0, 0], [0, 0, 0], [0, 0, 1]]])
  assert.equal(boundedMotionTrajectory([observation(0, 0), {...observation(0, 1), covariance: invalid}]), null,
    'Indefinite, asymmetric, and singular covariance fail closed');
console.log('Covariance ellipsoid trajectory witnesses, anisotropy, physical bounds, and fixed history passed.');

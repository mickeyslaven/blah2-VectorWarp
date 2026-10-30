(function (global) {
  'use strict';

  const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
  const models = new Map();

  function builder() {
    const vertices = [], faces = [];
    const addVertex = point => (vertices.push(point), vertices.length - 1);
    const face = (points, shade = 1) => faces.push({points, shade});
    const box = (cx, cy, cz, sx, sy, sz, shade = 1, rotation = 0) => {
      const cosine = Math.cos(rotation), sine = Math.sin(rotation);
      const points = [[-1,-1,-1],[1,-1,-1],[1,1,-1],[-1,1,-1],[-1,-1,1],[1,-1,1],[1,1,1],[-1,1,1]]
        .map(([x,y,z]) => addVertex([cx + (x * sx * cosine - y * sy * sine) / 2,
          cy + (x * sx * sine + y * sy * cosine) / 2, cz + z * sz / 2]));
      [[0,1,2,3],[4,7,6,5],[0,4,5,1],[1,5,6,2],[2,6,7,3],[3,7,4,0]]
        .forEach((indices, index) => face(indices.map(value => points[value]), shade * (.72 + index * .055)));
    };
    const prism = (outline, z0, z1, shade = 1) => {
      const lower = outline.map(([x,y]) => addVertex([x,y,z0]));
      const upper = outline.map(([x,y]) => addVertex([x,y,z1]));
      face(lower, shade * .7); face([...upper].reverse(), shade * 1.08);
      outline.forEach((_, index) => face([lower[index], lower[(index + 1) % lower.length],
        upper[(index + 1) % upper.length], upper[index]], shade * .86));
    };
    const ellipsoid = (cx, cy, cz, rx, ry, rz, shade = 1, segments = 8, rings = 4) => {
      const rows = [];
      for (let ring = 0; ring <= rings; ring++) {
        const phi = -Math.PI / 2 + Math.PI * ring / rings;
        rows.push(Array.from({length: segments}, (_, segment) => {
          const theta = Math.PI * 2 * segment / segments;
          return addVertex([cx + rx * Math.cos(phi) * Math.cos(theta),
            cy + ry * Math.cos(phi) * Math.sin(theta), cz + rz * Math.sin(phi)]);
        }));
      }
      for (let ring = 0; ring < rings; ring++) for (let segment = 0; segment < segments; segment++)
        face([rows[ring][segment], rows[ring][(segment + 1) % segments],
          rows[ring + 1][(segment + 1) % segments], rows[ring + 1][segment]],
        shade * (.78 + .25 * (ring + 1) / rings));
    };
    const cylinder = (cx, cy, z0, radius, height, shade = 1, segments = 7) => {
      const outline = Array.from({length: segments}, (_, index) => {
        const angle = Math.PI * 2 * index / segments;
        return [cx + Math.cos(angle) * radius, cy + Math.sin(angle) * radius];
      });
      prism(outline, z0, z0 + height, shade);
    };
    return {vertices, faces, box, prism, ellipsoid, cylinder};
  }

  function aircraftModel(kind) {
    const b = builder();
    const wing = (span, chord, y = .08, shade = 1) => b.prism([
      [-.08,y + chord / 2],[-span,y - chord / 2],[-span * .92,y - chord / 2 - .05],
      [0,y - chord / 5],[span * .92,y - chord / 2 - .05],[span,y - chord / 2],[.08,y + chord / 2]
    ], .08, .16, shade);
    const tail = (span = .38) => b.prism([[-.05,-.72],[-span,-.87],[-span,-.94],
      [0,-.87],[span,-.94],[span,-.87],[.05,-.72]], .12, .19, .88);
    if (kind === 'rotorcraft') {
      b.ellipsoid(0,.18,.28,.35,.5,.27,1.05,8,4);
      b.box(0,-.55,.3,.13,1.05,.13,.83);
      b.box(0,-1.08,.3,.5,.08,.1,.9);
      b.box(0,.1,.6,2.0,.07,.045,1.12,.18);
      b.box(0,.1,.61,.07,2.0,.04,1.12,-.18);
      b.box(-.25,.18,.02,.05,.75,.04,.55); b.box(.25,.18,.02,.05,.75,.04,.55);
      return b;
    }
    if (kind === 'glider') {
      b.ellipsoid(0,0,.2,.13,1.0,.14,1.02,8,4); wing(1.25,.2,.1,1.08); tail(.28);
      b.box(0,-.78,.37,.06,.28,.36,.82); return b;
    }
    if (kind === 'drone') {
      b.ellipsoid(0,.05,.18,.12,.88,.12,1.0,7,3); wing(1.05,.27,.12,1.05); tail(.45);
      b.box(0,-.76,.35,.05,.25,.34,.82); b.box(0,-.98,.2,.42,.04,.04,.7); return b;
    }
    const heavy = kind === 'heavy';
    const light = kind === 'light';
    b.ellipsoid(0,.02,.2, light ? .18 : .2, light ? .88 : 1.05, light ? .17 : .2, 1.03, 8, 4);
    wing(light ? .82 : heavy ? 1.12 : 1.0, light ? .28 : .38, .1, 1.04);
    tail(light ? .26 : .4);
    b.box(0,-.8,.39,.06,.3,.4,.82);
    if (!light) {
      b.ellipsoid(-.42,.02,.03,.12,.3,.11,.68,7,3);
      b.ellipsoid(.42,.02,.03,.12,.3,.11,.68,7,3);
    } else {
      b.box(0,.93,.2,.42,.035,.035,.62);
    }
    return b;
  }

  function receiverModel() {
    const b = builder();
    b.cylinder(0,0,-.7,.18,1.25,.74,8); b.cylinder(0,0,.5,.25,.18,1.04,8);
    for (let index = 0; index < 5; index++) {
      const angle = Math.PI * 2 * index / 5;
      b.box(Math.cos(angle) * .58, Math.sin(angle) * .58, .59, .95,.09,.08,.82,angle);
      b.cylinder(Math.cos(angle) * 1.03, Math.sin(angle) * 1.03, .48,.075,.52,1.08,7);
    }
    b.prism([[-.4,-.24],[.4,-.24],[.52,.2],[0,.42],[-.52,.2]],-.76,-.66,.58);
    return b;
  }

  function transmitterModel() {
    const b = builder();
    b.box(0,0,-.75,.7,.7,.12,.58);
    for (let level = 0; level < 4; level++) {
      const z = -.65 + level * .48, width = .46 - level * .07;
      b.box(0,0,z,width,width,.08,.78);
      b.box(-width/2,0,z+.24,.055,.055,.5,.78); b.box(width/2,0,z+.24,.055,.055,.5,.78);
      b.box(0,-width/2,z+.24,.055,.055,.5,.78); b.box(0,width/2,z+.24,.055,.055,.5,.78);
    }
    for (const side of [-1,1]) {
      b.box(side * .4,0,.35,.13,.16,.62,1.08); b.box(0,side * .4,.62,.16,.13,.62,1.08);
    }
    b.cylinder(0,0,1.27,.035,.48,1.2,6);
    return b;
  }

  function model(name) {
    if (!models.has(name)) models.set(name, name === 'receiver' ? receiverModel() :
      name === 'transmitter' ? transmitterModel() : aircraftModel(name));
    return models.get(name);
  }

  function parseColor(value) {
    const match = String(value || '').match(/^#([0-9a-f]{6})$/i);
    if (!match) return [77,212,176];
    const number = Number.parseInt(match[1], 16);
    return [number >> 16, (number >> 8) & 255, number & 255];
  }

  function createRenderer(canvas) {
    const context = canvas.getContext('2d', {alpha: true, desynchronized: true});
    if (!context) return {render() {}, available: false};
    canvas.dataset.renderer = 'vectorwarp-real-3d-v1';
    const render = (instances, options = {}) => {
      const width = Math.max(1, canvas.clientWidth), height = Math.max(1, canvas.clientHeight);
      // The aircraft occupy only a few dozen CSS pixels. Rendering the entire
      // full-screen transparent canvas at a 2x backing resolution wastes fill
      // bandwidth on large displays without improving those small models.
      const ratioCap = width * height > 1000000 ? 1.35 : 1.75;
      const ratio = Math.min(ratioCap, global.devicePixelRatio || 1);
      const fast = options.fast === true;
      if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
        canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio);
      }
      context.setTransform(ratio,0,0,ratio,0,0); context.clearRect(0,0,width,height);
      for (const instance of instances) {
        const mesh = model(instance.model), yaw = (instance.yaw || 0) * Math.PI / 180;
        const pitch = (instance.pitch || 0) * Math.PI / 180;
        // Retain real mesh depth while biasing the presentation toward a
        // readable planform. At the old 34° view, headings perpendicular to
        // the screen made small phone-size aircraft collapse edge-on.
        const view = (global.innerWidth <= 760 ? 20 : 25) * Math.PI / 180;
        const cosineYaw = Math.cos(yaw), sineYaw = Math.sin(yaw);
        const cosinePitch = Math.cos(pitch), sinePitch = Math.sin(pitch);
        const cosineView = Math.cos(view), sineView = Math.sin(view);
        const transformed = mesh.vertices.map(([x,y,z]) => {
          const py = y * cosinePitch - z * sinePitch, pz = y * sinePitch + z * cosinePitch;
          const rx = x * cosineYaw + py * sineYaw, ry = -x * sineYaw + py * cosineYaw;
          const sy = -ry * cosineView - pz * sineView, depth = ry * sineView - pz * cosineView;
          const perspective = 1 / Math.max(.72, 1 + depth * .08);
          return {x: instance.x + rx * instance.size * perspective,
            y: instance.y + sy * instance.size * perspective, depth, world:[rx,ry,pz]};
        });
        const color = parseColor(instance.color), light = [-.35,.42,.84];
        const visible = mesh.faces.map(face => {
          const points = face.points.map(index => transformed[index]);
          const [a,b,c] = points.map(point => point.world);
          const u = [b[0]-a[0],b[1]-a[1],b[2]-a[2]], v = [c[0]-a[0],c[1]-a[1],c[2]-a[2]];
          const normal = [u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]];
          const length = Math.hypot(...normal) || 1;
          const illumination = clamp(.38 + .65 * Math.abs((normal[0]*light[0]+normal[1]*light[1]+normal[2]*light[2])/length),.3,1.08);
          return {points, depth: points.reduce((sum, point) => sum + point.depth, 0) / points.length,
            shade: face.shade * illumination};
        }).sort((left,right) => right.depth - left.depth);
        context.save();
        if (!fast) {
          context.shadowColor = 'rgba(0,0,0,.45)'; context.shadowBlur = 2; context.shadowOffsetY = 2;
        }
        for (const face of visible) {
          context.beginPath(); face.points.forEach((point,index) => index ? context.lineTo(point.x,point.y) : context.moveTo(point.x,point.y)); context.closePath();
          const shade = clamp(face.shade, .18, 1.18);
          context.fillStyle = `rgb(${clamp(Math.round(color[0]*shade),0,255)} ${clamp(Math.round(color[1]*shade),0,255)} ${clamp(Math.round(color[2]*shade),0,255)})`;
          context.fill(); context.shadowColor = 'transparent';
          if (!fast) {
            context.strokeStyle = 'rgba(255,255,255,.16)'; context.lineWidth = .55; context.stroke();
          }
        }
        context.restore();
      }
    };
    return {render, available: true};
  }

  global.VectorWarp3D = {createRenderer};
})(typeof window !== 'undefined' ? window : globalThis);

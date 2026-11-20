(function () {
  'use strict';

  // MapLibre source/layer ownership lives here so the insight controller can
  // refresh data without rebuilding sources, layers, or the active camera.
  function createMapLayers({finite, array}) {
    const bearingOverlaySourceId = 'vectorwarp-bearing-overlays';
    const radarCoverageSourceId = 'vectorwarp-radar-coverage';

    function syncGeoJsonSource(map, id, data) {
      const source = map.getSource(id);
      if (source?.setData) source.setData(data);
      else map.addSource(id, {type: 'geojson', data});
      const activeSource = map.getSource(id);
      if (activeSource) activeSource._vectorwarpData = data;
    }

    function addLayerOnce(map, layer) {
      if (!map.getLayer(layer.id)) map.addLayer(layer);
    }

    function contiguousCoverageRuns(coverage, project, isValid) {
      const rows = array(coverage?.points).map(point => ({
        bin: Number(point.bin), point: project(point)
      })).filter(row => Number.isInteger(row.bin) && isValid(row.point))
        .sort((left, right) => left.bin - right.bin);
      const runs = [];
      for (const row of rows) {
        const run = runs.at(-1);
        if (run && row.bin === run.at(-1).bin + 1) run.push(row);
        else runs.push([row]);
      }
      const binCount = Math.max(1, Number(coverage?.bins) || 72);
      if (runs.length > 1 && runs[0][0]?.bin === 0 &&
          runs.at(-1).at(-1)?.bin === binCount - 1)
        runs.unshift(runs.pop().concat(runs.shift()));
      return runs;
    }

    function bearingOverlayGeoJson(rows) {
      const features = [];
      for (const item of rows) {
        const properties = {track: String(item.id ?? ''),
          uncertainty: finite(item.uncertainty_deg), state: String(item.state || '')};
        const corridor = array(item.uncertainty_corridor).map(point =>
          [finite(point.longitude), finite(point.latitude)])
          .filter(point => point.every(Number.isFinite));
        if (corridor.length > 1) features.push({type: 'Feature',
          properties: {...properties, kind: 'corridor'},
          geometry: {type: 'LineString', coordinates: corridor}});
        const resolved = item.location &&
          [finite(item.location.longitude), finite(item.location.latitude)];
        if (resolved && resolved.every(Number.isFinite)) features.push({type: 'Feature',
          properties: {...properties, kind: 'resolved'},
          geometry: {type: 'Point', coordinates: resolved}});
        const estimate = !item.location && item.estimate &&
          [finite(item.estimate.longitude), finite(item.estimate.latitude)];
        if (estimate && estimate.every(Number.isFinite)) features.push({type: 'Feature',
          properties: {...properties, kind: 'estimate'},
          geometry: {type: 'Point', coordinates: estimate}});
        if (!item.location) for (const candidate of array(item.candidates)) {
          const coordinates = [finite(candidate.longitude), finite(candidate.latitude)];
          if (coordinates.every(Number.isFinite)) features.push({type: 'Feature',
            properties: {...properties, kind: 'candidate'},
            geometry: {type: 'Point', coordinates}});
        }
      }
      return {type: 'FeatureCollection', features};
    }

    function syncBearingOverlayLayers(map, rows) {
      if (!map?.addSource || !map?.getSource || !map?.addLayer || !map?.getLayer) return;
      syncGeoJsonSource(map, bearingOverlaySourceId, bearingOverlayGeoJson(rows));
      addLayerOnce(map, {id: 'vectorwarp-bearing-corridors', type: 'line',
        source: bearingOverlaySourceId, filter: ['==', ['get', 'kind'], 'corridor'],
        paint: {'line-color': '#f7c75f', 'line-opacity': .72, 'line-width': 6}});
      addLayerOnce(map, {id: 'vectorwarp-bearing-resolved', type: 'circle',
        source: bearingOverlaySourceId, filter: ['==', ['get', 'kind'], 'resolved'],
        paint: {'circle-radius': 7, 'circle-color': '#f7c75f', 'circle-opacity': .95,
          'circle-stroke-color': '#211812', 'circle-stroke-width': 2}});
      addLayerOnce(map, {id: 'vectorwarp-bearing-estimates', type: 'circle',
        source: bearingOverlaySourceId, filter: ['==', ['get', 'kind'], 'estimate'],
        paint: {'circle-radius': 6, 'circle-color': '#b99cff', 'circle-opacity': .58}});
      addLayerOnce(map, {id: 'vectorwarp-bearing-candidates', type: 'circle',
        source: bearingOverlaySourceId, filter: ['==', ['get', 'kind'], 'candidate'],
        paint: {'circle-radius': 5, 'circle-color': '#f7c75f', 'circle-opacity': .55}});
    }

    function syncFusionEnvelopeLayers(map, data) {
      if (!map?.addSource || !map?.getSource || !map?.addLayer || !map?.getLayer) return;
      const sourceId = 'vectorwarp-fusion-envelopes';
      syncGeoJsonSource(map, sourceId, data);
      addLayerOnce(map, {id: 'vectorwarp-fusion-envelope-fill', type: 'fill',
        source: sourceId, paint: {'fill-color': '#b6aca6', 'fill-opacity': .08}});
      addLayerOnce(map, {id: 'vectorwarp-fusion-envelope-outline', type: 'line',
        source: sourceId,
        paint: {'line-color': '#b6aca6', 'line-width': 2, 'line-dasharray': [3, 2]}});
    }

    function radarCoverageGeoJson(coverage, receiver) {
      const runs = contiguousCoverageRuns(coverage,
        point => [Number(point.longitude), Number(point.latitude)],
        point => point.every(Number.isFinite));
      const origin = [Number(receiver?.longitude), Number(receiver?.latitude)];
      if (!origin.every(Number.isFinite)) return {type: 'FeatureCollection', features: []};
      return {type: 'FeatureCollection', features: runs.map((run, index) => {
        const points = run.map(row => row.point);
        return points.length > 1 ? {
          type: 'Feature', id: `sector-${index}`, properties: {},
          geometry: {type: 'Polygon', coordinates: [[origin, ...points, origin]]}
        } : {
          type: 'Feature', id: `radial-${index}`, properties: {},
          geometry: {type: 'LineString', coordinates: [origin, ...points]}
        };
      })};
    }

    function syncRadarCoverageLayer(map, coverage, receiver) {
      if (!map?.addSource || !map?.getSource || !map?.addLayer || !map?.getLayer) return false;
      // Keep malformed coverage distinct from a renderer failure. Callers can
      // then retain the SVG compatibility fallback without hiding bad data.
      const data = radarCoverageGeoJson(coverage, receiver);
      try {
        syncGeoJsonSource(map, radarCoverageSourceId, data);
        addLayerOnce(map, {id: 'vectorwarp-radar-coverage-fill', type: 'fill',
          source: radarCoverageSourceId, filter: ['==', '$type', 'Polygon'],
          paint: {'fill-color': '#20dddc', 'fill-opacity': .09}});
        addLayerOnce(map, {id: 'vectorwarp-radar-coverage-line', type: 'line',
          source: radarCoverageSourceId,
          paint: {'line-color': '#20dddc', 'line-opacity': .92, 'line-width': 3}});
        return true;
      } catch (_) {
        // Older geographic renderers may not expose style mutation. Retain the
        // screen-space SVG path below as a compatibility fallback.
        return false;
      }
    }

    return {contiguousCoverageRuns, syncBearingOverlayLayers, syncFusionEnvelopeLayers,
      syncRadarCoverageLayer};
  }

  window.VectorWarpLocationMapLayers = {createMapLayers};
}());

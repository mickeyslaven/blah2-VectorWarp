const http = require('http');

var nCpi = 20;
var ts = '';
var cpi = [];
var output = {};
const options_timestamp = {
  host: '127.0.0.1',
  path: '/api/timestamp',
  port: 3000
};
const options_iqdata = {
  host: '127.0.0.1',
  path: '/api/timing',
  port: 3000
};

function update_data(callback) {
  http.get(options_timestamp, function (res) {
    res.setEncoding('utf8');
    res.on('data', function (body) {
      if (ts != body) {
        ts = body;
        http.get(options_iqdata, function (res) {
          let body_map = '';
          res.setEncoding('utf8');
          res.on('data', (chunk) => {
            body_map += chunk;
          });
          res.on('end', () => {
            try {
              cpi = JSON.parse(body_map);
              keys = Object.keys(cpi);
              keys = keys.filter(item => item !== "uptime");
              keys = keys.filter(item => item !== "nCpi");

              // Drop anything blah2 has stopped reporting. Without this a key
              // that disappears, because the build changed or a stage was
              // removed, keeps its last values forever and the plot shows a
              // frozen line that looks like a live measurement. Observed after
              // swapping builds: cpi_interval and pipeline_wait were still
              // being served long after blah2 stopped emitting them.
              for (const stale of Object.keys(output)) {
                if (!keys.includes(stale)) {
                  delete output[stale];
                }
              }

              for (i = 0; i < keys.length; i++) {
                if (!(keys[i] in output)) {
                  output[keys[i]] = [];
                }
                output[keys[i]].push(cpi[keys[i]]);
                if (output[keys[i]].length > nCpi) {
                  output[keys[i]].shift();
                }
              }
            } catch (e) {
              console.error(e.message);
            }
          });
        });
      }
    });
  });
}

setInterval(update_data, 100);

function get_data() {
  return output;
}

module.exports.get_data_timing = get_data;

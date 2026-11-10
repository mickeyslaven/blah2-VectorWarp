'use strict';

(function () {
  async function request(path, options = {}) {
    const response = await fetchStatusResource(liveApiUrl(path),
      {cache: 'no-store', ...options}, 8000);
    const value = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(value.error || `Request failed (${response.status})`);
    return value;
  }

  async function renderLocalNodes() {
    const configuration = document.getElementById('configuration');
    if (!configuration || selectedNode() !== 'local') return;
    let panel = document.getElementById('local-radar-nodes');
    if (!panel) {
      panel = document.createElement('section');
      panel.id = 'local-radar-nodes';
      panel.className = 'local-node-settings';
      panel.innerHTML = `<div><h2>Local radar nodes</h2><p>Add another VectorWarp computer by its local IP address and API port. Identity, receiver capabilities, and RF channels are discovered automatically.</p></div>
        <form><label><span>IP address</span><input name="host" inputmode="decimal" autocomplete="off" placeholder="192.168.1.20" required></label>
        <label><span>Port</span><input name="port" inputmode="numeric" type="number" min="1" max="65535" value="3000" required></label>
        <label><span>Name (optional)</span><input name="label" autocomplete="off" placeholder="Roof radar"></label>
        <button class="button-secondary" type="submit">Add node</button></form>
        <p class="local-node-message" role="status" aria-live="polite"></p><div class="local-node-list"></div>`;
      configuration.prepend(panel);
      panel.querySelector('form').addEventListener('submit', async event => {
        event.preventDefault();
        const form = event.currentTarget;
        const button = form.querySelector('button');
        const message = panel.querySelector('.local-node-message');
        button.disabled = true; message.textContent = 'Discovering VectorWarp node…';
        try {
          const values = new FormData(form);
          await request('/api/federation/nodes', {method: 'POST',
            headers: {'Content-Type': 'application/json',
              'X-VectorWarp-Intent': 'local-node-add-v1'},
            body: JSON.stringify({host: values.get('host'), port: Number(values.get('port')),
              label: values.get('label')})});
          message.textContent = 'Node connected. Its channels are now available throughout VectorWarp.';
          form.reset(); form.elements.port.value = '3000';
          await renderLocalNodes();
        } catch (error) { message.textContent = error.message; }
        finally { button.disabled = false; }
      });
    }
    try {
      const data = await request('/api/federation/nodes');
      const nodes = (data.nodes || []).filter(node => !node.local);
      const list = panel.querySelector('.local-node-list');
      list.replaceChildren(...nodes.map(node => {
        const row = document.createElement('article');
        const copy = document.createElement('div');
        const title = document.createElement('strong'); title.textContent = node.label;
        const detail = document.createElement('small');
        detail.textContent = `VectorWarp identity: ${node.identity || node.id}`;
        copy.append(title, detail);
        const remove = document.createElement('button'); remove.type = 'button';
        remove.className = 'button-secondary'; remove.textContent = 'Remove';
        remove.addEventListener('click', async () => {
          if (!window.confirm(`Remove ${node.label} from this portal?`)) return;
          remove.disabled = true;
          try {
            await request(`/api/federation/nodes/${encodeURIComponent(node.id)}`, {
              method: 'DELETE', headers: {'X-VectorWarp-Intent': 'local-node-remove-v1'}});
            await renderLocalNodes();
          } catch (error) { panel.querySelector('.local-node-message').textContent = error.message; }
        });
        row.append(copy, remove); return row;
      }));
      if (!nodes.length) {
        const empty = document.createElement('p');
        empty.textContent = 'No additional local VectorWarp nodes connected.';
        list.appendChild(empty);
      }
    } catch (error) { panel.querySelector('.local-node-message').textContent = error.message; }
  }

  window.renderLocalNodes = renderLocalNodes;
  const start = () => renderLocalNodes().catch(() => {});
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
}());

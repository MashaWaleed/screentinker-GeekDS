'use strict';

/*
 * Password / secret fields on plugin schemas. GET never returns the value (empty string
 * so the form stays blank). PUT with blank / "***" keeps the stored value. authorization
 * is always treated as secret even if a plugin forgot to mark it.
 */

function secretNames(fields) {
  const names = new Set(['authorization']);
  for (const f of fields || []) {
    if (!f || typeof f.name !== 'string') continue;
    if (f.type === 'password' || f.secret === true) names.add(f.name);
  }
  return names;
}

function fieldsForDataSource(type) {
  try {
    const pluginRegistry = require('./registry');
    const spec = pluginRegistry.getDataSource(type);
    return (spec && spec.fields) || [];
  } catch {
    return [];
  }
}

function fieldsForWidget(type) {
  try {
    const pluginRegistry = require('./registry');
    const spec = pluginRegistry.getWidget(type);
    return (spec && spec.fields) || [];
  } catch {
    return [];
  }
}

function redactSecrets(cfg, fields) {
  const out = { ...(cfg && typeof cfg === 'object' ? cfg : {}) };
  for (const name of secretNames(fields)) {
    if (out[name] != null && out[name] !== '') out[name] = '';
  }
  return out;
}

function mergeSecrets(incoming, existing, fields) {
  const next = { ...(incoming && typeof incoming === 'object' ? incoming : {}) };
  const prev = existing && typeof existing === 'object' ? existing : {};
  for (const name of secretNames(fields)) {
    if (next[name] == null || next[name] === '' || next[name] === '***') {
      if (prev[name] != null && prev[name] !== '') next[name] = prev[name];
      else delete next[name];
    }
  }
  return next;
}

function redactConfigJson(json, fields) {
  let cfg;
  try { cfg = JSON.parse(json || '{}'); } catch { return json; }
  if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) return json;
  let changed = false;
  const out = { ...cfg };
  for (const name of secretNames(fields)) {
    if (out[name] != null && out[name] !== '') {
      out[name] = '';
      changed = true;
    }
  }
  return changed ? JSON.stringify(out) : json;
}

module.exports = {
  secretNames,
  redactSecrets,
  mergeSecrets,
  fieldsForDataSource,
  fieldsForWidget,
  redactConfigJson,
};

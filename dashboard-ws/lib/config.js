export const DEFAULT_VUE_URL = 'https://cdn.jsdelivr.net/npm/vue@3/dist/vue.esm-browser.js';

export function getDashboardConfig() {
  const runtime = (typeof window !== 'undefined' && window.__URFD_DASHBOARD_CONFIG__) || {};
  const hasConfig = runtime.__configLoaded === true;

  return {
    hasConfig,
    vueUrl: runtime.vueUrl || DEFAULT_VUE_URL,
    wsUrl: runtime.wsUrl || null,
  };
}

export function getVueRuntimeUrl() {
  return getDashboardConfig().vueUrl;
}

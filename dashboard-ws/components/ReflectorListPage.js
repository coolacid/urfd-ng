export default {
  name: 'ReflectorListPage',
  props: {
    state: { type: Object, required: true },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Directory</p>
          <h1>Reflectors</h1>
        </div>
      </div>

      <div class="panel">
        <div class="empty-state">
          <h2>Directory data is not part of the WebSocket notifier payload.</h2>
          <p>
            The live dashboard subscribes to reflector state, while the XLX directory list remains in the legacy dashboard.
            This page is intentionally left ready for a future directory-backed API or a proxy endpoint.
          </p>
        </div>
      </div>
    </section>
  `,
};

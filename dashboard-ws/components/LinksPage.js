export default {
  name: 'LinksPage',
  props: {
    state: { type: Object, required: true },
  },
  computed: {
    rows() {
      return [...(this.state.links || [])].sort((a, b) => (b.connect_time || 0) - (a.connect_time || 0));
    },
  },
  methods: {
    formatDuration(value) {
      if (!value) return '—';
      const seconds = Math.max(0, Math.floor(Date.now() / 1000) - Number(value));
      if (seconds < 60) return `${seconds}s`;
      if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`;
      return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
    },
  },
  template: `
    <section class="page">
      <div class="page-header">
        <div>
          <p class="eyebrow">Connected clients</p>
          <h1>Links</h1>
        </div>
      </div>

      <div class="panel">
        <div class="table-wrap">
          <table class="data-table">
            <thead>
              <tr>
                <th>Station</th>
                <th>Protocol</th>
                <th>Module</th>
                <th class="align-right">Linked for</th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="rows.length === 0">
                <td colspan="4" class="empty">There are no active links right now.</td>
              </tr>
              <tr v-for="item in rows" :key="item.callsign + ':' + item.protocol + ':' + item.on_module">
                <td class="callsign">{{ item.callsign }}</td>
                <td>{{ item.protocol || 'unknown' }}</td>
                <td><span class="module-chip">{{ item.on_module || '—' }}</span></td>
                <td class="align-right">{{ formatDuration(item.connect_time) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>
  `,
};

/* ═══════════════════════════════════════════════════════════════
   PureCVisor — modules/charts.js (F-1)
   Chart.js wrappers for time-series monitoring visualization.
   Reuses existing monHist rolling buffers populated by fetchAllMetrics().
   ═══════════════════════════════════════════════════════════════ */

window.PCV = window.PCV || {};
(function(PCV) {

/* Active chart instances keyed by canvas ID — reused on poll updates */
var pcvCharts = {};
window.pcvCharts = pcvCharts;

/* Cyberpunk-friendly color palette for multi-line series */
var PCV_CHART_COLORS = [
  '#00f0ff', '#00ff88', '#ff00aa', '#ffee00',
  '#ff6600', '#a78bfa', '#22d3ee', '#f472b6'
];

function pcvChartTheme() {
  /* Read CSS variables so charts respect the active theme */
  var s = getComputedStyle(document.documentElement);
  return {
    fg: (s.getPropertyValue('--fg') || '#e0f0ff').trim(),
    fg2: (s.getPropertyValue('--fg2') || '#5a6a8a').trim(),
    border: (s.getPropertyValue('--border') || '#1a1a3a').trim(),
    bg2: (s.getPropertyValue('--bg2') || '#0a0a14').trim()
  };
}

/**
 * pcvDestroyChart(id): Dispose chart instance if exists. Safe to call on no-op.
 */
function pcvDestroyChart(id) {
  if (pcvCharts[id]) {
    try { pcvCharts[id].destroy(); } catch (e) {}
    delete pcvCharts[id];
  }
}
window.pcvDestroyChart = pcvDestroyChart;

/**
 * pcvTimeSeries(canvasId, series, opts):
 *   series: [{label, data:[Number], color?}]
 *   opts: {title?, unit?, max?, height?, fill?}
 *
 * Creates or updates a multi-line time-series chart.
 * X-axis is index-based (rolling buffer), labels are relative seconds ago.
 */
function pcvTimeSeries(canvasId, series, opts) {
  if (typeof Chart === 'undefined') return;
  var el = document.getElementById(canvasId);
  if (!el) return;
  opts = opts || {};
  var theme = pcvChartTheme();
  var maxLen = Math.max.apply(null, series.map(function(s){return s.data.length;}).concat([1]));
  var labels = [];
  for (var i = 0; i < maxLen; i++) {
    var ago = (maxLen - 1 - i) * 5; /* fetchAllMetrics는 5초 간격 */
    labels.push(ago === 0 ? 'now' : '-' + ago + 's');
  }

  var datasets = series.map(function(s, idx) {
    var c = s.color || PCV_CHART_COLORS[idx % PCV_CHART_COLORS.length];
    /* 데이터 1~2개 시 점이 보이지 않는 문제: 짧은 시계열은 점을 키워 가시화 */
    var pr = s.data.length <= 2 ? 3 : 0;
    return {
      label: s.label,
      data: s.data,
      borderColor: c,
      backgroundColor: opts.fill ? c + '33' : 'transparent',
      borderWidth: 2,
      pointRadius: pr,
      pointHoverRadius: 4,
      tension: 0.3,
      fill: !!opts.fill
    };
  });

  /* If chart exists AND its canvas is still in DOM, update in place */
  if (pcvCharts[canvasId]) {
    var ch = pcvCharts[canvasId];
    if (ch.canvas && document.body.contains(ch.canvas) && ch.canvas === el) {
      ch.data.labels = labels;
      /* 데이터셋 개수가 변할 수 있으므로 전체 교체 */
      ch.data.datasets = datasets;
      ch.update('none');
      return ch;
    }
    /* Canvas가 DOM에서 제거됐거나 교체됨 → 기존 차트 폐기 후 재생성 */
    try { ch.destroy(); } catch (e) {}
    delete pcvCharts[canvasId];
  }

  if (opts.height) el.style.height = opts.height + 'px';

  var config = {
    type: 'line',
    data: { labels: labels, datasets: datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 0 },
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: {
          display: series.length > 1,
          position: 'top',
          align: 'end',
          labels: { color: theme.fg, font: { size: 10 }, boxWidth: 10, padding: 8 }
        },
        title: opts.title ? {
          display: true, text: opts.title, color: theme.fg,
          font: { size: 12, weight: 'bold' }, padding: { bottom: 8 }
        } : { display: false },
        tooltip: {
          backgroundColor: theme.bg2,
          titleColor: theme.fg,
          bodyColor: theme.fg,
          borderColor: theme.border,
          borderWidth: 1,
          callbacks: {
            label: function(ctx) {
              return ctx.dataset.label + ': ' + ctx.parsed.y.toFixed(1) + (opts.unit || '');
            }
          }
        }
      },
      scales: {
        x: {
          grid: { color: theme.border, drawBorder: false },
          ticks: { color: theme.fg2, font: { size: 9 }, maxTicksLimit: 6 }
        },
        y: {
          grid: { color: theme.border, drawBorder: false },
          ticks: {
            color: theme.fg2,
            font: { size: 9 },
            callback: function(v) { return v + (opts.unit || ''); }
          },
          min: 0,
          max: opts.max || undefined
        }
      }
    }
  };

  pcvCharts[canvasId] = new Chart(el, config);
  return pcvCharts[canvasId];
}
window.pcvTimeSeries = pcvTimeSeries;

/**
 * pcvDoughnut(canvasId, value, max, opts):
 *   value 0..max — single-arc gauge
 */
function pcvDoughnut(canvasId, value, max, opts) {
  if (typeof Chart === 'undefined') return;
  var el = document.getElementById(canvasId);
  if (!el) return;
  opts = opts || {};
  var theme = pcvChartTheme();
  var pct = Math.min(100, Math.max(0, (value / max) * 100));
  var color = opts.color ||
    (pct > 90 ? '#ff2266' : pct > 75 ? '#ffee00' : '#00ff88');

  if (pcvCharts[canvasId]) {
    var ch = pcvCharts[canvasId];
    ch.data.datasets[0].data = [value, Math.max(0, max - value)];
    ch.data.datasets[0].backgroundColor[0] = color;
    ch.update('none');
    return ch;
  }

  if (opts.height) el.style.height = opts.height + 'px';

  pcvCharts[canvasId] = new Chart(el, {
    type: 'doughnut',
    data: {
      datasets: [{
        data: [value, Math.max(0, max - value)],
        backgroundColor: [color, theme.border],
        borderWidth: 0,
        cutout: '75%'
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 300 },
      plugins: { legend: { display: false }, tooltip: { enabled: false } }
    }
  });
  return pcvCharts[canvasId];
}
window.pcvDoughnut = pcvDoughnut;

/**
 * pcvDestroyAllInContainer(parentEl):
 *   When re-rendering a section, dispose any charts whose canvas is no longer in DOM.
 */
function pcvDestroyAllInContainer(parentEl) {
  Object.keys(pcvCharts).forEach(function(id) {
    var el = document.getElementById(id);
    if (!el || (parentEl && !parentEl.contains(el))) {
      pcvDestroyChart(id);
    }
  });
}
window.pcvDestroyAllInContainer = pcvDestroyAllInContainer;

/* ── PCV.charts namespace export ──────────────────── */
PCV.charts = {
  instances: pcvCharts,
  colors: PCV_CHART_COLORS,
  theme: pcvChartTheme,
  destroy: pcvDestroyChart,
  timeSeries: pcvTimeSeries,
  doughnut: pcvDoughnut,
  destroyAllInContainer: pcvDestroyAllInContainer
};
})(window.PCV);

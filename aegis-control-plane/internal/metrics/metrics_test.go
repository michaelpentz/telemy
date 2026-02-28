package metrics

import (
	"strings"
	"testing"
)

func TestRenderIncludesCounterAndHistogramSeries(t *testing.T) {
	r := NewRegistry()
	r.IncCounter("aegis_job_runs_total", map[string]string{"job": "idempotency_ttl_cleanup", "status": "ok"})
	r.ObserveHistogram("aegis_job_duration_ms", 42, map[string]string{"job": "idempotency_ttl_cleanup"})

	out := r.Render()
	if !strings.Contains(out, `aegis_job_runs_total{job="idempotency_ttl_cleanup",status="ok"} 1`) {
		t.Fatalf("missing counter sample: %s", out)
	}
	if !strings.Contains(out, `aegis_job_duration_ms_count{job="idempotency_ttl_cleanup"} 1`) {
		t.Fatalf("missing histogram count sample: %s", out)
	}
}

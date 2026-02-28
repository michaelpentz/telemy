package jobs

import (
	"context"
	"log"
	"time"

	"github.com/telemyapp/aegis-control-plane/internal/metrics"
)

type Store interface {
	CleanupExpiredIdempotencyRecords(context.Context) error
	RollupLiveSessionDurations(context.Context) error
	ReconcileOutageFromHealth(context.Context) error
	UpsertUsageRollups(context.Context) error
}

type Runner struct {
	store Store
}

func NewRunner(store Store) *Runner {
	return &Runner{store: store}
}

func (r *Runner) Start(ctx context.Context) {
	go r.runEvery(ctx, "idempotency_ttl_cleanup", 5*time.Minute, r.store.CleanupExpiredIdempotencyRecords)
	go r.runEvery(ctx, "session_usage_rollup", 1*time.Minute, func(c context.Context) error {
		if err := r.store.RollupLiveSessionDurations(c); err != nil {
			return err
		}
		return r.store.UpsertUsageRollups(c)
	})
	go r.runEvery(ctx, "outage_reconciliation", 2*time.Minute, func(c context.Context) error {
		if err := r.store.ReconcileOutageFromHealth(c); err != nil {
			return err
		}
		return r.store.UpsertUsageRollups(c)
	})
}

func (r *Runner) runEvery(ctx context.Context, name string, interval time.Duration, fn func(context.Context) error) {
	r.runOnce(ctx, name, fn)
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			r.runOnce(ctx, name, fn)
		}
	}
}

func (r *Runner) runOnce(ctx context.Context, name string, fn func(context.Context) error) {
	start := time.Now()
	err := fn(ctx)
	durMs := float64(time.Since(start).Milliseconds())
	labels := map[string]string{
		"job": name,
	}
	if err != nil {
		log.Printf("metric=job_run name=%s status=error duration_ms=%d err=%q", name, int64(durMs), err.Error())
		labels["status"] = "error"
		metrics.Default().IncCounter("aegis_job_runs_total", labels)
		metrics.Default().ObserveHistogram("aegis_job_duration_ms", durMs, map[string]string{"job": name})
		return
	}
	log.Printf("metric=job_run name=%s status=ok duration_ms=%d", name, int64(durMs))
	labels["status"] = "ok"
	metrics.Default().IncCounter("aegis_job_runs_total", labels)
	metrics.Default().ObserveHistogram("aegis_job_duration_ms", durMs, map[string]string{"job": name})
}

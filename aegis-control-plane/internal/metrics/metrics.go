package metrics

import (
	"fmt"
	"net/http"
	"sort"
	"strings"
	"sync"
)

type metricType string

const (
	counterType   metricType = "counter"
	histogramType metricType = "histogram"
)

type descriptor struct {
	Name    string
	Help    string
	Type    metricType
	Buckets []float64
}

type counterSeries struct {
	Labels map[string]string
	Value  uint64
}

type histogramSeries struct {
	Labels       map[string]string
	Count        uint64
	Sum          float64
	BucketCounts []uint64
}

type Registry struct {
	mu         sync.RWMutex
	descs      map[string]descriptor
	counters   map[string]map[string]*counterSeries
	histograms map[string]map[string]*histogramSeries
}

func NewRegistry() *Registry {
	r := &Registry{
		descs:      make(map[string]descriptor),
		counters:   make(map[string]map[string]*counterSeries),
		histograms: make(map[string]map[string]*histogramSeries),
	}
	r.registerDefaults()
	return r
}

func (r *Registry) registerDefaults() {
	r.RegisterCounter("aegis_job_runs_total", "Total background job runs by job and status.")
	r.RegisterHistogram("aegis_job_duration_ms", "Background job duration in milliseconds by job.", []float64{10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000})
	r.RegisterCounter("aegis_relay_provision_total", "Total relay provision attempts by provider, region, and status.")
	r.RegisterHistogram("aegis_relay_provision_latency_ms", "Relay provision latency in milliseconds by provider, region, and status.", []float64{25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000, 120000})
	r.RegisterCounter("aegis_relay_deprovision_total", "Total relay deprovision attempts by provider, region, and status.")
	r.RegisterHistogram("aegis_relay_deprovision_latency_ms", "Relay deprovision latency in milliseconds by provider, region, and status.", []float64{25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000})
	r.RegisterCounter("aegis_aws_retries_total", "Total AWS retries by operation, region, and error code.")
	r.RegisterCounter("aegis_aws_retry_exhausted_total", "Total AWS operations that exhausted retry attempts by operation and region.")
	r.RegisterCounter("aegis_aws_operations_total", "Total AWS operation attempts by operation, region, and status.")
	r.RegisterHistogram("aegis_aws_operation_latency_ms", "AWS operation latency in milliseconds by operation, region, and status.", []float64{25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000, 120000})
}

func (r *Registry) RegisterCounter(name, help string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.descs[name] = descriptor{Name: name, Help: help, Type: counterType}
}

func (r *Registry) RegisterHistogram(name, help string, buckets []float64) {
	cp := append([]float64(nil), buckets...)
	sort.Float64s(cp)
	r.mu.Lock()
	defer r.mu.Unlock()
	r.descs[name] = descriptor{Name: name, Help: help, Type: histogramType, Buckets: cp}
}

func (r *Registry) IncCounter(name string, labels map[string]string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	desc, ok := r.descs[name]
	if !ok || desc.Type != counterType {
		return
	}
	seriesMap := r.counters[name]
	if seriesMap == nil {
		seriesMap = make(map[string]*counterSeries)
		r.counters[name] = seriesMap
	}
	key := labelsKey(labels)
	series := seriesMap[key]
	if series == nil {
		series = &counterSeries{Labels: cloneLabels(labels)}
		seriesMap[key] = series
	}
	series.Value++
}

func (r *Registry) ObserveHistogram(name string, value float64, labels map[string]string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	desc, ok := r.descs[name]
	if !ok || desc.Type != histogramType {
		return
	}
	seriesMap := r.histograms[name]
	if seriesMap == nil {
		seriesMap = make(map[string]*histogramSeries)
		r.histograms[name] = seriesMap
	}
	key := labelsKey(labels)
	series := seriesMap[key]
	if series == nil {
		series = &histogramSeries{
			Labels:       cloneLabels(labels),
			BucketCounts: make([]uint64, len(desc.Buckets)+1),
		}
		seriesMap[key] = series
	}
	bi := len(desc.Buckets)
	for i, bucket := range desc.Buckets {
		if value <= bucket {
			bi = i
			break
		}
	}
	series.BucketCounts[bi]++
	series.Count++
	series.Sum += value
}

func (r *Registry) Handler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
		_, _ = w.Write([]byte(r.Render()))
	})
}

func (r *Registry) Render() string {
	r.mu.RLock()
	defer r.mu.RUnlock()

	var b strings.Builder
	names := make([]string, 0, len(r.descs))
	for name := range r.descs {
		names = append(names, name)
	}
	sort.Strings(names)

	for _, name := range names {
		d := r.descs[name]
		b.WriteString("# HELP ")
		b.WriteString(name)
		b.WriteString(" ")
		b.WriteString(d.Help)
		b.WriteString("\n")
		b.WriteString("# TYPE ")
		b.WriteString(name)
		b.WriteString(" ")
		b.WriteString(string(d.Type))
		b.WriteString("\n")

		switch d.Type {
		case counterType:
			series := r.counters[name]
			if len(series) == 0 {
				continue
			}
			keys := sortedSeriesKeys(series)
			for _, key := range keys {
				s := series[key]
				writeMetricLine(&b, name, s.Labels, fmt.Sprintf("%d", s.Value))
			}
		case histogramType:
			series := r.histograms[name]
			if len(series) == 0 {
				continue
			}
			keys := sortedSeriesKeys(series)
			for _, key := range keys {
				s := series[key]
				var cumulative uint64
				for i, bucketCount := range s.BucketCounts {
					cumulative += bucketCount
					withLE := cloneLabels(s.Labels)
					if i < len(d.Buckets) {
						withLE["le"] = trimFloat(d.Buckets[i])
					} else {
						withLE["le"] = "+Inf"
					}
					writeMetricLine(&b, name+"_bucket", withLE, fmt.Sprintf("%d", cumulative))
				}
				writeMetricLine(&b, name+"_sum", s.Labels, trimFloat(s.Sum))
				writeMetricLine(&b, name+"_count", s.Labels, fmt.Sprintf("%d", s.Count))
			}
		}
	}

	return b.String()
}

func sortedSeriesKeys[T any](m map[string]*T) []string {
	out := make([]string, 0, len(m))
	for key := range m {
		out = append(out, key)
	}
	sort.Strings(out)
	return out
}

func writeMetricLine(b *strings.Builder, name string, labels map[string]string, value string) {
	b.WriteString(name)
	if len(labels) > 0 {
		b.WriteString("{")
		keys := make([]string, 0, len(labels))
		for key := range labels {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		for i, key := range keys {
			if i > 0 {
				b.WriteString(",")
			}
			b.WriteString(key)
			b.WriteString("=\"")
			b.WriteString(escapeLabel(labels[key]))
			b.WriteString("\"")
		}
		b.WriteString("}")
	}
	b.WriteString(" ")
	b.WriteString(value)
	b.WriteString("\n")
}

func labelsKey(labels map[string]string) string {
	if len(labels) == 0 {
		return ""
	}
	keys := make([]string, 0, len(labels))
	for key := range labels {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var b strings.Builder
	for _, key := range keys {
		b.WriteString(key)
		b.WriteString("=")
		b.WriteString(labels[key])
		b.WriteString(";")
	}
	return b.String()
}

func cloneLabels(in map[string]string) map[string]string {
	if len(in) == 0 {
		return map[string]string{}
	}
	out := make(map[string]string, len(in))
	for key, value := range in {
		out[key] = value
	}
	return out
}

func escapeLabel(v string) string {
	v = strings.ReplaceAll(v, "\\", "\\\\")
	v = strings.ReplaceAll(v, "\n", "\\n")
	v = strings.ReplaceAll(v, "\"", "\\\"")
	return v
}

func trimFloat(v float64) string {
	s := fmt.Sprintf("%.6f", v)
	s = strings.TrimRight(s, "0")
	s = strings.TrimRight(s, ".")
	if s == "" {
		return "0"
	}
	return s
}

var (
	defaultMu       sync.Mutex
	defaultRegistry = NewRegistry()
)

func Default() *Registry {
	defaultMu.Lock()
	defer defaultMu.Unlock()
	return defaultRegistry
}

func ResetDefaultForTest() {
	defaultMu.Lock()
	defer defaultMu.Unlock()
	defaultRegistry = NewRegistry()
}

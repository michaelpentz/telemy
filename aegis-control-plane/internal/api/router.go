package api

import (
	"context"
	"encoding/json"
	"net/http"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
	"github.com/google/uuid"

	"github.com/telemyapp/aegis-control-plane/internal/auth"
	"github.com/telemyapp/aegis-control-plane/internal/config"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

type Store interface {
	StartOrGetSession(rctx context.Context, in store.StartInput) (*model.Session, bool, error)
	ActivateProvisionedSession(rctx context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error)
	GetActiveSession(rctx context.Context, userID string) (*model.Session, error)
	GetSessionByID(rctx context.Context, userID, sessionID string) (*model.Session, error)
	StopSession(rctx context.Context, userID, sessionID string) (*model.Session, error)
	GetUsageCurrent(rctx context.Context, userID string) (*model.UsageCurrent, error)
	RecordRelayHealth(rctx context.Context, in store.RelayHealthInput) error
	ListRelayManifest(rctx context.Context) ([]model.RelayManifestEntry, error)
}

type Server struct {
	cfg         config.Config
	store       Store
	provisioner relay.Provisioner
}

func NewRouter(cfg config.Config, st Store, prov relay.Provisioner) http.Handler {
	s := &Server{cfg: cfg, store: st, provisioner: prov}
	r := chi.NewRouter()
	r.Use(middleware.RequestID)
	r.Use(middleware.RealIP)
	r.Use(middleware.Recoverer)
	// AWS relay provisioning can exceed tens of seconds during EC2 launch/wait.
	r.Use(middleware.Timeout(3 * time.Minute))

	r.Get("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok"})
	})
	r.Get("/metrics", metrics.Default().Handler().ServeHTTP)

	r.Route("/api/v1", func(v1 chi.Router) {
		v1.With(auth.Middleware(cfg.JWTSecret)).Group(func(authed chi.Router) {
			authed.Post("/relay/start", s.handleRelayStart)
			authed.Get("/relay/active", s.handleRelayActive)
			authed.Post("/relay/stop", s.handleRelayStop)
			authed.Get("/relay/manifest", s.handleRelayManifest)
			authed.Get("/usage/current", s.handleUsageCurrent)
		})

		v1.With(s.relaySharedAuth).Post("/relay/health", s.handleRelayHealth)
	})

	return r
}

func (s *Server) relaySharedAuth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Relay-Auth") != s.cfg.RelaySharedKey {
			writeAPIError(w, http.StatusUnauthorized, "unauthorized", "invalid relay auth")
			return
		}
		next.ServeHTTP(w, r)
	})
}

type apiError struct {
	Error struct {
		Code      string `json:"code"`
		Message   string `json:"message"`
		RequestID string `json:"request_id,omitempty"`
	} `json:"error"`
}

func writeAPIError(w http.ResponseWriter, status int, code, message string) {
	var payload apiError
	payload.Error.Code = code
	payload.Error.Message = message
	writeJSON(w, status, payload)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func parseIdempotencyKey(h string) (uuid.UUID, error) {
	return uuid.Parse(h)
}

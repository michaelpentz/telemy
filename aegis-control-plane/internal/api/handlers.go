package api

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"slices"
	"time"

	"github.com/telemyapp/aegis-control-plane/internal/auth"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

type relayStartRequest struct {
	RegionPreference string `json:"region_preference"`
	ClientContext    struct {
		OBSConnected bool   `json:"obs_connected"`
		Mode         string `json:"mode"`
		RequestedBy  string `json:"requested_by"`
	} `json:"client_context"`
}

type relayStopRequest struct {
	SessionID string `json:"session_id"`
	Reason    string `json:"reason"`
}

type relayHealthRequest struct {
	SessionID            string `json:"session_id"`
	InstanceID           string `json:"instance_id"`
	IngestActive         bool   `json:"ingest_active"`
	EgressActive         bool   `json:"egress_active"`
	SessionUptimeSeconds int    `json:"session_uptime_seconds"`
	ObservedAt           string `json:"observed_at"`
}

func (s *Server) handleRelayStart(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}

	idemRaw := r.Header.Get("Idempotency-Key")
	if idemRaw == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "Idempotency-Key is required")
		return
	}
	idem, err := parseIdempotencyKey(idemRaw)
	if err != nil {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "Idempotency-Key must be uuid-v4")
		return
	}

	var req relayStartRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "invalid JSON payload")
		return
	}

	region := s.resolveRegion(req.RegionPreference)
	requestedBy := req.ClientContext.RequestedBy
	if requestedBy == "" {
		requestedBy = "dashboard"
	}

	hash, err := store.HashJSON(req)
	if err != nil {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "failed to hash request")
		return
	}

	sess, created, err := s.store.StartOrGetSession(r.Context(), store.StartInput{
		UserID:         userID,
		Region:         region,
		RequestedBy:    requestedBy,
		IdempotencyKey: idem,
		RequestHash:    hash,
	})
	if err != nil {
		switch {
		case errors.Is(err, store.ErrIdempotencyMismatch):
			writeAPIError(w, http.StatusConflict, "idempotency_mismatch", "same key used with different payload")
		default:
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to start relay session")
		}
		return
	}

	if created {
		compensateStop := func() {
			if _, stopErr := s.store.StopSession(r.Context(), userID, sess.ID); stopErr != nil {
				log.Printf("relay_start_compensation stop_session_failed session_id=%s user_id=%s err=%v", sess.ID, userID, stopErr)
			}
		}

		provisionStart := time.Now()
		prov, err := s.provisioner.Provision(r.Context(), relay.ProvisionRequest{
			SessionID: sess.ID,
			UserID:    userID,
			Region:    sess.Region,
		})
		durMS := float64(time.Since(provisionStart).Milliseconds())
		labels := map[string]string{
			"provider": s.cfg.RelayProvider,
			"region":   sess.Region,
		}
		if err != nil {
			log.Printf("metric=relay_provision_latency_ms session_id=%s user_id=%s region=%s value=%d status=error", sess.ID, userID, sess.Region, time.Since(provisionStart).Milliseconds())
			labels["status"] = "error"
			metrics.Default().IncCounter("aegis_relay_provision_total", labels)
			metrics.Default().ObserveHistogram("aegis_relay_provision_latency_ms", durMS, labels)
			compensateStop()
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "relay provisioning failed")
			return
		}
		log.Printf("metric=relay_provision_latency_ms session_id=%s user_id=%s region=%s value=%d status=ok", sess.ID, userID, sess.Region, time.Since(provisionStart).Milliseconds())
		labels["status"] = "ok"
		metrics.Default().IncCounter("aegis_relay_provision_total", labels)
		metrics.Default().ObserveHistogram("aegis_relay_provision_latency_ms", durMS, labels)

		pairToken, err := generatePairToken(8)
		if err != nil {
			s.compensateRelayStartProvisioned(r.Context(), sess, userID, prov)
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "token generation failed")
			return
		}
		relayWSToken, err := generateRelayWSToken()
		if err != nil {
			s.compensateRelayStartProvisioned(r.Context(), sess, userID, prov)
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "token generation failed")
			return
		}

		activatedSess, err := s.store.ActivateProvisionedSession(r.Context(), store.ActivateProvisionedSessionInput{
			UserID:        userID,
			SessionID:     sess.ID,
			Region:        sess.Region,
			AWSInstanceID: prov.AWSInstanceID,
			AMIID:         prov.AMIID,
			InstanceType:  prov.InstanceType,
			PublicIP:      prov.PublicIP,
			SRTPort:       prov.SRTPort,
			WSURL:         prov.WSURL,
			PairToken:     pairToken,
			RelayWSToken:  relayWSToken,
		})
		if err != nil {
			s.compensateRelayStartProvisioned(r.Context(), sess, userID, prov)
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to activate relay session")
			return
		}
		sess = activatedSess
	}

	status := http.StatusOK
	if created {
		status = http.StatusCreated
	}
	writeJSON(w, status, map[string]any{"session": toSessionResponse(sess)})
}

func (s *Server) compensateRelayStartProvisioned(ctx context.Context, sess *model.Session, userID string, prov relay.ProvisionResult) {
	if deprovErr := s.provisioner.Deprovision(ctx, relay.DeprovisionRequest{
		SessionID:     sess.ID,
		UserID:        userID,
		Region:        sess.Region,
		AWSInstanceID: prov.AWSInstanceID,
	}); deprovErr != nil {
		log.Printf("relay_start_compensation deprovision_failed session_id=%s user_id=%s instance_id=%s err=%v", sess.ID, userID, prov.AWSInstanceID, deprovErr)
	}
	if _, stopErr := s.store.StopSession(ctx, userID, sess.ID); stopErr != nil {
		log.Printf("relay_start_compensation stop_session_failed session_id=%s user_id=%s err=%v", sess.ID, userID, stopErr)
	}
}

func (s *Server) handleRelayActive(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}
	sess, err := s.store.GetActiveSession(r.Context(), userID)
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to query active session")
		return
	}
	if sess == nil {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"session": toSessionResponse(sess)})
}

func (s *Server) handleRelayStop(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}

	var req relayStopRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.SessionID == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "session_id is required")
		return
	}

	curr, err := s.store.GetSessionByID(r.Context(), userID, req.SessionID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "session not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to query session")
		return
	}
	if curr.Status != model.SessionStopped && curr.RelayAWSInstanceID != "" {
		deprovStart := time.Now()
		if err := s.provisioner.Deprovision(r.Context(), relay.DeprovisionRequest{
			SessionID:     curr.ID,
			UserID:        curr.UserID,
			Region:        curr.Region,
			AWSInstanceID: curr.RelayAWSInstanceID,
		}); err != nil {
			durMS := float64(time.Since(deprovStart).Milliseconds())
			log.Printf("metric=relay_deprovision_latency_ms session_id=%s user_id=%s region=%s value=%d status=error", curr.ID, curr.UserID, curr.Region, time.Since(deprovStart).Milliseconds())
			labels := map[string]string{
				"provider": s.cfg.RelayProvider,
				"region":   curr.Region,
				"status":   "error",
			}
			metrics.Default().IncCounter("aegis_relay_deprovision_total", labels)
			metrics.Default().ObserveHistogram("aegis_relay_deprovision_latency_ms", durMS, labels)
			writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to terminate relay instance")
			return
		}
		durMS := float64(time.Since(deprovStart).Milliseconds())
		log.Printf("metric=relay_deprovision_latency_ms session_id=%s user_id=%s region=%s value=%d status=ok", curr.ID, curr.UserID, curr.Region, time.Since(deprovStart).Milliseconds())
		labels := map[string]string{
			"provider": s.cfg.RelayProvider,
			"region":   curr.Region,
			"status":   "ok",
		}
		metrics.Default().IncCounter("aegis_relay_deprovision_total", labels)
		metrics.Default().ObserveHistogram("aegis_relay_deprovision_latency_ms", durMS, labels)
	}

	sess, err := s.store.StopSession(r.Context(), userID, req.SessionID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "session not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to stop session")
		return
	}
	stoppedAt := time.Now().UTC().Format(time.RFC3339)
	if sess.StoppedAt != nil {
		stoppedAt = sess.StoppedAt.UTC().Format(time.RFC3339)
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"session_id": sess.ID,
		"status":     string(sess.Status),
		"stopped_at": stoppedAt,
	})
}

func (s *Server) handleRelayManifest(w http.ResponseWriter, r *http.Request) {
	type regionDef struct {
		Region              string `json:"region"`
		AMIID               string `json:"ami_id"`
		DefaultInstanceType string `json:"default_instance_type"`
		UpdatedAt           string `json:"updated_at"`
	}
	manifest, err := s.store.ListRelayManifest(r.Context())
	if err != nil {
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to read relay manifest")
		return
	}
	if len(manifest) == 0 {
		writeAPIError(w, http.StatusServiceUnavailable, "manifest_unavailable", "relay manifest is not configured")
		return
	}
	regions := make([]regionDef, 0, len(manifest))
	for _, entry := range manifest {
		regions = append(regions, regionDef{
			Region:              entry.Region,
			AMIID:               entry.AMIID,
			DefaultInstanceType: entry.DefaultInstanceType,
			UpdatedAt:           entry.UpdatedAt.UTC().Format(time.RFC3339),
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"regions": regions})
}

func (s *Server) handleUsageCurrent(w http.ResponseWriter, r *http.Request) {
	userID, ok := auth.UserIDFromContext(r.Context())
	if !ok {
		writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
		return
	}
	usage, err := s.store.GetUsageCurrent(r.Context(), userID)
	if err != nil {
		if errors.Is(err, store.ErrNotFound) {
			writeAPIError(w, http.StatusNotFound, "not_found", "user usage not found")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to query usage")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"plan_tier":         usage.PlanTier,
		"cycle_start":       usage.CycleStart.UTC().Format(time.RFC3339),
		"cycle_end":         usage.CycleEnd.UTC().Format(time.RFC3339),
		"included_seconds":  usage.IncludedSeconds,
		"consumed_seconds":  usage.ConsumedSeconds,
		"remaining_seconds": usage.RemainingSeconds,
		"overage_seconds":   usage.OverageSeconds,
	})
}

func (s *Server) handleRelayHealth(w http.ResponseWriter, r *http.Request) {
	var req relayHealthRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.SessionID == "" {
		writeAPIError(w, http.StatusBadRequest, "invalid_request", "invalid relay health payload")
		return
	}

	observedAt := time.Now().UTC()
	if req.ObservedAt != "" {
		t, err := time.Parse(time.RFC3339, req.ObservedAt)
		if err != nil {
			writeAPIError(w, http.StatusBadRequest, "invalid_request", "observed_at must be RFC3339")
			return
		}
		observedAt = t.UTC()
	}
	raw, _ := json.Marshal(req)

	err := s.store.RecordRelayHealth(r.Context(), store.RelayHealthInput{
		SessionID:            req.SessionID,
		ObservedAt:           observedAt,
		IngestActive:         req.IngestActive,
		EgressActive:         req.EgressActive,
		SessionUptimeSeconds: req.SessionUptimeSeconds,
		RawPayload:           raw,
	})
	if err != nil {
		if errors.Is(err, store.ErrRelayHealthRejected) {
			writeAPIError(w, http.StatusBadRequest, "invalid_request", "relay health rejected")
			return
		}
		writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to record relay health")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

func (s *Server) resolveRegion(pref string) string {
	if pref == "" || pref == "auto" {
		return s.cfg.DefaultRegion
	}
	if slices.Contains(s.cfg.SupportedRegion, pref) {
		return pref
	}
	return s.cfg.DefaultRegion
}

func toSessionResponse(sess *model.Session) map[string]any {
	resp := map[string]any{
		"session_id": sess.ID,
		"status":     string(sess.Status),
		"region":     sess.Region,
		"relay": map[string]any{
			"public_ip": sess.PublicIP,
			"srt_port":  sess.SRTPort,
			"ws_url":    sess.WSURL,
		},
		"credentials": map[string]any{
			"pair_token":     sess.PairToken,
			"relay_ws_token": sess.RelayWSToken,
		},
		"timers": map[string]any{
			"grace_window_seconds": sess.GraceWindowSeconds,
			"max_session_seconds":  sess.MaxSessionSeconds,
		},
	}
	return resp
}

func generatePairToken(length int) (string, error) {
	const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	if length <= 0 {
		return "", errors.New("invalid token length")
	}
	buf := make([]byte, length)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	out := make([]byte, length)
	for i := range buf {
		out[i] = alphabet[int(buf[i])%len(alphabet)]
	}
	return string(out), nil
}

func generateRelayWSToken() (string, error) {
	b := make([]byte, 24)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

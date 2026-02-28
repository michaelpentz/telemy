package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"

	"github.com/telemyapp/aegis-control-plane/internal/config"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

type mockStore struct {
	getSessionByIDFn         func(context.Context, string, string) (*model.Session, error)
	stopSessionFn            func(context.Context, string, string) (*model.Session, error)
	startOrGetSessionFn      func(context.Context, store.StartInput) (*model.Session, bool, error)
	activateSessionFn        func(context.Context, store.ActivateProvisionedSessionInput) (*model.Session, error)
	getActiveSessionFn       func(context.Context, string) (*model.Session, error)
	getUsageCurrentFn        func(context.Context, string) (*model.UsageCurrent, error)
	recordRelayHealthEventFn func(context.Context, store.RelayHealthInput) error
	listRelayManifestFn      func(context.Context) ([]model.RelayManifestEntry, error)
}

func (m *mockStore) StartOrGetSession(ctx context.Context, in store.StartInput) (*model.Session, bool, error) {
	if m.startOrGetSessionFn != nil {
		return m.startOrGetSessionFn(ctx, in)
	}
	return nil, false, nil
}

func (m *mockStore) ActivateProvisionedSession(ctx context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error) {
	if m.activateSessionFn != nil {
		return m.activateSessionFn(ctx, in)
	}
	return nil, nil
}

func (m *mockStore) GetActiveSession(ctx context.Context, userID string) (*model.Session, error) {
	if m.getActiveSessionFn != nil {
		return m.getActiveSessionFn(ctx, userID)
	}
	return nil, nil
}

func (m *mockStore) GetSessionByID(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	if m.getSessionByIDFn != nil {
		return m.getSessionByIDFn(ctx, userID, sessionID)
	}
	return nil, store.ErrNotFound
}

func (m *mockStore) StopSession(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	if m.stopSessionFn != nil {
		return m.stopSessionFn(ctx, userID, sessionID)
	}
	return nil, store.ErrNotFound
}

func (m *mockStore) GetUsageCurrent(ctx context.Context, userID string) (*model.UsageCurrent, error) {
	if m.getUsageCurrentFn != nil {
		return m.getUsageCurrentFn(ctx, userID)
	}
	return nil, store.ErrNotFound
}

func (m *mockStore) RecordRelayHealth(ctx context.Context, in store.RelayHealthInput) error {
	if m.recordRelayHealthEventFn != nil {
		return m.recordRelayHealthEventFn(ctx, in)
	}
	return nil
}

func (m *mockStore) ListRelayManifest(ctx context.Context) ([]model.RelayManifestEntry, error) {
	if m.listRelayManifestFn != nil {
		return m.listRelayManifestFn(ctx)
	}
	return nil, nil
}

type mockProvisioner struct {
	provisionFn   func(context.Context, relay.ProvisionRequest) (relay.ProvisionResult, error)
	deprovisionFn func(context.Context, relay.DeprovisionRequest) error
}

func (m *mockProvisioner) Provision(ctx context.Context, req relay.ProvisionRequest) (relay.ProvisionResult, error) {
	if m.provisionFn != nil {
		return m.provisionFn(ctx, req)
	}
	return relay.ProvisionResult{
		AWSInstanceID: "i-default",
		AMIID:         "ami-default",
		InstanceType:  "t4g.small",
		PublicIP:      "203.0.113.10",
		SRTPort:       9000,
		WSURL:         "wss://relay.default/ws",
	}, nil
}

func (m *mockProvisioner) Deprovision(ctx context.Context, req relay.DeprovisionRequest) error {
	if m.deprovisionFn != nil {
		return m.deprovisionFn(ctx, req)
	}
	return nil
}

func TestRelayStop_IdempotentAlreadyStoppedSkipsDeprovision(t *testing.T) {
	stoppedAt := time.Now().UTC()
	ms := &mockStore{
		getSessionByIDFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:                 "ses_1",
				UserID:             "usr_1",
				Status:             model.SessionStopped,
				RelayAWSInstanceID: "i-abc",
			}, nil
		},
		stopSessionFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:        "ses_1",
				UserID:    "usr_1",
				Status:    model.SessionStopped,
				StoppedAt: &stoppedAt,
			}, nil
		},
	}

	deprovCalls := 0
	mp := &mockProvisioner{
		deprovisionFn: func(_ context.Context, _ relay.DeprovisionRequest) error {
			deprovCalls++
			return nil
		},
	}

	router := NewRouter(testConfig(), ms, mp)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/stop", jsonBody(map[string]any{
		"session_id": "ses_1",
		"reason":     "user_requested",
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	if deprovCalls != 0 {
		t.Fatalf("expected no deprovision call for stopped session, got %d", deprovCalls)
	}
}

func TestRelayStop_ActiveSessionCallsDeprovisionThenStops(t *testing.T) {
	stoppedAt := time.Now().UTC()
	ms := &mockStore{
		getSessionByIDFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:                 "ses_2",
				UserID:             "usr_1",
				Status:             model.SessionActive,
				Region:             "us-east-1",
				RelayAWSInstanceID: "i-xyz",
			}, nil
		},
		stopSessionFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:        "ses_2",
				UserID:    "usr_1",
				Status:    model.SessionStopped,
				StoppedAt: &stoppedAt,
			}, nil
		},
	}

	deprovCalls := 0
	mp := &mockProvisioner{
		deprovisionFn: func(_ context.Context, req relay.DeprovisionRequest) error {
			deprovCalls++
			if req.AWSInstanceID != "i-xyz" {
				t.Fatalf("unexpected instance id: %s", req.AWSInstanceID)
			}
			return nil
		},
	}

	router := NewRouter(testConfig(), ms, mp)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/stop", jsonBody(map[string]any{
		"session_id": "ses_2",
		"reason":     "user_requested",
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	if deprovCalls != 1 {
		t.Fatalf("expected exactly one deprovision call, got %d", deprovCalls)
	}
}

func TestRelayStop_DeprovisionFailureReturns500(t *testing.T) {
	ms := &mockStore{
		getSessionByIDFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			return &model.Session{
				ID:                 "ses_3",
				UserID:             "usr_1",
				Status:             model.SessionActive,
				Region:             "us-east-1",
				RelayAWSInstanceID: "i-fail",
			}, nil
		},
		stopSessionFn: func(_ context.Context, _, _ string) (*model.Session, error) {
			t.Fatal("stop should not be called when deprovision fails")
			return nil, nil
		},
	}
	mp := &mockProvisioner{
		deprovisionFn: func(_ context.Context, _ relay.DeprovisionRequest) error {
			return context.DeadlineExceeded
		},
	}

	router := NewRouter(testConfig(), ms, mp)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/stop", jsonBody(map[string]any{
		"session_id": "ses_3",
		"reason":     "user_requested",
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("expected 500, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayStart_IdempotencyReplaySkipsProvisioning(t *testing.T) {
	idem := "8a849d0e-04eb-4a11-bf8a-6b8e5ea1572f"
	firstSession := &model.Session{
		ID:                 "ses_new",
		UserID:             "usr_1",
		Status:             model.SessionProvisioning,
		Region:             "us-east-1",
		SRTPort:            9000,
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}
	replayedSession := &model.Session{
		ID:                 "ses_new",
		UserID:             "usr_1",
		Status:             model.SessionActive,
		Region:             "us-east-1",
		RelayAWSInstanceID: "i-123",
		PublicIP:           "198.51.100.21",
		SRTPort:            9000,
		WSURL:              "wss://relay.test/ws",
		PairToken:          "PAIR1234",
		RelayWSToken:       "ws_token",
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}

	startCalls := 0
	activateCalls := 0
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, in store.StartInput) (*model.Session, bool, error) {
			startCalls++
			if in.IdempotencyKey.String() != idem {
				t.Fatalf("unexpected idempotency key: %s", in.IdempotencyKey.String())
			}
			if startCalls == 1 {
				return firstSession, true, nil
			}
			return replayedSession, false, nil
		},
		activateSessionFn: func(_ context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error) {
			activateCalls++
			return replayedSession, nil
		},
	}

	provisionCalls := 0
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, req relay.ProvisionRequest) (relay.ProvisionResult, error) {
			provisionCalls++
			if req.SessionID != "ses_new" {
				t.Fatalf("unexpected session id: %s", req.SessionID)
			}
			return relay.ProvisionResult{
				AWSInstanceID: "i-123",
				AMIID:         "ami-123",
				InstanceType:  "t4g.small",
				PublicIP:      "198.51.100.21",
				SRTPort:       9000,
				WSURL:         "wss://relay.test/ws",
			}, nil
		},
	}

	router := NewRouter(testConfig(), ms, mp)
	body := map[string]any{
		"region_preference": "us-east-1",
		"client_context": map[string]any{
			"requested_by": "dashboard",
		},
	}
	for i := 0; i < 2; i++ {
		req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(body))
		req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
		req.Header.Set("Idempotency-Key", idem)
		rr := httptest.NewRecorder()
		router.ServeHTTP(rr, req)

		if i == 0 && rr.Code != http.StatusCreated {
			t.Fatalf("first start expected 201, got %d body=%s", rr.Code, rr.Body.String())
		}
		if i == 1 && rr.Code != http.StatusOK {
			t.Fatalf("replay start expected 200, got %d body=%s", rr.Code, rr.Body.String())
		}
	}

	if startCalls != 2 {
		t.Fatalf("expected 2 start/get calls, got %d", startCalls)
	}
	if provisionCalls != 1 {
		t.Fatalf("expected 1 provision call, got %d", provisionCalls)
	}
	if activateCalls != 1 {
		t.Fatalf("expected 1 activation call, got %d", activateCalls)
	}
}

func TestRelayStart_DuplicateActiveSessionPreventsProvisioning(t *testing.T) {
	activeSession := &model.Session{
		ID:                 "ses_existing",
		UserID:             "usr_1",
		Status:             model.SessionActive,
		Region:             "eu-west-1",
		RelayAWSInstanceID: "i-existing",
		PublicIP:           "203.0.113.77",
		SRTPort:            9000,
		WSURL:              "wss://relay.existing/ws",
		PairToken:          "EXIST123",
		RelayWSToken:       "existing_ws_token",
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}

	activateCalls := 0
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, _ store.StartInput) (*model.Session, bool, error) {
			return activeSession, false, nil
		},
		activateSessionFn: func(_ context.Context, _ store.ActivateProvisionedSessionInput) (*model.Session, error) {
			activateCalls++
			return nil, nil
		},
	}

	provisionCalls := 0
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, _ relay.ProvisionRequest) (relay.ProvisionResult, error) {
			provisionCalls++
			return relay.ProvisionResult{}, nil
		},
	}

	router := NewRouter(testConfig(), ms, mp)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(map[string]any{
		"region_preference": "eu-west-1",
		"client_context": map[string]any{
			"requested_by": "dashboard",
		},
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	req.Header.Set("Idempotency-Key", "e699cf53-cdf9-44c6-835c-f867a8b6aa95")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200 for existing active session, got %d body=%s", rr.Code, rr.Body.String())
	}
	if provisionCalls != 0 {
		t.Fatalf("expected no provisioning for duplicate active session, got %d", provisionCalls)
	}
	if activateCalls != 0 {
		t.Fatalf("expected no activation for duplicate active session, got %d", activateCalls)
	}
}

func TestRelayStart_ProvisionFailureCompensatesByStoppingSession(t *testing.T) {
	createdSession := &model.Session{
		ID:                 "ses_prov_fail",
		UserID:             "usr_1",
		Status:             model.SessionProvisioning,
		Region:             "us-east-1",
		SRTPort:            9000,
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}

	stopCalls := 0
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, _ store.StartInput) (*model.Session, bool, error) {
			return createdSession, true, nil
		},
		stopSessionFn: func(_ context.Context, userID, sessionID string) (*model.Session, error) {
			stopCalls++
			if userID != "usr_1" || sessionID != "ses_prov_fail" {
				t.Fatalf("unexpected stop target user=%s session=%s", userID, sessionID)
			}
			return &model.Session{
				ID:     sessionID,
				UserID: userID,
				Status: model.SessionStopped,
			}, nil
		},
	}

	deprovCalls := 0
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, _ relay.ProvisionRequest) (relay.ProvisionResult, error) {
			return relay.ProvisionResult{}, context.DeadlineExceeded
		},
		deprovisionFn: func(_ context.Context, _ relay.DeprovisionRequest) error {
			deprovCalls++
			return nil
		},
	}

	router := NewRouter(testConfig(), ms, mp)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(map[string]any{
		"region_preference": "us-east-1",
		"client_context": map[string]any{
			"requested_by": "dashboard",
		},
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	req.Header.Set("Idempotency-Key", "b9e2bdb0-0ef2-46ba-8201-76558a3d5337")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("expected 500, got %d body=%s", rr.Code, rr.Body.String())
	}
	if stopCalls != 1 {
		t.Fatalf("expected 1 stop compensation call, got %d", stopCalls)
	}
	if deprovCalls != 0 {
		t.Fatalf("expected no deprovision on provision failure, got %d", deprovCalls)
	}
}

func TestRelayStart_ActivationFailureCompensatesByDeprovisionAndStoppingSession(t *testing.T) {
	createdSession := &model.Session{
		ID:                 "ses_activate_fail",
		UserID:             "usr_1",
		Status:             model.SessionProvisioning,
		Region:             "us-east-1",
		SRTPort:            9000,
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}

	stopCalls := 0
	activateCalls := 0
	ms := &mockStore{
		startOrGetSessionFn: func(_ context.Context, _ store.StartInput) (*model.Session, bool, error) {
			return createdSession, true, nil
		},
		activateSessionFn: func(_ context.Context, in store.ActivateProvisionedSessionInput) (*model.Session, error) {
			activateCalls++
			if in.SessionID != "ses_activate_fail" {
				t.Fatalf("unexpected activation session id: %s", in.SessionID)
			}
			return nil, context.Canceled
		},
		stopSessionFn: func(_ context.Context, userID, sessionID string) (*model.Session, error) {
			stopCalls++
			if userID != "usr_1" || sessionID != "ses_activate_fail" {
				t.Fatalf("unexpected stop target user=%s session=%s", userID, sessionID)
			}
			return &model.Session{
				ID:     sessionID,
				UserID: userID,
				Status: model.SessionStopped,
			}, nil
		},
	}

	deprovCalls := 0
	mp := &mockProvisioner{
		provisionFn: func(_ context.Context, _ relay.ProvisionRequest) (relay.ProvisionResult, error) {
			return relay.ProvisionResult{
				AWSInstanceID: "i-orphan-risk",
				AMIID:         "ami-123",
				InstanceType:  "t4g.small",
				PublicIP:      "198.51.100.50",
				SRTPort:       9000,
				WSURL:         "wss://relay.test/ws",
			}, nil
		},
		deprovisionFn: func(_ context.Context, req relay.DeprovisionRequest) error {
			deprovCalls++
			if req.SessionID != "ses_activate_fail" || req.AWSInstanceID != "i-orphan-risk" {
				t.Fatalf("unexpected deprovision request: %+v", req)
			}
			return nil
		},
	}

	router := NewRouter(testConfig(), ms, mp)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/start", jsonBody(map[string]any{
		"region_preference": "us-east-1",
		"client_context": map[string]any{
			"requested_by": "dashboard",
		},
	}))
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	req.Header.Set("Idempotency-Key", "d4717a10-f714-4ea7-8ee4-df2de023c868")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("expected 500, got %d body=%s", rr.Code, rr.Body.String())
	}
	if activateCalls != 1 {
		t.Fatalf("expected 1 activation call, got %d", activateCalls)
	}
	if deprovCalls != 1 {
		t.Fatalf("expected 1 deprovision compensation call, got %d", deprovCalls)
	}
	if stopCalls != 1 {
		t.Fatalf("expected 1 stop compensation call, got %d", stopCalls)
	}
}

func TestRelayManifest_ReturnsConfiguredEntries(t *testing.T) {
	ts := time.Date(2026, 2, 21, 18, 0, 0, 0, time.UTC)
	ms := &mockStore{
		listRelayManifestFn: func(_ context.Context) ([]model.RelayManifestEntry, error) {
			return []model.RelayManifestEntry{
				{
					Region:              "eu-west-1",
					AMIID:               "ami-0456efgh",
					DefaultInstanceType: "t4g.small",
					UpdatedAt:           ts,
				},
				{
					Region:              "us-east-1",
					AMIID:               "ami-0123abcd",
					DefaultInstanceType: "t4g.small",
					UpdatedAt:           ts,
				},
			}, nil
		},
	}

	router := NewRouter(testConfig(), ms, &mockProvisioner{})
	req := httptest.NewRequest(http.MethodGet, "/api/v1/relay/manifest", nil)
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	var body struct {
		Regions []map[string]any `json:"regions"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode body: %v", err)
	}
	if len(body.Regions) != 2 {
		t.Fatalf("expected 2 regions, got %d", len(body.Regions))
	}
}

func TestRelayManifest_EmptyManifestReturns503(t *testing.T) {
	ms := &mockStore{
		listRelayManifestFn: func(_ context.Context) ([]model.RelayManifestEntry, error) {
			return []model.RelayManifestEntry{}, nil
		},
	}

	router := NewRouter(testConfig(), ms, &mockProvisioner{})
	req := httptest.NewRequest(http.MethodGet, "/api/v1/relay/manifest", nil)
	req.Header.Set("Authorization", "Bearer "+testJWT(t, "test-secret", "usr_1"))
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayHealth_RejectedPayloadReturns400(t *testing.T) {
	ms := &mockStore{
		recordRelayHealthEventFn: func(_ context.Context, _ store.RelayHealthInput) error {
			return store.ErrRelayHealthRejected
		},
	}

	router := NewRouter(testConfig(), ms, &mockProvisioner{})
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/health", jsonBody(map[string]any{
		"session_id":             "ses_1",
		"instance_id":            "i-1",
		"ingest_active":          true,
		"egress_active":          true,
		"session_uptime_seconds": 12,
	}))
	req.Header.Set("X-Relay-Auth", "relay-key")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestRelayHealth_StoreFailureReturns500(t *testing.T) {
	ms := &mockStore{
		recordRelayHealthEventFn: func(_ context.Context, _ store.RelayHealthInput) error {
			return errors.New("db unavailable")
		},
	}

	router := NewRouter(testConfig(), ms, &mockProvisioner{})
	req := httptest.NewRequest(http.MethodPost, "/api/v1/relay/health", jsonBody(map[string]any{
		"session_id":             "ses_1",
		"instance_id":            "i-1",
		"ingest_active":          true,
		"egress_active":          true,
		"session_uptime_seconds": 12,
	}))
	req.Header.Set("X-Relay-Auth", "relay-key")
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("expected 500, got %d body=%s", rr.Code, rr.Body.String())
	}
}

func TestMetricsEndpoint_ExposesPrometheusPayload(t *testing.T) {
	metrics.ResetDefaultForTest()

	ms := &mockStore{}
	router := NewRouter(testConfig(), ms, &mockProvisioner{})
	req := httptest.NewRequest(http.MethodGet, "/metrics", nil)
	rr := httptest.NewRecorder()
	router.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", rr.Code, rr.Body.String())
	}
	body := rr.Body.String()
	if body == "" {
		t.Fatal("expected non-empty metrics body")
	}
	if !bytes.Contains(rr.Body.Bytes(), []byte("# TYPE aegis_job_runs_total counter")) {
		t.Fatalf("expected job counter type in metrics payload, body=%s", body)
	}
	if !bytes.Contains(rr.Body.Bytes(), []byte("# TYPE aegis_relay_provision_latency_ms histogram")) {
		t.Fatalf("expected provision histogram type in metrics payload, body=%s", body)
	}
}

func testConfig() config.Config {
	return config.Config{
		JWTSecret:       "test-secret",
		RelaySharedKey:  "relay-key",
		DefaultRegion:   "us-east-1",
		SupportedRegion: []string{"us-east-1", "eu-west-1"},
		AWSInstanceType: "t4g.small",
	}
}

func testJWT(t *testing.T, secret, userID string) string {
	t.Helper()
	claims := jwt.MapClaims{
		"uid": userID,
		"exp": time.Now().Add(1 * time.Hour).Unix(),
		"iat": time.Now().Unix(),
	}
	tok := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	signed, err := tok.SignedString([]byte(secret))
	if err != nil {
		t.Fatalf("sign jwt: %v", err)
	}
	return signed
}

func jsonBody(v any) *bytes.Reader {
	b, _ := json.Marshal(v)
	return bytes.NewReader(b)
}

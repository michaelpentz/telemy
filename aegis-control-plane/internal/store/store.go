package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/telemyapp/aegis-control-plane/internal/model"
)

var (
	ErrNotFound            = errors.New("not found")
	ErrIdempotencyMismatch = errors.New("idempotency mismatch")
	ErrRelayHealthRejected = errors.New("relay health rejected")
)

type Store struct {
	db DB
}

type DB interface {
	QueryRow(ctx context.Context, sql string, args ...any) pgx.Row
	Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error)
	Exec(ctx context.Context, sql string, arguments ...any) (pgconn.CommandTag, error)
	BeginTx(ctx context.Context, txOptions pgx.TxOptions) (pgx.Tx, error)
}

type StartInput struct {
	UserID         string
	Region         string
	RequestedBy    string
	IdempotencyKey uuid.UUID
	RequestHash    string
}

type RelayHealthInput struct {
	SessionID            string
	ObservedAt           time.Time
	IngestActive         bool
	EgressActive         bool
	SessionUptimeSeconds int
	RawPayload           json.RawMessage
}

type ActivateProvisionedSessionInput struct {
	UserID        string
	SessionID     string
	Region        string
	AWSInstanceID string
	AMIID         string
	InstanceType  string
	PublicIP      string
	SRTPort       int
	WSURL         string
	PairToken     string
	RelayWSToken  string
}

func New(db DB) *Store {
	return &Store{db: db}
}

func HashJSON(v any) (string, error) {
	b, err := json.Marshal(v)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:]), nil
}

func (s *Store) GetActiveSession(ctx context.Context, userID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.relay_instance_id, ''), coalesce(ri.aws_instance_id, ''), s.status, s.region, s.pair_token, s.relay_ws_token,
       coalesce(ri.public_ip::text, ''), coalesce(ri.srt_port, 9000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
where user_id = $1 and status in ('provisioning', 'active', 'grace')
order by s.created_at desc
limit 1`

	var out model.Session
	var relayInstanceID string
	var stoppedAt *time.Time
	if err := s.db.QueryRow(ctx, q, userID).Scan(
		&out.ID, &out.UserID, &relayInstanceID, &out.RelayAWSInstanceID, &out.Status, &out.Region, &out.PairToken, &out.RelayWSToken,
		&out.PublicIP, &out.SRTPort, &out.WSURL,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayInstanceID = strPtr(relayInstanceID)
	return &out, nil
}

func (s *Store) StartOrGetSession(ctx context.Context, in StartInput) (*model.Session, bool, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, false, err
	}
	defer tx.Rollback(ctx)

	var storedHash string
	var storedResp []byte
	const idemLookup = `
select request_hash, response_json
from idempotency_records
where user_id = $1 and endpoint = '/api/v1/relay/start' and idempotency_key = $2 and expires_at > now()`
	err = tx.QueryRow(ctx, idemLookup, in.UserID, in.IdempotencyKey).Scan(&storedHash, &storedResp)
	if err == nil {
		if storedHash != in.RequestHash {
			return nil, false, ErrIdempotencyMismatch
		}
		var sess model.Session
		if err := json.Unmarshal(storedResp, &sess); err != nil {
			return nil, false, err
		}
		if err := tx.Commit(ctx); err != nil {
			return nil, false, err
		}
		return &sess, false, nil
	}
	if err != nil && !errors.Is(err, pgx.ErrNoRows) {
		return nil, false, err
	}

	existing, err := s.getActiveSessionTx(ctx, tx, in.UserID)
	if err != nil {
		return nil, false, err
	}
	if existing != nil {
		if err := s.persistIdempotencyRecord(ctx, tx, in, existing); err != nil {
			return nil, false, err
		}
		if err := tx.Commit(ctx); err != nil {
			return nil, false, err
		}
		return existing, false, nil
	}

	newID := "ses_" + uuid.NewString()
	now := time.Now().UTC()
	const insertSession = `
insert into sessions
  (id, user_id, status, region, idempotency_key, requested_by, pair_token, relay_ws_token, started_at, max_session_seconds, grace_window_seconds, duration_seconds, reconciled_seconds, created_at, updated_at)
values
  ($1, $2, 'provisioning', $3, $4, $5, '', '', $6, 57600, 600, 0, 0, $6, $6)`
	if _, err := tx.Exec(ctx, insertSession, newID, in.UserID, in.Region, in.IdempotencyKey, in.RequestedBy, now); err != nil {
		return nil, false, err
	}

	sess := &model.Session{
		ID:                 newID,
		UserID:             in.UserID,
		Status:             model.SessionProvisioning,
		Region:             in.Region,
		SRTPort:            9000,
		StartedAt:          now,
		GraceWindowSeconds: 600,
		MaxSessionSeconds:  57600,
	}

	if err := s.persistIdempotencyRecord(ctx, tx, in, sess); err != nil {
		return nil, false, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, false, err
	}
	return sess, true, nil
}

func (s *Store) getActiveSessionTx(ctx context.Context, tx pgx.Tx, userID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.relay_instance_id, ''), coalesce(ri.aws_instance_id, ''), s.status, s.region, s.pair_token, s.relay_ws_token,
       coalesce(ri.public_ip::text, ''), coalesce(ri.srt_port, 9000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
where s.user_id = $1 and s.status in ('provisioning', 'active', 'grace')
order by s.created_at desc
limit 1`
	var out model.Session
	var relayInstanceID string
	var stoppedAt *time.Time
	if err := tx.QueryRow(ctx, q, userID).Scan(
		&out.ID, &out.UserID, &relayInstanceID, &out.RelayAWSInstanceID, &out.Status, &out.Region, &out.PairToken, &out.RelayWSToken,
		&out.PublicIP, &out.SRTPort, &out.WSURL,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, nil
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayInstanceID = strPtr(relayInstanceID)
	return &out, nil
}

func (s *Store) ActivateProvisionedSession(ctx context.Context, in ActivateProvisionedSessionInput) (*model.Session, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)

	relayID := "rly_" + uuid.NewString()
	now := time.Now().UTC()
	const insertRelay = `
insert into relay_instances
  (id, session_id, aws_instance_id, region, ami_id, instance_type, public_ip, srt_port, ws_url, state, launched_at, created_at)
values
  ($1, $2, $3, $4, $5, $6, $7::inet, $8, $9, 'running', $10, $10)`
	if _, err := tx.Exec(ctx, insertRelay,
		relayID, in.SessionID, in.AWSInstanceID, in.Region, in.AMIID, in.InstanceType, in.PublicIP, in.SRTPort, in.WSURL, now,
	); err != nil {
		return nil, err
	}

	const updateSession = `
update sessions
set relay_instance_id = $3,
    status = 'active',
    pair_token = $4,
    relay_ws_token = $5,
    updated_at = now()
where user_id = $1 and id = $2 and status = 'provisioning'`
	tag, err := tx.Exec(ctx, updateSession, in.UserID, in.SessionID, relayID, in.PairToken, in.RelayWSToken)
	if err != nil {
		return nil, err
	}
	if tag.RowsAffected() == 0 {
		return nil, ErrNotFound
	}

	sess, err := s.getSessionByIDTx(ctx, tx, in.UserID, in.SessionID)
	if err != nil {
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return sess, nil
}

func (s *Store) getSessionByIDTx(ctx context.Context, tx pgx.Tx, userID, sessionID string) (*model.Session, error) {
	const q = `
select s.id, s.user_id, coalesce(s.relay_instance_id, ''), coalesce(ri.aws_instance_id, ''), s.status, s.region, s.pair_token, s.relay_ws_token,
       coalesce(ri.public_ip::text, ''), coalesce(ri.srt_port, 9000), coalesce(ri.ws_url, ''),
       s.started_at, s.stopped_at, s.duration_seconds, s.grace_window_seconds, s.max_session_seconds
from sessions s
left join relay_instances ri on ri.id = s.relay_instance_id
where s.user_id = $1 and s.id = $2
limit 1`
	var out model.Session
	var relayInstanceID string
	var stoppedAt *time.Time
	if err := tx.QueryRow(ctx, q, userID, sessionID).Scan(
		&out.ID, &out.UserID, &relayInstanceID, &out.RelayAWSInstanceID, &out.Status, &out.Region, &out.PairToken, &out.RelayWSToken,
		&out.PublicIP, &out.SRTPort, &out.WSURL,
		&out.StartedAt, &stoppedAt, &out.DurationSeconds, &out.GraceWindowSeconds, &out.MaxSessionSeconds,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	out.StoppedAt = stoppedAt
	out.RelayInstanceID = strPtr(relayInstanceID)
	return &out, nil
}

func (s *Store) GetSessionByID(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)
	sess, err := s.getSessionByIDTx(ctx, tx, userID, sessionID)
	if err != nil {
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return sess, nil
}

func (s *Store) persistIdempotencyRecord(ctx context.Context, tx pgx.Tx, in StartInput, sess *model.Session) error {
	resp, err := json.Marshal(sess)
	if err != nil {
		return err
	}
	const q = `
insert into idempotency_records
  (user_id, endpoint, idempotency_key, request_hash, response_json, session_id, created_at, expires_at)
values
  ($1, '/api/v1/relay/start', $2, $3, $4, $5, now(), now() + interval '1 hour')
on conflict (user_id, endpoint, idempotency_key)
do update set response_json = excluded.response_json, session_id = excluded.session_id`
	_, err = tx.Exec(ctx, q, in.UserID, in.IdempotencyKey, in.RequestHash, resp, sess.ID)
	return err
}

func (s *Store) StopSession(ctx context.Context, userID, sessionID string) (*model.Session, error) {
	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(ctx)

	curr, err := s.getSessionByIDTx(ctx, tx, userID, sessionID)
	if err != nil {
		return nil, err
	}
	if curr.Status != model.SessionStopped {
		const stopQ = `
update sessions
set status = 'stopped', stopped_at = now(), updated_at = now()
where user_id = $1 and id = $2 and status in ('provisioning', 'active', 'grace')`
		tag, err := tx.Exec(ctx, stopQ, userID, sessionID)
		if err != nil {
			return nil, err
		}
		if tag.RowsAffected() == 0 {
			return nil, ErrNotFound
		}
		if curr.RelayInstanceID != nil {
			const relayQ = `
update relay_instances
set state = 'terminated', terminated_at = coalesce(terminated_at, now())
where id = $1`
			if _, err := tx.Exec(ctx, relayQ, *curr.RelayInstanceID); err != nil {
				return nil, err
			}
		}
	}

	out, err := s.getSessionByIDTx(ctx, tx, userID, sessionID)
	if err != nil {
		return nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Store) GetUsageCurrent(ctx context.Context, userID string) (*model.UsageCurrent, error) {
	const q = `
select
  u.plan_tier,
  u.cycle_start_at,
  u.cycle_end_at,
  u.included_seconds,
  coalesce(sum(ur.billable_seconds), 0) as consumed_seconds
from users u
left join usage_records ur
  on ur.user_id = u.id
 and ur.cycle_start_at = u.cycle_start_at
 and ur.cycle_end_at = u.cycle_end_at
where u.id = $1
group by u.plan_tier, u.cycle_start_at, u.cycle_end_at, u.included_seconds`
	var out model.UsageCurrent
	if err := s.db.QueryRow(ctx, q, userID).Scan(
		&out.PlanTier, &out.CycleStart, &out.CycleEnd, &out.IncludedSeconds, &out.ConsumedSeconds,
	); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, err
	}
	out.RemainingSeconds = max(out.IncludedSeconds-out.ConsumedSeconds, 0)
	out.OverageSeconds = max(out.ConsumedSeconds-out.IncludedSeconds, 0)
	return &out, nil
}

func (s *Store) RecordRelayHealth(ctx context.Context, in RelayHealthInput) error {
	const q = `
insert into relay_health_events
  (session_id, relay_instance_id, observed_at, ingest_active, egress_active, session_uptime_seconds, payload_json, created_at)
select
  s.id, s.relay_instance_id, $2, $3, $4, $5, $6, now()
from sessions s
where s.id = $1 and s.relay_instance_id is not null`
	tag, err := s.db.Exec(ctx, q, in.SessionID, in.ObservedAt, in.IngestActive, in.EgressActive, in.SessionUptimeSeconds, in.RawPayload)
	if err != nil {
		return err
	}
	if tag.RowsAffected() == 0 {
		return fmt.Errorf("%w: no relay_instance bound for session", ErrRelayHealthRejected)
	}

	_, err = s.db.Exec(ctx, `update relay_instances ri set last_health_at = $2 where ri.id = (select relay_instance_id from sessions where id = $1)`, in.SessionID, in.ObservedAt)
	return err
}

func (s *Store) ListRelayManifest(ctx context.Context) ([]model.RelayManifestEntry, error) {
	const q = `
select region, ami_id, default_instance_type, updated_at
from relay_manifests
order by region asc`

	rows, err := s.db.Query(ctx, q)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]model.RelayManifestEntry, 0)
	for rows.Next() {
		var e model.RelayManifestEntry
		if err := rows.Scan(&e.Region, &e.AMIID, &e.DefaultInstanceType, &e.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Store) UpsertRelayManifest(ctx context.Context, entries []model.RelayManifestEntry) error {
	if len(entries) == 0 {
		return nil
	}

	tx, err := s.db.BeginTx(ctx, pgx.TxOptions{})
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)

	const q = `
insert into relay_manifests (region, ami_id, default_instance_type, updated_at)
values ($1, $2, $3, now())
on conflict (region)
do update set
  ami_id = excluded.ami_id,
  default_instance_type = excluded.default_instance_type,
  updated_at = now()`
	for _, e := range entries {
		if _, err := tx.Exec(ctx, q, e.Region, e.AMIID, e.DefaultInstanceType); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}

func (s *Store) CleanupExpiredIdempotencyRecords(ctx context.Context) error {
	_, err := s.db.Exec(ctx, `delete from idempotency_records where expires_at <= now()`)
	return err
}

func (s *Store) RollupLiveSessionDurations(ctx context.Context) error {
	const q = `
update sessions
set duration_seconds = greatest(
      duration_seconds,
      floor(extract(epoch from (now() - started_at)))::integer
    ),
    updated_at = now()
where status in ('active', 'grace')
  and started_at <= now()`
	_, err := s.db.Exec(ctx, q)
	return err
}

func (s *Store) ReconcileOutageFromHealth(ctx context.Context) error {
	const q = `
with latest as (
  select distinct on (session_id)
    session_id,
    session_uptime_seconds
  from relay_health_events
  order by session_id, observed_at desc, id desc
)
update sessions s
set reconciled_seconds = greatest(s.reconciled_seconds, latest.session_uptime_seconds),
    duration_seconds = greatest(s.duration_seconds, latest.session_uptime_seconds),
    updated_at = now()
from latest
where s.id = latest.session_id
  and s.status in ('active', 'grace', 'stopped')`
	_, err := s.db.Exec(ctx, q)
	return err
}

func (s *Store) UpsertUsageRollups(ctx context.Context) error {
	const q = `
insert into usage_records
  (id, user_id, session_id, cycle_start_at, cycle_end_at, measured_seconds, reconciled_seconds, billable_seconds, overage_seconds, created_at, updated_at)
select
  'use_' || s.id,
  s.user_id,
  s.id,
  u.cycle_start_at,
  u.cycle_end_at,
  s.duration_seconds,
  s.reconciled_seconds,
  greatest(s.duration_seconds, s.reconciled_seconds),
  0,
  now(),
  now()
from sessions s
join users u on u.id = s.user_id
where s.status in ('active', 'grace', 'stopped')
  and s.started_at >= u.cycle_start_at
  and s.started_at <= u.cycle_end_at
on conflict (id)
do update set
  measured_seconds = excluded.measured_seconds,
  reconciled_seconds = excluded.reconciled_seconds,
  billable_seconds = excluded.billable_seconds,
  updated_at = now()`
	_, err := s.db.Exec(ctx, q)
	return err
}

func strPtr(v string) *string {
	if v == "" {
		return nil
	}
	return &v
}

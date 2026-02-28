package store

import (
	"context"
	"regexp"
	"testing"
	"time"

	pgxmock "github.com/pashagolub/pgxmock/v4"

	"github.com/telemyapp/aegis-control-plane/internal/model"
)

func TestStopSession_AlreadyStopped_Idempotent(t *testing.T) {
	mock, err := pgxmock.NewPool()
	if err != nil {
		t.Fatalf("pgxmock pool: %v", err)
	}
	defer mock.Close()

	stoppedAt := time.Now().UTC()
	queryPrefix := "select s.id, s.user_id, coalesce(s.relay_instance_id, ''), coalesce(ri.aws_instance_id, ''), s.status, s.region, s.pair_token, s.relay_ws_token,"

	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(queryPrefix)).
		WithArgs("usr_1", "ses_1").
		WillReturnRows(sessionRow("ses_1", "usr_1", "rly_1", "i-abc", string(model.SessionStopped), stoppedAt))
	mock.ExpectQuery(regexp.QuoteMeta(queryPrefix)).
		WithArgs("usr_1", "ses_1").
		WillReturnRows(sessionRow("ses_1", "usr_1", "rly_1", "i-abc", string(model.SessionStopped), stoppedAt))
	mock.ExpectCommit()

	s := New(mock)
	out, err := s.StopSession(context.Background(), "usr_1", "ses_1")
	if err != nil {
		t.Fatalf("StopSession returned err: %v", err)
	}
	if out.Status != model.SessionStopped {
		t.Fatalf("expected stopped status, got %s", out.Status)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatalf("unmet expectations: %v", err)
	}
}

func TestStopSession_Active_TransitionsAndTerminatesRelay(t *testing.T) {
	mock, err := pgxmock.NewPool()
	if err != nil {
		t.Fatalf("pgxmock pool: %v", err)
	}
	defer mock.Close()

	startedAt := time.Now().UTC().Add(-5 * time.Minute)
	stoppedAt := time.Now().UTC()
	activeRow := sessionRowWithTimes("ses_2", "usr_1", "rly_2", "i-xyz", string(model.SessionActive), startedAt, nil)
	stoppedRow := sessionRowWithTimes("ses_2", "usr_1", "rly_2", "i-xyz", string(model.SessionStopped), startedAt, &stoppedAt)
	queryPrefix := "select s.id, s.user_id, coalesce(s.relay_instance_id, ''), coalesce(ri.aws_instance_id, ''), s.status, s.region, s.pair_token, s.relay_ws_token,"

	mock.ExpectBegin()
	mock.ExpectQuery(regexp.QuoteMeta(queryPrefix)).
		WithArgs("usr_1", "ses_2").
		WillReturnRows(activeRow)
	mock.ExpectExec(regexp.QuoteMeta("update sessions")).
		WithArgs("usr_1", "ses_2").
		WillReturnResult(pgxmock.NewResult("UPDATE", 1))
	mock.ExpectExec(regexp.QuoteMeta("update relay_instances")).
		WithArgs("rly_2").
		WillReturnResult(pgxmock.NewResult("UPDATE", 1))
	mock.ExpectQuery(regexp.QuoteMeta(queryPrefix)).
		WithArgs("usr_1", "ses_2").
		WillReturnRows(stoppedRow)
	mock.ExpectCommit()

	s := New(mock)
	out, err := s.StopSession(context.Background(), "usr_1", "ses_2")
	if err != nil {
		t.Fatalf("StopSession returned err: %v", err)
	}
	if out.Status != model.SessionStopped {
		t.Fatalf("expected stopped status, got %s", out.Status)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatalf("unmet expectations: %v", err)
	}
}

func TestCleanupExpiredIdempotencyRecords(t *testing.T) {
	mock, err := pgxmock.NewPool()
	if err != nil {
		t.Fatalf("pgxmock pool: %v", err)
	}
	defer mock.Close()

	mock.ExpectExec(regexp.QuoteMeta("delete from idempotency_records where expires_at <= now()")).
		WillReturnResult(pgxmock.NewResult("DELETE", 2))

	s := New(mock)
	if err := s.CleanupExpiredIdempotencyRecords(context.Background()); err != nil {
		t.Fatalf("CleanupExpiredIdempotencyRecords returned err: %v", err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatalf("unmet expectations: %v", err)
	}
}

func TestRollupJobsExecutes(t *testing.T) {
	mock, err := pgxmock.NewPool()
	if err != nil {
		t.Fatalf("pgxmock pool: %v", err)
	}
	defer mock.Close()

	mock.ExpectExec(regexp.QuoteMeta("update sessions")).
		WillReturnResult(pgxmock.NewResult("UPDATE", 1))
	mock.ExpectExec(regexp.QuoteMeta("with latest as")).
		WillReturnResult(pgxmock.NewResult("UPDATE", 1))
	mock.ExpectExec(regexp.QuoteMeta("insert into usage_records")).
		WillReturnResult(pgxmock.NewResult("INSERT", 1))

	s := New(mock)
	if err := s.RollupLiveSessionDurations(context.Background()); err != nil {
		t.Fatalf("RollupLiveSessionDurations returned err: %v", err)
	}
	if err := s.ReconcileOutageFromHealth(context.Background()); err != nil {
		t.Fatalf("ReconcileOutageFromHealth returned err: %v", err)
	}
	if err := s.UpsertUsageRollups(context.Background()); err != nil {
		t.Fatalf("UpsertUsageRollups returned err: %v", err)
	}
	if err := mock.ExpectationsWereMet(); err != nil {
		t.Fatalf("unmet expectations: %v", err)
	}
}

func sessionRow(sessionID, userID, relayID, awsID, status string, stoppedAt time.Time) *pgxmock.Rows {
	return sessionRowWithTimes(sessionID, userID, relayID, awsID, status, time.Now().UTC(), &stoppedAt)
}

func sessionRowWithTimes(sessionID, userID, relayID, awsID, status string, startedAt time.Time, stoppedAt *time.Time) *pgxmock.Rows {
	cols := []string{
		"id", "user_id", "relay_instance_id", "aws_instance_id", "status", "region", "pair_token", "relay_ws_token",
		"public_ip", "srt_port", "ws_url", "started_at", "stopped_at", "duration_seconds", "grace_window_seconds", "max_session_seconds",
	}
	return pgxmock.NewRows(cols).AddRow(
		sessionID, userID, relayID, awsID, status, "us-east-1", "ABCDEFGH", "relaytoken",
		"203.0.113.10", 9000, "wss://203.0.113.10:7443/telemetry", startedAt, stoppedAt, 120, 600, 57600,
	)
}

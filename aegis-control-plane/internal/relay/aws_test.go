package relay

import (
	"context"
	"errors"
	"testing"

	"github.com/aws/smithy-go"
)

func TestShouldIgnoreTerminateError(t *testing.T) {
	tests := []struct {
		name string
		err  error
		want bool
	}{
		{
			name: "instance not found",
			err:  &smithy.GenericAPIError{Code: "InvalidInstanceID.NotFound", Message: "missing"},
			want: true,
		},
		{
			name: "incorrect instance state",
			err:  &smithy.GenericAPIError{Code: "IncorrectInstanceState", Message: "already terminated"},
			want: true,
		},
		{
			name: "other aws error",
			err:  &smithy.GenericAPIError{Code: "RequestLimitExceeded", Message: "throttle"},
			want: false,
		},
		{
			name: "non aws error",
			err:  errors.New("boom"),
			want: false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := shouldIgnoreTerminateError(tt.err)
			if got != tt.want {
				t.Fatalf("got %v, want %v", got, tt.want)
			}
		})
	}
}

func TestIsTransientAWSError(t *testing.T) {
	tests := []struct {
		name string
		err  error
		want bool
	}{
		{
			name: "request limit exceeded",
			err:  &smithy.GenericAPIError{Code: "RequestLimitExceeded", Message: "throttle"},
			want: true,
		},
		{
			name: "service unavailable",
			err:  &smithy.GenericAPIError{Code: "ServiceUnavailable", Message: "retry later"},
			want: true,
		},
		{
			name: "invalid instance id",
			err:  &smithy.GenericAPIError{Code: "InvalidInstanceID.NotFound", Message: "not found"},
			want: false,
		},
		{
			name: "non aws error",
			err:  errors.New("boom"),
			want: false,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := isTransientAWSError(tt.err)
			if got != tt.want {
				t.Fatalf("got %v, want %v", got, tt.want)
			}
		})
	}
}

func TestRetryAWS_NonTransientDoesNotRetry(t *testing.T) {
	attempts := 0
	err := retryAWS(context.Background(), "run_instances", "us-east-1", func(context.Context) error {
		attempts++
		return &smithy.GenericAPIError{Code: "InvalidParameterValue", Message: "bad request"}
	})
	if err == nil {
		t.Fatal("expected error")
	}
	if attempts != 1 {
		t.Fatalf("expected 1 attempt, got %d", attempts)
	}
}

package model

import "time"

type SessionStatus string

const (
	SessionProvisioning SessionStatus = "provisioning"
	SessionActive       SessionStatus = "active"
	SessionGrace        SessionStatus = "grace"
	SessionStopped      SessionStatus = "stopped"
)

type Session struct {
	ID                 string
	UserID             string
	RelayInstanceID    *string
	RelayAWSInstanceID string
	Status             SessionStatus
	Region             string
	PairToken          string
	RelayWSToken       string
	PublicIP           string
	SRTPort            int
	WSURL              string
	StartedAt          time.Time
	StoppedAt          *time.Time
	DurationSeconds    int
	GraceWindowSeconds int
	MaxSessionSeconds  int
}

type UsageCurrent struct {
	PlanTier         string
	CycleStart       time.Time
	CycleEnd         time.Time
	IncludedSeconds  int
	ConsumedSeconds  int
	RemainingSeconds int
	OverageSeconds   int
}

type RelayManifestEntry struct {
	Region              string
	AMIID               string
	DefaultInstanceType string
	UpdatedAt           time.Time
}

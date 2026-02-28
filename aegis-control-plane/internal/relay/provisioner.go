package relay

import "context"

type ProvisionRequest struct {
	SessionID string
	UserID    string
	Region    string
}

type ProvisionResult struct {
	AWSInstanceID string
	AMIID         string
	InstanceType  string
	PublicIP      string
	SRTPort       int
	WSURL         string
}

type DeprovisionRequest struct {
	SessionID     string
	UserID        string
	Region        string
	AWSInstanceID string
}

type Provisioner interface {
	Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error)
	Deprovision(ctx context.Context, req DeprovisionRequest) error
}

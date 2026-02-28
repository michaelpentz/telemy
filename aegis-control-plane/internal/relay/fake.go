package relay

import (
	"context"
	"crypto/rand"
	"fmt"
)

type FakeProvisioner struct{}

func NewFakeProvisioner() *FakeProvisioner {
	return &FakeProvisioner{}
}

func (f *FakeProvisioner) Provision(_ context.Context, req ProvisionRequest) (ProvisionResult, error) {
	ipTail, err := randomUint8()
	if err != nil {
		return ProvisionResult{}, err
	}
	ip := fmt.Sprintf("203.0.113.%d", 10+int(ipTail)%200)
	return ProvisionResult{
		AWSInstanceID: "i-fake-" + req.SessionID,
		AMIID:         "ami-placeholder-" + req.Region,
		InstanceType:  "t4g.small",
		PublicIP:      ip,
		SRTPort:       9000,
		WSURL:         fmt.Sprintf("wss://%s:7443/telemetry", ip),
	}, nil
}

func (f *FakeProvisioner) Deprovision(_ context.Context, _ DeprovisionRequest) error {
	return nil
}

func randomUint8() (byte, error) {
	var b [1]byte
	if _, err := rand.Read(b[:]); err != nil {
		return 0, err
	}
	return b[0], nil
}

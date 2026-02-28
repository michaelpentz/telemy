package main

import (
	"testing"

	"github.com/telemyapp/aegis-control-plane/internal/config"
)

func TestBuildManifestEntries_FakeModeUsesPlaceholderAMI(t *testing.T) {
	cfg := config.Config{
		RelayProvider:   "fake",
		SupportedRegion: []string{"us-east-1", "eu-west-1"},
		AWSAMIMap:       map[string]string{},
		AWSInstanceType: "t4g.small",
	}

	got := buildManifestEntries(cfg)
	if len(got) != 2 {
		t.Fatalf("expected 2 entries, got %d", len(got))
	}
	if got[0].AMIID != "ami-fake-us-east-1" {
		t.Fatalf("unexpected fake ami for first region: %s", got[0].AMIID)
	}
	if got[1].AMIID != "ami-fake-eu-west-1" {
		t.Fatalf("unexpected fake ami for second region: %s", got[1].AMIID)
	}
}

func TestBuildManifestEntries_AWSModeRequiresAMIMapEntries(t *testing.T) {
	cfg := config.Config{
		RelayProvider:   "aws",
		SupportedRegion: []string{"us-east-1", "eu-west-1"},
		AWSAMIMap: map[string]string{
			"us-east-1": "ami-real-1",
		},
		AWSInstanceType: "t4g.small",
	}

	got := buildManifestEntries(cfg)
	if len(got) != 1 {
		t.Fatalf("expected 1 entry, got %d", len(got))
	}
	if got[0].Region != "us-east-1" || got[0].AMIID != "ami-real-1" {
		t.Fatalf("unexpected manifest entry: %+v", got[0])
	}
}

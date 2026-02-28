package main

import (
	"context"
	"log"
	"net/http"
	"os/signal"
	"syscall"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/telemyapp/aegis-control-plane/internal/api"
	"github.com/telemyapp/aegis-control-plane/internal/config"
	"github.com/telemyapp/aegis-control-plane/internal/model"
	"github.com/telemyapp/aegis-control-plane/internal/relay"
	"github.com/telemyapp/aegis-control-plane/internal/store"
)

func main() {
	cfg, err := config.LoadFromEnv()
	if err != nil {
		log.Fatalf("load config: %v", err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	pool, err := pgxpool.New(ctx, cfg.DatabaseURL)
	if err != nil {
		log.Fatalf("connect db: %v", err)
	}
	defer pool.Close()

	if err := pool.Ping(ctx); err != nil {
		log.Fatalf("ping db: %v", err)
	}

	st := store.New(pool)
	manifestEntries := buildManifestEntries(cfg)
	if err := st.UpsertRelayManifest(ctx, manifestEntries); err != nil {
		log.Fatalf("sync relay manifest: %v", err)
	}
	var prov relay.Provisioner
	switch cfg.RelayProvider {
	case "aws":
		awsProv, err := relay.NewAWSProvisioner(relay.AWSProvisionerOptions{
			AMIByRegion:   cfg.AWSAMIMap,
			InstanceType:  cfg.AWSInstanceType,
			SubnetID:      cfg.AWSSubnetID,
			SecurityGroup: cfg.AWSSecurityIDs,
			KeyName:       cfg.AWSKeyName,
		})
		if err != nil {
			log.Fatalf("init aws provisioner: %v", err)
		}
		prov = awsProv
	default:
		prov = relay.NewFakeProvisioner()
	}
	handler := api.NewRouter(cfg, st, prov)

	srv := &http.Server{
		Addr:        cfg.ListenAddr,
		Handler:     handler,
		ReadTimeout: 30 * time.Second,
		// Relay provisioning in AWS mode may take >15s before the handler writes a response.
		WriteTimeout: 3 * time.Minute,
		IdleTimeout:  60 * time.Second,
	}

	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdownCtx)
	}()

	log.Printf("aegis-control-plane listening on %s", cfg.ListenAddr)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("http server: %v", err)
	}
}

func buildManifestEntries(cfg config.Config) []model.RelayManifestEntry {
	manifestEntries := make([]model.RelayManifestEntry, 0, len(cfg.SupportedRegion))
	for _, region := range cfg.SupportedRegion {
		ami := cfg.AWSAMIMap[region]
		if ami == "" && cfg.RelayProvider == "fake" {
			ami = "ami-fake-" + region
		}
		if ami == "" {
			continue
		}
		manifestEntries = append(manifestEntries, model.RelayManifestEntry{
			Region:              region,
			AMIID:               ami,
			DefaultInstanceType: cfg.AWSInstanceType,
		})
	}
	return manifestEntries
}

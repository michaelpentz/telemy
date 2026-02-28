package main

import (
	"context"
	"log"
	"os/signal"
	"syscall"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/telemyapp/aegis-control-plane/internal/config"
	"github.com/telemyapp/aegis-control-plane/internal/jobs"
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
	jobs.NewRunner(st).Start(ctx)

	log.Printf("aegis-jobs worker started")
	<-ctx.Done()
	log.Printf("aegis-jobs worker stopping")
}

package relay

import (
	"crypto/rand"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"log"
	"strings"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	awscfg "github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/service/ec2"
	ec2types "github.com/aws/aws-sdk-go-v2/service/ec2/types"
	"github.com/aws/smithy-go"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
)

type AWSProvisioner struct {
	amiByRegion   map[string]string
	instanceType  string
	subnetID      string
	securityGroup []string
	keyName       string
}

type AWSProvisionerOptions struct {
	AMIByRegion   map[string]string
	InstanceType  string
	SubnetID      string
	SecurityGroup []string
	KeyName       string
}

func NewAWSProvisioner(opts AWSProvisionerOptions) (*AWSProvisioner, error) {
	if len(opts.AMIByRegion) == 0 {
		return nil, fmt.Errorf("AMIByRegion is required")
	}
	instanceType := strings.TrimSpace(opts.InstanceType)
	if instanceType == "" {
		instanceType = "t4g.small"
	}
	return &AWSProvisioner{
		amiByRegion:   opts.AMIByRegion,
		instanceType:  instanceType,
		subnetID:      strings.TrimSpace(opts.SubnetID),
		securityGroup: opts.SecurityGroup,
		keyName:       strings.TrimSpace(opts.KeyName),
	}, nil
}

func (p *AWSProvisioner) Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error) {
	amiID, ok := p.amiByRegion[req.Region]
	if !ok || strings.TrimSpace(amiID) == "" {
		return ProvisionResult{}, fmt.Errorf("no AMI configured for region %s", req.Region)
	}

	cfg, err := awscfg.LoadDefaultConfig(ctx, awscfg.WithRegion(req.Region))
	if err != nil {
		return ProvisionResult{}, fmt.Errorf("aws config: %w", err)
	}
	client := ec2.NewFromConfig(cfg)

	runInput := &ec2.RunInstancesInput{
		ImageId:      aws.String(amiID),
		InstanceType: ec2types.InstanceType(p.instanceType),
		MinCount:     aws.Int32(1),
		MaxCount:     aws.Int32(1),
		TagSpecifications: []ec2types.TagSpecification{
			{
				ResourceType: ec2types.ResourceTypeInstance,
				Tags: []ec2types.Tag{
					{Key: aws.String("Name"), Value: aws.String("aegis-relay-" + req.SessionID)},
					{Key: aws.String("ManagedBy"), Value: aws.String("aegis-control-plane")},
					{Key: aws.String("AegisSessionID"), Value: aws.String(req.SessionID)},
					{Key: aws.String("AegisUserID"), Value: aws.String(req.UserID)},
				},
			},
		},
	}
	if p.keyName != "" {
		runInput.KeyName = aws.String(p.keyName)
	}

	if p.subnetID != "" {
		eni := ec2types.InstanceNetworkInterfaceSpecification{
			DeviceIndex:              aws.Int32(0),
			AssociatePublicIpAddress: aws.Bool(true),
			SubnetId:                 aws.String(p.subnetID),
		}
		if len(p.securityGroup) > 0 {
			eni.Groups = p.securityGroup
		}
		runInput.NetworkInterfaces = []ec2types.InstanceNetworkInterfaceSpecification{eni}
	} else if len(p.securityGroup) > 0 {
		runInput.SecurityGroupIds = p.securityGroup
	}

	var runOut *ec2.RunInstancesOutput
	runStart := time.Now()
	err = retryAWS(ctx, "run_instances", req.Region, func(callCtx context.Context) error {
		var runErr error
		runOut, runErr = client.RunInstances(callCtx, runInput)
		return runErr
	})
	log.Printf("metric=aws_run_instances_latency_ms region=%s session_id=%s value=%d", req.Region, req.SessionID, time.Since(runStart).Milliseconds())
	runDurMS := float64(time.Since(runStart).Milliseconds())
	if err != nil {
		labels := map[string]string{"op": "run_instances", "region": req.Region, "status": "error"}
		metrics.Default().IncCounter("aegis_aws_operations_total", labels)
		metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", runDurMS, labels)
		return ProvisionResult{}, fmt.Errorf("run instances: %w", err)
	}
	labels := map[string]string{"op": "run_instances", "region": req.Region, "status": "ok"}
	metrics.Default().IncCounter("aegis_aws_operations_total", labels)
	metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", runDurMS, labels)
	if len(runOut.Instances) == 0 || runOut.Instances[0].InstanceId == nil {
		return ProvisionResult{}, fmt.Errorf("run instances: no instance returned")
	}
	instanceID := aws.ToString(runOut.Instances[0].InstanceId)

	waitCtx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	waiter := ec2.NewInstanceRunningWaiter(client)
	if err := waiter.Wait(waitCtx, &ec2.DescribeInstancesInput{InstanceIds: []string{instanceID}}, 2*time.Minute); err != nil {
		return ProvisionResult{}, fmt.Errorf("wait running: %w", err)
	}

	descOut, err := client.DescribeInstances(ctx, &ec2.DescribeInstancesInput{InstanceIds: []string{instanceID}})
	if err != nil {
		return ProvisionResult{}, fmt.Errorf("describe instances: %w", err)
	}

	publicIP := extractPublicIP(descOut)
	if publicIP == "" {
		return ProvisionResult{}, fmt.Errorf("instance %s has no public ip", instanceID)
	}

	return ProvisionResult{
		AWSInstanceID: instanceID,
		AMIID:         amiID,
		InstanceType:  p.instanceType,
		PublicIP:      publicIP,
		SRTPort:       9000,
		WSURL:         fmt.Sprintf("wss://%s:7443/telemetry", publicIP),
	}, nil
}

func (p *AWSProvisioner) Deprovision(ctx context.Context, req DeprovisionRequest) error {
	if strings.TrimSpace(req.AWSInstanceID) == "" {
		return nil
	}
	cfg, err := awscfg.LoadDefaultConfig(ctx, awscfg.WithRegion(req.Region))
	if err != nil {
		return fmt.Errorf("aws config: %w", err)
	}
	client := ec2.NewFromConfig(cfg)
	termStart := time.Now()
	err = retryAWS(ctx, "terminate_instances", req.Region, func(callCtx context.Context) error {
		_, termErr := client.TerminateInstances(callCtx, &ec2.TerminateInstancesInput{
			InstanceIds: []string{req.AWSInstanceID},
		})
		return termErr
	})
	log.Printf("metric=aws_terminate_instances_latency_ms region=%s session_id=%s instance_id=%s value=%d", req.Region, req.SessionID, req.AWSInstanceID, time.Since(termStart).Milliseconds())
	termDurMS := float64(time.Since(termStart).Milliseconds())
	if err != nil {
		if shouldIgnoreTerminateError(err) {
			labels := map[string]string{"op": "terminate_instances", "region": req.Region, "status": "ignored"}
			metrics.Default().IncCounter("aegis_aws_operations_total", labels)
			metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", termDurMS, labels)
			return nil
		}
		labels := map[string]string{"op": "terminate_instances", "region": req.Region, "status": "error"}
		metrics.Default().IncCounter("aegis_aws_operations_total", labels)
		metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", termDurMS, labels)
		return fmt.Errorf("terminate instance: %w", err)
	}
	labels := map[string]string{"op": "terminate_instances", "region": req.Region, "status": "ok"}
	metrics.Default().IncCounter("aegis_aws_operations_total", labels)
	metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", termDurMS, labels)
	return nil
}

func shouldIgnoreTerminateError(err error) bool {
	var apiErr smithy.APIError
	if !errors.As(err, &apiErr) {
		return false
	}
	code := apiErr.ErrorCode()
	return code == "InvalidInstanceID.NotFound" || code == "IncorrectInstanceState"
}

func retryAWS(ctx context.Context, opName, region string, fn func(context.Context) error) error {
	const (
		maxAttempts = 4
		baseDelay   = 250 * time.Millisecond
		maxDelay    = 2 * time.Second
	)
	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		err := fn(ctx)
		if err == nil {
			return nil
		}
		lastErr = err
		if !isTransientAWSError(err) {
			return err
		}
		if attempt == maxAttempts {
			metrics.Default().IncCounter("aegis_aws_retry_exhausted_total", map[string]string{
				"op":     opName,
				"region": region,
			})
			return err
		}
		reason := awsErrorCode(err)
		metrics.Default().IncCounter("aegis_aws_retries_total", map[string]string{
			"op":     opName,
			"region": region,
			"reason": reason,
		})
		delay := baseDelay * time.Duration(1<<(attempt-1))
		if delay > maxDelay {
			delay = maxDelay
		}
		delay = withJitter(delay)
		log.Printf("event=aws_retry op=%s region=%s attempt=%d delay_ms=%d err=%q", opName, region, attempt, delay.Milliseconds(), err.Error())
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		case <-timer.C:
		}
	}
	return lastErr
}

func withJitter(delay time.Duration) time.Duration {
	if delay <= 0 {
		return 0
	}
	floor := delay / 10
	span := delay - floor
	if span <= 0 {
		return floor
	}
	var raw [8]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return floor + (span / 2)
	}
	max := uint64(span)
	if max == 0 {
		return floor + (span / 2)
	}
	n := binary.LittleEndian.Uint64(raw[:]) % max
	// Jittered delay in [10% of base, 100% of base).
	return floor + time.Duration(n)
}

func isTransientAWSError(err error) bool {
	var apiErr smithy.APIError
	if !errors.As(err, &apiErr) {
		return false
	}
	switch apiErr.ErrorCode() {
	case "RequestLimitExceeded",
		"Throttling",
		"ThrottlingException",
		"RequestThrottled",
		"ServiceUnavailable",
		"InternalError",
		"RequestTimeout",
		"EC2ThrottledException",
		"InsufficientInstanceCapacity":
		return true
	default:
		return false
	}
}

func awsErrorCode(err error) string {
	var apiErr smithy.APIError
	if !errors.As(err, &apiErr) {
		return "non_api_error"
	}
	code := strings.TrimSpace(apiErr.ErrorCode())
	if code == "" {
		return "unknown"
	}
	return code
}

func extractPublicIP(out *ec2.DescribeInstancesOutput) string {
	for _, res := range out.Reservations {
		for _, inst := range res.Instances {
			if inst.PublicIpAddress != nil && strings.TrimSpace(*inst.PublicIpAddress) != "" {
				return *inst.PublicIpAddress
			}
		}
	}
	return ""
}

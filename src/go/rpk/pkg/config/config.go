// Copyright 2020 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package config

import (
	"fmt"
	"strings"
)

const (
	ModeDev  = "dev"
	ModeProd = "prod"

	DefaultKafkaPort     = 9092
	DefaultSchemaRegPort = 8081
	DefaultProxyPort     = 8082
	DefaultAdminPort     = 9644

	DefaultBallastFilePath = "/var/lib/redpanda/data/ballast"
	DefaultBallastFileSize = "1GiB"
)

func Default() *Config {
	return &Config{
		ConfigFile: "/etc/redpanda/redpanda.yaml",
		Redpanda: RedpandaConfig{
			Directory: "/var/lib/redpanda/data",
			RPCServer: SocketAddress{
				Address: "0.0.0.0",
				Port:    33145,
			},
			KafkaAPI: []NamedAuthNSocketAddress{{
				Address: "0.0.0.0",
				Port:    9092,
			}},
			AdminAPI: []NamedSocketAddress{{
				Address: "0.0.0.0",
				Port:    9644,
			}},
			SeedServers:   []SeedServer{},
			DeveloperMode: true,
		},
		Rpk: RpkConfig{
			CoredumpDir: "/var/lib/redpanda/coredump",
		},
		// enable pandaproxy and schema_registry by default
		Pandaproxy:     &Pandaproxy{},
		SchemaRegistry: &SchemaRegistry{},
	}
}

func SetMode(mode string, conf *Config) (*Config, error) {
	m, err := NormalizeMode(mode)
	if err != nil {
		return nil, err
	}
	switch m {
	case ModeDev:
		return setDevelopment(conf), nil

	case ModeProd:
		return setProduction(conf), nil

	default:
		err := fmt.Errorf(
			"'%s' is not a supported mode. Available modes: %s",
			mode,
			strings.Join(AvailableModes(), ", "),
		)
		return nil, err
	}
}

func setDevelopment(conf *Config) *Config {
	conf.Redpanda.DeveloperMode = true
	// Defaults to setting all tuners to false
	conf.Rpk = RpkConfig{
		TLS:                  conf.Rpk.TLS,
		SASL:                 conf.Rpk.SASL,
		KafkaAPI:             conf.Rpk.KafkaAPI,
		AdminAPI:             conf.Rpk.AdminAPI,
		AdditionalStartFlags: conf.Rpk.AdditionalStartFlags,
		EnableUsageStats:     conf.Rpk.EnableUsageStats,
		CoredumpDir:          conf.Rpk.CoredumpDir,
		SMP:                  Default().Rpk.SMP,
		BallastFilePath:      conf.Rpk.BallastFilePath,
		BallastFileSize:      conf.Rpk.BallastFileSize,
		Overprovisioned:      true,
	}
	return conf
}

func setProduction(conf *Config) *Config {
	conf.Redpanda.DeveloperMode = false
	conf.Rpk.TuneNetwork = true
	conf.Rpk.TuneDiskScheduler = true
	conf.Rpk.TuneNomerges = true
	conf.Rpk.TuneDiskIrq = true
	conf.Rpk.TuneFstrim = false
	conf.Rpk.TuneCPU = true
	conf.Rpk.TuneAioEvents = true
	conf.Rpk.TuneClocksource = true
	conf.Rpk.TuneSwappiness = true
	conf.Rpk.Overprovisioned = false
	conf.Rpk.TuneDiskWriteCache = true
	conf.Rpk.TuneBallastFile = true
	return conf
}

func NormalizeMode(mode string) (string, error) {
	switch mode {
	case "":
		fallthrough
	case "development", ModeDev:
		return ModeDev, nil

	case "production", ModeProd:
		return ModeProd, nil

	default:
		err := fmt.Errorf(
			"'%s' is not a supported mode. Available modes: %s",
			mode,
			strings.Join(AvailableModes(), ", "),
		)
		return "", err
	}
}

func AvailableModes() []string {
	return []string{
		ModeDev,
		"development",
		ModeProd,
		"production",
	}
}

// FileOrDefaults return the configuration as read from the file or
// the default configuration if there is no file loaded.
func (c *Config) FileOrDefaults() *Config {
	if c.File() != nil {
		cfg := c.File()
		cfg.loadedPath = c.loadedPath
		cfg.ConfigFile = c.ConfigFile // preserve loaded ConfigFile property.
		return cfg
	} else {
		cfg := Default()
		cfg.ConfigFile = c.ConfigFile
		return cfg // no file, write the defaults
	}
}

// Check checks if the redpanda and rpk configuration is valid before running
// the tuners. See: redpanda_checkers.
func (c *Config) Check() (bool, []error) {
	errs := checkRedpandaConfig(c)
	errs = append(
		errs,
		checkRpkConfig(c)...,
	)
	ok := len(errs) == 0
	return ok, errs
}

func checkRedpandaConfig(cfg *Config) []error {
	var errs []error
	rp := cfg.Redpanda
	// top level check
	if rp.Directory == "" {
		errs = append(errs, fmt.Errorf("redpanda.data_directory can't be empty"))
	}
	if rp.ID < 0 {
		errs = append(errs, fmt.Errorf("redpanda.node_id can't be a negative integer"))
	}

	// rpc server
	if rp.RPCServer == (SocketAddress{}) {
		errs = append(errs, fmt.Errorf("redpanda.rpc_server missing"))
	} else {
		saErrs := checkSocketAddress(rp.RPCServer, "redpanda.rpc_server")
		if len(saErrs) > 0 {
			errs = append(errs, saErrs...)
		}
	}

	// kafka api
	if len(rp.KafkaAPI) == 0 {
		errs = append(errs, fmt.Errorf("redpanda.kafka_api missing"))
	} else {
		for i, addr := range rp.KafkaAPI {
			configPath := fmt.Sprintf("redpanda.kafka_api[%d]", i)
			saErrs := checkSocketAddress(SocketAddress{addr.Address, addr.Port}, configPath)
			if len(saErrs) > 0 {
				errs = append(errs, saErrs...)
			}
		}
	}

	// seed servers
	if len(rp.SeedServers) > 0 {
		for i, seed := range rp.SeedServers {
			configPath := fmt.Sprintf("redpanda.seed_servers[%d].host", i)
			saErrs := checkSocketAddress(seed.Host, configPath)
			if len(saErrs) > 0 {
				errs = append(errs, saErrs...)
			}
		}
	}
	return errs
}

func checkRpkConfig(cfg *Config) []error {
	var errs []error
	if cfg.Rpk.TuneCoredump && cfg.Rpk.CoredumpDir == "" {
		errs = append(errs, fmt.Errorf("if rpk.tune_coredump is set to true, rpk.coredump_dir can't be empty"))
	}
	return errs
}

func checkSocketAddress(s SocketAddress, configPath string) []error {
	var errs []error
	if s.Port == 0 {
		errs = append(errs, fmt.Errorf("%s.port can't be 0", configPath))
	}
	if s.Address == "" {
		errs = append(errs, fmt.Errorf("%s.address can't be empty", configPath))
	}
	return errs
}

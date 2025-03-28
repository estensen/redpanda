// Copyright 2022 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

//go:build windows

package config

import (
	"os"

	"github.com/spf13/afero"
)

// In windows this is a no-op.

func preserveUnixOwnership(fs afero.Fs, stat os.FileInfo, file string) error {
	return nil
}

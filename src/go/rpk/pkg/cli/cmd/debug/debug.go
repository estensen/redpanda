// Copyright 2021 Redpanda Data, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package debug

import (
	"github.com/spf13/afero"
	"github.com/spf13/cobra"
)

func NewCommand(fs afero.Fs) *cobra.Command {
	cmd := &cobra.Command{
		Use:   "debug",
		Short: "Debug the local Redpanda process",
	}

	cmd.AddCommand(
		newBundleCommand(fs),
		NewInfoCommand(fs),
		newLogdirsCommand(fs),
	)

	return cmd
}

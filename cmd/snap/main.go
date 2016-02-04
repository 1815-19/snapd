// -*- Mode: Go; indent-tabs-mode: t -*-

/*
 * Copyright (C) 2014-2015 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

package main

import (
	"fmt"
	"os"

	"github.com/ubuntu-core/snappy/logger"

	"github.com/jessevdk/go-flags"
)

type options struct {
	// No global options yet
}

var optionsData options

var parser *flags.Parser

// cmdInfo holds information needed to call parser.AddCommand(...).
type cmdInfo struct {
	name, shortHelp, longHelp string
	callback                  func() interface{}
}

// commands holds information about all non-experimental commands.
var commands []cmdInfo

// Parser creates and populates a fresh parser.
// Since commands have local state a fresh parser is required to isolate tests
// from each other.
func Parser() *flags.Parser {
	parser := flags.NewParser(&optionsData, flags.HelpFlag|flags.PassDoubleDash)
	// Add all regular commands
	for _, c := range commands {
		if _, err := parser.AddCommand(c.name, c.shortHelp, c.longHelp, c.callback()); err != nil {
			logger.Panicf("unable to add command %q: %v", c.name, err)
		}
	}
	return parser
}

func init() {
	parser = Parser()
	err := logger.SimpleSetup()
	if err != nil {
		fmt.Fprintf(os.Stderr, "WARNING: failed to activate logging: %s\n", err)
	}
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	_, err := parser.Parse()
	if err != nil {
		return err
	}
	if _, ok := err.(*flags.Error); !ok {
		logger.Debugf("cannot parse arguments: %v: %v", os.Args, err)
	}
	return nil
}

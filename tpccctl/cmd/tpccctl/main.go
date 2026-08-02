package main

import (
	"os"

	"portable-tpcc/tpccctl/internal/cli"
)

func main() {
	os.Exit(cli.Run(os.Args[1:]))
}

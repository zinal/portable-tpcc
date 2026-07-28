package specclient

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// Client invokes tpcc-spec CLI per specification §4.1.
type Client struct {
	BinaryPath string
}

// DescribeResult is the output of tpcc-spec describe.
type DescribeResult struct {
	Edition        string `json:"edition"`
	DocumentURL    string `json:"document_url"`
	DocumentSHA256 string `json:"document_sha256"`
	ModuleABI      int    `json:"module_abi"`
	ModuleSHA256   string `json:"module_sha256"`
}

// MaterializeInput parameters for tpcc-spec materialize.
type MaterializeInput struct {
	Edition    string
	Scale      map[string]interface{}
	SeedSource map[string]interface{}
}

// New creates a spec client. If binaryPath is empty, uses PATH lookup.
func New(binaryPath string) *Client {
	return &Client{BinaryPath: binaryPath}
}

func (c *Client) binary() string {
	if c.BinaryPath != "" {
		return c.BinaryPath
	}
	return "tpcc-spec"
}

// Describe runs tpcc-spec describe --edition.
func (c *Client) Describe(edition string) (*DescribeResult, error) {
	out, err := c.run([]string{"describe", "--edition", edition})
	if err != nil {
		// Stub when tpcc-spec is not available.
		if c.BinaryPath == "" && !binaryExists(c.binary()) {
			return stubDescribe(edition), nil
		}
		return nil, err
	}
	var res DescribeResult
	if err := json.Unmarshal(out, &res); err != nil {
		return nil, err
	}
	return &res, nil
}

// Materialize runs tpcc-spec materialize and writes spec-state.json.
func (c *Client) Materialize(in MaterializeInput, outPath string) error {
	if c.BinaryPath == "" && !binaryExists(c.binary()) {
		return writeStubSpecState(outPath, in)
	}
	scaleJSON, err := json.Marshal(in.Scale)
	if err != nil {
		return err
	}
	seedJSON, err := json.Marshal(in.SeedSource)
	if err != nil {
		return err
	}
	tmpScale := filepath.Join(os.TempDir(), "tpccctl-scale.json")
	tmpSeed := filepath.Join(os.TempDir(), "tpccctl-seed.json")
	if err := os.WriteFile(tmpScale, scaleJSON, 0600); err != nil {
		return err
	}
	defer os.Remove(tmpScale)
	if err := os.WriteFile(tmpSeed, seedJSON, 0600); err != nil {
		return err
	}
	defer os.Remove(tmpSeed)
	out, err := c.run([]string{
		"materialize",
		"--edition", in.Edition,
		"--scale", tmpScale,
		"--seed-source", tmpSeed,
	})
	if err != nil {
		return err
	}
	return os.WriteFile(outPath, out, 0644)
}

// Qualify runs tpcc-spec qualify with aggregate input.
func (c *Client) Qualify(specStatePath string, aggregateInput map[string]interface{}) (map[string]interface{}, error) {
	if c.BinaryPath == "" && !binaryExists(c.binary()) {
		return map[string]interface{}{
			"qualified": false,
			"reason":    "tpcc-spec not available; stub qualify",
		}, nil
	}
	inputJSON, err := json.Marshal(aggregateInput)
	if err != nil {
		return nil, err
	}
	tmp := filepath.Join(os.TempDir(), "tpccctl-aggregate-input.json")
	if err := os.WriteFile(tmp, inputJSON, 0600); err != nil {
		return nil, err
	}
	defer os.Remove(tmp)
	out, err := c.run([]string{
		"qualify",
		"--spec-state", specStatePath,
		"--aggregate-input", tmp,
	})
	if err != nil {
		return nil, err
	}
	var res map[string]interface{}
	if err := json.Unmarshal(out, &res); err != nil {
		return nil, err
	}
	return res, nil
}

func (c *Client) run(args []string) ([]byte, error) {
	cmd := exec.Command(c.binary(), args...)
	out, err := cmd.Output()
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return nil, fmt.Errorf("tpcc-spec %v: %s", args, string(ee.Stderr))
		}
		return nil, err
	}
	return out, nil
}

func binaryExists(name string) bool {
	_, err := exec.LookPath(name)
	return err == nil
}

func stubDescribe(edition string) *DescribeResult {
	return &DescribeResult{
		Edition:        edition,
		DocumentURL:    "https://www.tpc.org/tpc_documents_current_versions/pdf/tpc-c_v5.11.0.pdf",
		DocumentSHA256: "0000000000000000000000000000000000000000000000000000000000000000",
		ModuleABI:      1,
		ModuleSHA256:   "0000000000000000000000000000000000000000000000000000000000000000",
	}
}

func writeStubSpecState(path string, in MaterializeInput) error {
	stub := map[string]interface{}{
		"schema_version": 1,
		"edition":        in.Edition,
		"opaque":         true,
		"scale":          in.Scale,
		"seed_source":    in.SeedSource,
		"stub":           true,
	}
	data, err := json.MarshalIndent(stub, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	return os.WriteFile(path, data, 0644)
}

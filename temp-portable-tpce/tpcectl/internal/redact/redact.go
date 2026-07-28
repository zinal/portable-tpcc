package redact

import (
	"bytes"
	"regexp"
	"strings"
)

var (
	conninfoPassword = regexp.MustCompile(`(?i)password=(?:'(?:\\.|[^'])*'|[^\s']+)`)
	envAssignment    = regexp.MustCompile(`(?i)(TPCE_[A-Z0-9_]*PASSWORD[A-Z0-9_]*=)(?:'(?:\\.|[^'])*'|[^\s]+)`)
	yamlSecretLine   = regexp.MustCompile(`(?i)^(\s*(?:password|secret|token)\s*:\s*).+$`)
)

// String redacts secrets from free-form text (argv, logs, command lines).
func String(s string) string {
	s = conninfoPassword.ReplaceAllString(s, "password=REDACTED")
	s = envAssignment.ReplaceAllString(s, "${1}REDACTED")
	return s
}

// ProfileYAML returns profile bytes safe for collection and plan output.
func ProfileYAML(data []byte) []byte {
	lines := bytes.Split(data, []byte("\n"))
	for i, line := range lines {
		if yamlSecretLine.Match(line) {
			if sub := yamlSecretLine.FindSubmatch(line); len(sub) == 2 {
				lines[i] = append(sub[1], []byte("REDACTED")...)
			}
		}
	}
	return bytes.Join(lines, []byte("\n"))
}

// Tail returns up to n lines from the end of output with secrets redacted.
func Tail(output string, n int) string {
	if n <= 0 {
		return ""
	}
	lines := strings.Split(strings.TrimRight(output, "\n"), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return String(strings.Join(lines, "\n"))
}

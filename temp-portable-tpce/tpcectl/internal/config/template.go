package config

import (
	"fmt"
	"regexp"
	"strings"
)

var allowedPlaceholders = map[string]struct{}{
	"run_id":     {},
	"local_bin":  {},
	"local_data": {},
	"local_sql":  {},
}

var placeholderRE = regexp.MustCompile(`\{\{\s*([a-zA-Z0-9_]+)\s*\}\}`)

// ExpandTemplates replaces supported placeholders in s (spec-orchestrator §5.1).
func ExpandTemplates(s string, vars map[string]string) (string, error) {
	var err error
	out := placeholderRE.ReplaceAllStringFunc(s, func(match string) string {
		if err != nil {
			return match
		}
		sub := placeholderRE.FindStringSubmatch(match)
		if len(sub) != 2 {
			err = fmt.Errorf("invalid template construct: %s", match)
			return match
		}
		name := sub[1]
		if _, ok := allowedPlaceholders[name]; !ok {
			err = fmt.Errorf("unknown template placeholder: {{ %s }}", name)
			return match
		}
		value, ok := vars[name]
		if !ok {
			err = fmt.Errorf("missing value for placeholder: {{ %s }}", name)
			return match
		}
		return value
	})
	if err != nil {
		return "", err
	}
	if strings.Contains(out, "{{") {
		return "", fmt.Errorf("unclosed or unsupported template construct in: %s", s)
	}
	return out, nil
}

// ScanTemplates validates that only allowed placeholders appear in s.
func ScanTemplates(s string) error {
	matches := placeholderRE.FindAllStringSubmatch(s, -1)
	for _, m := range matches {
		if len(m) != 2 {
			return fmt.Errorf("invalid template construct: %s", m[0])
		}
		if _, ok := allowedPlaceholders[m[1]]; !ok {
			return fmt.Errorf("unknown template placeholder: {{ %s }}", m[1])
		}
	}
	if strings.Contains(s, "{{") {
		rest := placeholderRE.ReplaceAllString(s, "")
		if strings.Contains(rest, "{{") {
			return fmt.Errorf("unclosed or unsupported template construct")
		}
	}
	return nil
}

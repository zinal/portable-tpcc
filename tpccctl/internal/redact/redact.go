package redact

import (
	"strings"

	"gopkg.in/yaml.v3"
)

var secretKeys = []string{
	"password",
	"passwd",
	"secret",
	"token",
	"credential",
}

// RedactYAML removes secret literals from profile YAML bytes.
func RedactYAML(data []byte) ([]byte, error) {
	var root yaml.Node
	if err := yaml.Unmarshal(data, &root); err != nil {
		return nil, err
	}
	if len(root.Content) == 0 {
		return data, nil
	}
	doc := root.Content[0]
	redactNode(doc)
	return yaml.Marshal(doc)
}

func redactNode(n *yaml.Node) {
	if n.Kind == yaml.MappingNode {
		for i := 0; i < len(n.Content); i += 2 {
			key := n.Content[i]
			val := n.Content[i+1]
			if key.Kind == yaml.ScalarNode && isSecretKey(key.Value) {
				val.Kind = yaml.ScalarNode
				val.Value = "<redacted>"
				val.Tag = "!!str"
			} else {
				redactNode(val)
			}
		}
	} else if n.Kind == yaml.DocumentNode || n.Kind == yaml.SequenceNode {
		for _, child := range n.Content {
			redactNode(child)
		}
	}
}

func isSecretKey(key string) bool {
	lower := strings.ToLower(key)
	// Env-var *names* and credential *file paths* are not secret literals.
	if lower == "password_env" || lower == "token_env" ||
		strings.HasSuffix(lower, "_file") || strings.HasSuffix(lower, "_path") {
		return false
	}
	for _, s := range secretKeys {
		if strings.Contains(lower, s) {
			return true
		}
	}
	return false
}

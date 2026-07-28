package canonical

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"sort"
)

// MarshalJSON produces RFC 8785 canonical JSON bytes for a decoded JSON value.
func MarshalJSON(v interface{}) ([]byte, error) {
	normalized, err := normalize(v)
	if err != nil {
		return nil, err
	}
	return marshalCanonical(normalized)
}

// SHA256 computes the lowercase hex SHA-256 of canonical JSON for v.
func SHA256(v interface{}) (string, error) {
	data, err := MarshalJSON(v)
	if err != nil {
		return "", err
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:]), nil
}

// SHA256Any canonicalizes arbitrary Go values via JSON round-trip.
func SHA256Any(v interface{}) (string, error) {
	data, err := json.Marshal(v)
	if err != nil {
		return "", err
	}
	var decoded interface{}
	if err := json.Unmarshal(data, &decoded); err != nil {
		return "", err
	}
	return SHA256(decoded)
}

// SHA256Bytes computes lowercase hex SHA-256 of raw bytes.
func SHA256Bytes(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// SHA256File reads a file and returns its SHA-256 hex digest.
func SHA256File(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	return SHA256Bytes(data), nil
}

func normalize(v interface{}) (interface{}, error) {
	switch x := v.(type) {
	case map[string]interface{}:
		out := make(map[string]interface{}, len(x))
		for k, val := range x {
			n, err := normalize(val)
			if err != nil {
				return nil, err
			}
			out[k] = n
		}
		return out, nil
	case []interface{}:
		out := make([]interface{}, len(x))
		for i, val := range x {
			n, err := normalize(val)
			if err != nil {
				return nil, err
			}
			out[i] = n
		}
		return out, nil
	case []string:
		out := make([]interface{}, len(x))
		for i, s := range x {
			out[i] = s
		}
		return out, nil
	case int:
		return json.Number(fmt.Sprintf("%d", x)), nil
	case int64:
		return json.Number(fmt.Sprintf("%d", x)), nil
	case float64:
		if x == float64(int64(x)) {
			return json.Number(fmt.Sprintf("%d", int64(x))), nil
		}
		return json.Number(fmt.Sprintf("%g", x)), nil
	case json.Number:
		return x, nil
	case nil, bool, string:
		return x, nil
	default:
		return nil, fmt.Errorf("unsupported JSON type %T", v)
	}
}

func marshalCanonical(v interface{}) ([]byte, error) {
	switch x := v.(type) {
	case map[string]interface{}:
		keys := make([]string, 0, len(x))
		for k := range x {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		var buf []byte
		buf = append(buf, '{')
		for i, k := range keys {
			if i > 0 {
				buf = append(buf, ',')
			}
			kb, err := marshalString(k)
			if err != nil {
				return nil, err
			}
			buf = append(buf, kb...)
			buf = append(buf, ':')
			vb, err := marshalCanonical(x[k])
			if err != nil {
				return nil, err
			}
			buf = append(buf, vb...)
		}
		buf = append(buf, '}')
		return buf, nil
	case []interface{}:
		var buf []byte
		buf = append(buf, '[')
		for i, elem := range x {
			if i > 0 {
				buf = append(buf, ',')
			}
			vb, err := marshalCanonical(elem)
			if err != nil {
				return nil, err
			}
			buf = append(buf, vb...)
		}
		buf = append(buf, ']')
		return buf, nil
	case nil:
		return []byte("null"), nil
	case bool:
		if x {
			return []byte("true"), nil
		}
		return []byte("false"), nil
	case string:
		return marshalString(x)
	case json.Number:
		return []byte(x.String()), nil
	default:
		return nil, fmt.Errorf("unsupported canonical type %T", v)
	}
}

func marshalString(s string) ([]byte, error) {
	var buf []byte
	buf = append(buf, '"')
	for _, r := range s {
		switch r {
		case '"':
			buf = append(buf, '\\', '"')
		case '\\':
			buf = append(buf, '\\', '\\')
		case '\b':
			buf = append(buf, '\\', 'b')
		case '\f':
			buf = append(buf, '\\', 'f')
		case '\n':
			buf = append(buf, '\\', 'n')
		case '\r':
			buf = append(buf, '\\', 'r')
		case '\t':
			buf = append(buf, '\\', 't')
		default:
			if r < 0x20 {
				buf = append(buf, '\\', 'u', '0', '0', hexDigit(r>>4), hexDigit(r&0xf))
			} else {
				buf = append(buf, string(r)...)
			}
		}
	}
	buf = append(buf, '"')
	return buf, nil
}

func hexDigit(d rune) byte {
	return byte("0123456789abcdef"[d])
}

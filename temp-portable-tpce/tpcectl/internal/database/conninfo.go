package database

import (
	"context"
	"fmt"
	"net"
	"os"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/ydb-platform/tpc-e-tools/tools/tpcectl/internal/config"
)

// Password reads the database password from the profile's password_env variable.
func Password(profile *config.ResolvedProfile) (string, error) {
	if profile == nil {
		return "", fmt.Errorf("profile is nil")
	}
	envName := profile.DB.PasswordEnv
	if envName == "" {
		return "", fmt.Errorf("db.password_env is required")
	}
	value := os.Getenv(envName)
	if value == "" {
		return "", fmt.Errorf("environment variable %s is required", envName)
	}
	return value, nil
}

// ConnInfo builds a libpq connection string for Loader -p and pgx.
func ConnInfo(profile *config.ResolvedProfile, includePassword bool) (string, error) {
	if profile == nil {
		return "", fmt.Errorf("profile is nil")
	}
	parts := []string{
		"host=" + quoteConninfoValue(profile.DB.Host),
		fmt.Sprintf("port=%d", profile.DB.Port),
		"dbname=" + quoteConninfoValue(profile.DB.Name),
		"user=" + quoteConninfoValue(profile.DB.User),
	}
	if profile.DB.SSLMode != "" {
		parts = append(parts, "sslmode="+quoteConninfoValue(profile.DB.SSLMode))
	}
	if includePassword {
		password, err := Password(profile)
		if err != nil {
			return "", err
		}
		parts = append(parts, "password="+quoteConninfoValue(password))
	}
	return strings.Join(parts, " "), nil
}

func quoteConninfoValue(s string) string {
	if s == "" {
		return "''"
	}
	if !strings.ContainsAny(s, " \t'\\") {
		return s
	}
	escaped := strings.ReplaceAll(s, `\`, `\\`)
	escaped = strings.ReplaceAll(escaped, `'`, `\'`)
	return "'" + escaped + "'"
}

// Ping checks TCP reachability and authenticates with PostgreSQL.
func Ping(ctx context.Context, profile *config.ResolvedProfile) error {
	addr := fmt.Sprintf("%s:%d", profile.DB.Host, profile.DB.Port)
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return fmt.Errorf("database unreachable at %s: %w", addr, err)
	}
	_ = conn.Close()

	pgxConn, err := Connect(ctx, profile)
	if err != nil {
		return err
	}
	pgxConn.Close(ctx)
	return nil
}

// Connect opens a PostgreSQL connection using profile database settings.
func Connect(ctx context.Context, profile *config.ResolvedProfile) (*pgx.Conn, error) {
	conninfo, err := ConnInfo(profile, true)
	if err != nil {
		return nil, err
	}
	conn, err := pgx.Connect(ctx, conninfo)
	if err != nil {
		return nil, fmt.Errorf("connect to database %s@%s:%d/%s: %w",
			profile.DB.User, profile.DB.Host, profile.DB.Port, profile.DB.Name, err)
	}
	return conn, nil
}

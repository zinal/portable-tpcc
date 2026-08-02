package state_test

import (
	"sync"
	"testing"

	"portable-tpcc/tpccctl/internal/state"
)

func TestAcquireProfileLockConcurrent(t *testing.T) {
	store := &state.Store{StateDir: t.TempDir()}
	const attempts = 32

	var wg sync.WaitGroup
	start := make(chan struct{})
	errs := make(chan error, attempts)
	for i := 0; i < attempts; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start
			errs <- store.AcquireProfileLock("profile-a", "run")
		}(i)
	}
	close(start)
	wg.Wait()
	close(errs)

	successes := 0
	for err := range errs {
		if err == nil {
			successes++
		}
	}
	if successes != 1 {
		t.Fatalf("successful lock acquisitions=%d, want 1", successes)
	}
}

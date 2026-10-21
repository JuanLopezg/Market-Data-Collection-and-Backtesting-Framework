package postgres

import (
	"context"
	"sync"
)

type queryCacheKey struct{}

type cachedQueryKey struct {
	reader *Reader
	sql    string
}

type cachedQuery struct {
	done chan struct{}
	raw  []byte
	err  error
}

type queryCache struct {
	mu      sync.Mutex
	queries map[cachedQueryKey]*cachedQuery
}

// WithQueryCache shares identical reads, including failures, only within one
// observability request/sweep. The next sweep always reads the source again.
// Do not use this context for trading writes or admission checks.
func WithQueryCache(ctx context.Context) context.Context {
	if _, ok := ctx.Value(queryCacheKey{}).(*queryCache); ok {
		return ctx
	}
	return context.WithValue(ctx, queryCacheKey{}, &queryCache{queries: make(map[cachedQueryKey]*cachedQuery)})
}

func (c *queryCache) query(ctx context.Context, reader *Reader, sql string) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	key := cachedQueryKey{reader: reader, sql: sql}
	c.mu.Lock()
	if result, ok := c.queries[key]; ok {
		c.mu.Unlock()
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-result.done:
			return result.raw, result.err
		}
	}
	result := &cachedQuery{done: make(chan struct{})}
	c.queries[key] = result
	c.mu.Unlock()

	result.raw, result.err = reader.runQuery(ctx, sql)
	close(result.done)
	return result.raw, result.err
}

package eopclient

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"time"

	"go.opentelemetry.io/otel/propagation"
)

// Options configures a Client. Zero values are not valid; use config defaults.
type Options struct {
	// Address of eop-server, host:port.
	Address string
	// DialTimeout bounds connecting and reading the WELCOME banner.
	DialTimeout time.Duration
	// RequestTimeout bounds one command when the caller's context has no earlier deadline.
	RequestTimeout time.Duration
	// IdleTimeout is the longest a pooled connection may sit unused. It must stay below the
	// server's EOP_IDLE_TIMEOUT so the gateway never writes to a socket the server closed.
	IdleTimeout time.Duration
	// MaxConns bounds open connections (idle + in use), well below EOP_MAX_CLIENTS.
	MaxConns int
	// MaxListNodes rejects a LIST_NODES answer announcing more records than this.
	MaxListNodes int
	// MaxLineBytes rejects any single response line longer than this.
	MaxLineBytes int
	// AnalyzeTimeout bounds one ANALYZE_GRAPH exchange. It must exceed the server's
	// EOP_HPC_TIMEOUT_MS, so the server's own timeout answer arrives before the client gives up.
	AnalyzeTimeout time.Duration
	// MaxAnalyzeResponseBytes rejects an ANALYZE_GRAPH answer longer than this. Results of large
	// graphs are one multi-megabyte line.
	MaxAnalyzeResponseBytes int
}

// Validate reports the first option that cannot work.
func (o Options) Validate() error {
	switch {
	case o.Address == "":
		return errors.New("eopclient: address is required")
	case o.DialTimeout <= 0, o.RequestTimeout <= 0, o.IdleTimeout <= 0, o.AnalyzeTimeout <= 0:
		return errors.New("eopclient: timeouts must be positive")
	case o.MaxConns <= 0:
		return errors.New("eopclient: MaxConns must be positive")
	case o.MaxListNodes <= 0 || o.MaxLineBytes <= 0 || o.MaxAnalyzeResponseBytes <= 0:
		return errors.New("eopclient: response limits must be positive")
	}
	return nil
}

// Client sends commands to eop-server over a bounded pool of connections.
// It is safe for concurrent use.
type Client struct {
	opts   Options
	dialer net.Dialer
	idle   chan *conn
	// slots holds one token per open connection, idle or in use.
	slots     chan struct{}
	closeOnce sync.Once
	closed    chan struct{}
	now       func() time.Time
}

type conn struct {
	net.Conn
	reader   *bufio.Reader
	lastUsed time.Time
}

// New builds a Client. It does not connect until the first command.
func New(opts Options) (*Client, error) {
	if err := opts.Validate(); err != nil {
		return nil, err
	}
	return &Client{
		opts:   opts,
		dialer: net.Dialer{Timeout: opts.DialTimeout},
		idle:   make(chan *conn, opts.MaxConns),
		slots:  make(chan struct{}, opts.MaxConns),
		closed: make(chan struct{}),
		now:    time.Now,
	}, nil
}

// Close closes idle connections. Connections in use are closed when they are returned.
func (c *Client) Close() error {
	c.closeOnce.Do(func() {
		close(c.closed)
		for {
			select {
			case pc := <-c.idle:
				c.discard(pc)
			default:
				return
			}
		}
	})
	return nil
}

// ListNodes returns every node in the server's registry.
func (c *Client) ListNodes(ctx context.Context) ([]Node, error) {
	var nodes []Node
	err := c.do(ctx, "LIST_NODES", func(pc *conn) error {
		header, err := c.readLine(pc)
		if err != nil {
			return err
		}
		if perr := parseErrorLine(header); perr != nil {
			return perr
		}
		count, err := parseListHeader(header, c.opts.MaxListNodes)
		if err != nil {
			return err
		}
		nodes = make([]Node, 0, count)
		for i := 0; i < count; i++ {
			line, err := c.readLine(pc)
			if err != nil {
				return err
			}
			node, err := parseNode(line)
			if err != nil {
				return err
			}
			nodes = append(nodes, node)
		}
		terminator, err := c.readLine(pc)
		if err != nil {
			return err
		}
		if terminator != listTerminator {
			return &MalformedResponseError{Line: terminator, Reason: "expected END_LIST after the announced records"}
		}
		return nil
	})
	return nodes, err
}

// QueryNode returns one node. It returns an error matching ErrNotFound for unknown ids.
func (c *Client) QueryNode(ctx context.Context, nodeID string) (Node, error) {
	if !ValidNodeID(nodeID) {
		return Node{}, fmt.Errorf("%w: node_id %q", ErrInvalidArgument, nodeID)
	}
	var node Node
	err := c.do(ctx, "QUERY_NODE node_id="+nodeID, func(pc *conn) error {
		line, err := c.readLine(pc)
		if err != nil {
			return err
		}
		if perr := parseErrorLine(line); perr != nil {
			return perr
		}
		node, err = parseNode(line)
		return err
	})
	return node, err
}

// Heartbeat checks that the server answers commands. It is cheap: the server neither logs it
// nor requires a registered session. It carries no traceparent because the server does not
// trace HEARTBEAT; the key would only add bytes to every readiness probe.
func (c *Client) Heartbeat(ctx context.Context) error {
	return c.run(ctx, "HEARTBEAT\n", c.opts.RequestTimeout, true, func(pc *conn) error {
		line, err := c.readLine(pc)
		if err != nil {
			return err
		}
		if perr := parseErrorLine(line); perr != nil {
			return perr
		}
		if line != heartbeatAck {
			return &MalformedResponseError{Line: line, Reason: "expected ACK HEARTBEAT"}
		}
		return nil
	})
}

// do sends one read-only text command and lets read consume its answer. A failure on a reused
// connection is retried once on a fresh one: the server may have closed it while it sat in the
// pool. Only read-only commands go through do, so the retry cannot apply a command twice.
func (c *Client) do(ctx context.Context, command string, read func(*conn) error) error {
	return c.run(ctx, command+traceparentSuffix(ctx)+"\n", c.opts.RequestTimeout, true, read)
}

// run sends line and lets read consume the answer. retry allows one more attempt on a fresh
// connection after a stale pooled one failed; reuse returns the connection to the pool.
func (c *Client) run(ctx context.Context, line string, timeout time.Duration, retry bool, read func(*conn) error) error {
	return c.exchangeOnPool(ctx, line, timeout, retry, true, read)
}

func (c *Client) exchangeOnPool(ctx context.Context, line string, timeout time.Duration, retry, reuse bool, read func(*conn) error) error {
	for attempt := 0; ; attempt++ {
		pc, reused, err := c.get(ctx, attempt > 0)
		if err != nil {
			return err
		}
		err = c.exchange(ctx, pc, line, timeout, read)
		if reuse && (err == nil || isExpectedAnswer(err)) {
			c.put(pc)
			return err
		}
		if err == nil || isExpectedAnswer(err) {
			c.discard(pc)
			return err
		}
		c.discard(pc)
		if ctxErr := c.callerGaveUp(ctx); ctxErr != nil {
			return errors.Join(ctxErr, err)
		}
		if !retry || !isConnectionError(err) || !reused || attempt > 0 {
			return err
		}
	}
}

// callerGaveUp returns the caller's reason when a failure happened because ctx was cancelled or
// reached its deadline, which the socket only reports as a timeout.
func (c *Client) callerGaveUp(ctx context.Context) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if d, ok := ctx.Deadline(); ok && !c.now().Before(d) {
		return context.DeadlineExceeded
	}
	return nil
}

// isExpectedAnswer is true for the one error answer a well-behaved exchange produces: an unknown
// node. Only then is the connection known to be in sync and safe to reuse; after any other error
// the server may have sent more lines than the client read (for example the framing bug fixed
// in #55, where one long request produced two answers).
func isExpectedAnswer(err error) bool {
	var perr *ProtocolError
	return errors.As(err, &perr) && perr.Code == CodeNotFound
}

func (c *Client) exchange(ctx context.Context, pc *conn, line string, timeout time.Duration, read func(*conn) error) error {
	deadline := c.now().Add(timeout)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	if err := pc.SetDeadline(deadline); err != nil {
		return err
	}
	// Cancelling ctx must interrupt a blocked read, not only bound the next one.
	stop := context.AfterFunc(ctx, func() { _ = pc.SetDeadline(time.Unix(1, 0)) })
	defer stop()
	if _, err := io.WriteString(pc, line); err != nil {
		return err
	}
	return read(pc)
}

// get returns an idle connection or dials a new one. With fresh set it skips idle ones.
func (c *Client) get(ctx context.Context, fresh bool) (*conn, bool, error) {
	for {
		select {
		case <-c.closed:
			return nil, false, errors.New("eopclient: client is closed")
		default:
		}
		if !fresh {
			select {
			case pc := <-c.idle:
				if c.now().Sub(pc.lastUsed) >= c.opts.IdleTimeout {
					c.discard(pc)
					continue
				}
				return pc, true, nil
			default:
			}
		}
		select {
		case pc := <-c.idle:
			if fresh || c.now().Sub(pc.lastUsed) >= c.opts.IdleTimeout {
				c.discard(pc)
				continue
			}
			return pc, true, nil
		case c.slots <- struct{}{}:
			pc, err := c.dial(ctx)
			if err != nil {
				<-c.slots
				return nil, false, err
			}
			return pc, false, nil
		case <-ctx.Done():
			return nil, false, ctx.Err()
		}
	}
}

func (c *Client) dial(ctx context.Context) (*conn, error) {
	raw, err := c.dialer.DialContext(ctx, "tcp", c.opts.Address)
	if err != nil {
		return nil, &UnavailableError{Err: err}
	}
	pc := &conn{Conn: raw, reader: bufio.NewReaderSize(raw, c.opts.MaxLineBytes)}
	if err := raw.SetDeadline(c.now().Add(c.opts.DialTimeout)); err != nil {
		_ = raw.Close()
		return nil, &UnavailableError{Err: err}
	}
	banner, err := c.readLine(pc)
	if err != nil {
		_ = raw.Close()
		return nil, &UnavailableError{Err: err}
	}
	if !strings.HasPrefix(banner, bannerPrefix) {
		_ = raw.Close()
		return nil, &MalformedResponseError{Line: banner, Reason: "expected WELCOME banner"}
	}
	return pc, nil
}

func (c *Client) put(pc *conn) {
	select {
	case <-c.closed:
		c.discard(pc)
		return
	default:
	}
	pc.lastUsed = c.now()
	c.idle <- pc
}

func (c *Client) discard(pc *conn) {
	_ = pc.Close()
	<-c.slots
}

// readLine reads one line without its terminator, refusing lines over MaxLineBytes.
func (c *Client) readLine(pc *conn) (string, error) {
	line, err := pc.reader.ReadSlice('\n')
	if errors.Is(err, bufio.ErrBufferFull) {
		return "", &MalformedResponseError{Line: string(line[:min(len(line), 80)]), Reason: "line exceeds the configured limit"}
	}
	if err != nil {
		return "", err
	}
	return strings.TrimRight(string(line), "\r\n"), nil
}

// UnavailableError means eop-server could not be reached.
type UnavailableError struct{ Err error }

func (e *UnavailableError) Error() string {
	return "eopclient: eop-server unavailable: " + e.Err.Error()
}
func (e *UnavailableError) Unwrap() error { return e.Err }

// IsUnavailable reports whether err means the server could not be reached or did not answer
// in time, as opposed to answering with an error.
func IsUnavailable(err error) bool {
	var unavailable *UnavailableError
	if errors.As(err, &unavailable) {
		return true
	}
	return isConnectionError(err) || errors.Is(err, context.DeadlineExceeded)
}

func isConnectionError(err error) bool {
	if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, net.ErrClosed) {
		return true
	}
	var netErr net.Error
	return errors.As(err, &netErr)
}

// traceparentSuffix appends the caller's trace context as the optional, last key of a text
// command (ADR-026). Servers that do not read it ignore it.
func traceparentSuffix(ctx context.Context) string {
	if tp := traceparent(ctx); tp != "" {
		return " traceparent=" + tp
	}
	return ""
}

// traceparent returns the W3C traceparent of the span in ctx, or "" when there is none.
func traceparent(ctx context.Context) string {
	carrier := propagation.MapCarrier{}
	propagation.TraceContext{}.Inject(ctx, carrier)
	return carrier.Get("traceparent")
}

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
}

// Validate reports the first option that cannot work.
func (o Options) Validate() error {
	switch {
	case o.Address == "":
		return errors.New("eopclient: address is required")
	case o.DialTimeout <= 0, o.RequestTimeout <= 0, o.IdleTimeout <= 0:
		return errors.New("eopclient: timeouts must be positive")
	case o.MaxConns <= 0:
		return errors.New("eopclient: MaxConns must be positive")
	case o.MaxListNodes <= 0 || o.MaxLineBytes <= 0:
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
// nor requires a registered session.
func (c *Client) Heartbeat(ctx context.Context) error {
	return c.do(ctx, "HEARTBEAT", func(pc *conn) error {
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

// do sends one command and lets read consume its answer. A failure on a reused connection is
// retried once on a fresh one: the server may have closed it while it sat in the pool. Only
// read-only commands go through do, so the retry cannot apply a command twice.
func (c *Client) do(ctx context.Context, command string, read func(*conn) error) error {
	line := command + traceparentSuffix(ctx) + "\n"
	for attempt := 0; ; attempt++ {
		pc, reused, err := c.get(ctx, attempt > 0)
		if err != nil {
			return err
		}
		err = c.exchange(ctx, pc, line, read)
		var perr *ProtocolError
		if err == nil || errors.As(err, &perr) {
			// An ERROR_* answer is a complete, well-framed reply: the connection is still usable.
			c.put(pc)
			return err
		}
		c.discard(pc)
		if !isConnectionError(err) || !reused || attempt > 0 || ctx.Err() != nil {
			return err
		}
	}
}

func (c *Client) exchange(ctx context.Context, pc *conn, line string, read func(*conn) error) error {
	deadline := c.now().Add(c.opts.RequestTimeout)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	if err := pc.SetDeadline(deadline); err != nil {
		return err
	}
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
	carrier := propagation.MapCarrier{}
	propagation.TraceContext{}.Inject(ctx, carrier)
	if tp := carrier.Get("traceparent"); tp != "" {
		return " traceparent=" + tp
	}
	return ""
}

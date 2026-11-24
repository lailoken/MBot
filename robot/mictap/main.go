// Copyright (c) 2026 MBot contributors. MIT License (see LICENSE and THIRD_PARTY_NOTICES.md).
// Built against github.com/digital-dream-labs/vector-cloud (MIT License, Copyright (c) 2021 Digital Dream
// Labs): uses its internal/ipc (unixgram "ANKICONN" sockets) and internal/clad/cloud (mic message types).
// The original mictap idea of registering a consumer on mic_sock follows vector-cloud's own cloud process.
// mictap: serves Vector's microphones to MBot as a TCP stream of 16 kHz / 16-bit mono PCM.
//
// Two sources:
//  1. RAW (preferred, constant): the libmbotmic.so shim inside vic-anim copies every raw 4-mic packet
//     (robot->anim, tag 0xf2) to the unixgram socket /dev/socket/mbot_rawmic. We remove each mic's DC
//     offset, average the 4 channels, resample 15625 -> 16000 Hz and stream it continuously - Vector
//     hears all the time, no wake word.
//  2. GATED (fallback): the firmware's mic_sock, which only carries audio after "Hey Vector". Used
//     only while no raw packets have arrived for a moment (e.g. shim not loaded).
//
// The robot LISTENS on :9099; MBot connects in. Only one client at a time (a new one replaces it).
package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/digital-dream-labs/vector-cloud/internal/clad/cloud"
	"github.com/digital-dream-labs/vector-cloud/internal/ipc"
)

const (
	rawPath     = "/dev/socket/mbot_rawmic"
	micTag      = 0xf2
	micPktLen   = 1293
	chSamples   = 160     // per channel per packet
	rawFreshFor = 500 * time.Millisecond
)

var (
	mu       sync.Mutex
	client   net.Conn
	lastRaw  atomic.Int64 // unix nanos of the last raw packet
	rawCount atomic.Int64
)

func writeClient(b []byte) {
	mu.Lock()
	defer mu.Unlock()
	if client == nil {
		return
	}
	client.SetWriteDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := client.Write(b); err != nil {
		client.Close()
		client = nil
	}
}

// rawProc turns raw planar 4-mic packets into 16 kHz mono PCM, in integer fixed point (cheap on
// the robot's little ARM CPU). 15625 -> 16000 Hz is exactly 125/128, so the resampler steps 125/128
// of an input sample per output sample.
type rawProc struct {
	// Two cascaded 1-pole high-passes at ~150 Hz (pole 241/256) on the mono mix: removes each mic's DC
	// offset AND the fan/body rumble (74% of measured noise energy is below 100 Hz) - noise RMS halves,
	// speech is untouched (-1.4 dB at 300 Hz, flat above).
	x1, y1, y2 int32
	last       int32 // last mono input sample (interpolation across packets)
	pos          int32    // resampler position in 1/128 input samples, relative to `last`
	mono         [chSamples + 1]int32
}

const (
	resStep = 125 // 1/128 units
	resOne  = 128
)

// process appends this packet's 16 kHz PCM (int16 LE) to out.
func (r *rawProc) process(pkt []byte, out []byte) []byte {
	m := r.mono[:]
	m[0] = r.last
	for i := 0; i < chSamples; i++ {
		var sum int32 // average of 4 mics x gain 4 == plain sum
		for c := 0; c < 4; c++ {
			off := 1 + 2*(c*chSamples+i)
			sum += int32(int16(uint16(pkt[off]) | uint16(pkt[off+1])<<8))
		}
		a := sum - r.x1 + (r.y1*241)>>8 // stage 1
		b := a - r.y1 + (r.y2*241)>>8   // stage 2
		r.x1, r.y1, r.y2 = sum, a, b
		m[i+1] = b
	}
	pos := r.pos
	for pos < chSamples*resOne {
		k := pos >> 7
		f := pos & (resOne - 1)
		v := m[k] + ((m[k+1]-m[k])*f)>>7
		if v > 32767 {
			v = 32767
		} else if v < -32768 {
			v = -32768
		}
		out = append(out, byte(v), byte(uint16(int16(v))>>8))
		pos += resStep
	}
	r.pos = pos - chSamples*resOne
	r.last = m[chSamples]
	return out
}

func rawLoop() {
	os.Remove(rawPath)
	conn, err := net.ListenUnixgram("unixgram", &net.UnixAddr{Name: rawPath, Net: "unixgram"})
	if err != nil {
		fmt.Fprintln(os.Stderr, "[raw] listen error:", err)
		return
	}
	os.Chmod(rawPath, 0666)
	conn.SetReadBuffer(256 * 1024)
	fmt.Fprintln(os.Stderr, "[raw] waiting for raw mic packets on", rawPath)
	var rp rawProc
	buf := make([]byte, 2048)
	const batch = 4 // ~41 ms per network write: 4x fewer syscalls
	pcm := make([]byte, 0, batch*2*(chSamples+8))
	pending := 0
	for {
		n, _, err := conn.ReadFromUnix(buf)
		if err != nil {
			fmt.Fprintln(os.Stderr, "[raw] read error:", err)
			time.Sleep(time.Second)
			continue
		}
		if n != micPktLen || buf[0] != micTag {
			continue
		}
		if rawCount.Add(1) == 1 {
			fmt.Fprintln(os.Stderr, "[raw] raw mic stream flowing - constant listening")
		}
		lastRaw.Store(time.Now().UnixNano())
		pcm = rp.process(buf[:n], pcm)
		if pending++; pending >= batch {
			writeClient(pcm)
			pcm, pending = pcm[:0], 0
		}
	}
}

func rawFresh() bool { return time.Since(time.Unix(0, lastRaw.Load())) < rawFreshFor }

func main() {
	addr := "0.0.0.0:9099"
	if len(os.Args) > 1 {
		addr = os.Args[1]
	}

	ln, err := net.Listen("tcp4", addr)
	if err != nil {
		fmt.Fprintln(os.Stderr, "listen error:", err)
		os.Exit(1)
	}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			fmt.Fprintln(os.Stderr, "[mic] client connected:", c.RemoteAddr())
			mu.Lock()
			if client != nil {
				client.Close()
			}
			client = c
			mu.Unlock()
		}
	}()

	go rawLoop()
	go func() { // heartbeat so the logs show the stream is alive (every 10 min)
		for range time.Tick(10 * time.Minute) {
			if rawFresh() {
				fmt.Fprintf(os.Stderr, "[raw] streaming (%d packets so far)\n", rawCount.Load())
			}
		}
	}()

	mic, err := ipc.NewUnixgramClient(ipc.GetSocketPath("mic_sock"), "cp_mbot")
	if err != nil {
		fmt.Fprintln(os.Stderr, "mic_sock error (raw path still works):", err)
		select {}
	}
	defer mic.Close()
	fmt.Fprintln(os.Stderr, "mic_sock tapped; serving PCM on", addr)

	var hotwords int
	for {
		data := mic.ReadBlock()
		if data == nil {
			fmt.Fprintln(os.Stderr, "mic_sock closed (raw path still works)")
			select {}
		}
		if len(data) == 0 {
			continue
		}
		var m cloud.Message
		if err := m.Unpack(bytes.NewBuffer(data)); err != nil {
			continue
		}
		switch m.Tag() {
		case cloud.MessageTag_Audio:
			if rawFresh() {
				continue // constant raw stream already carries this audio
			}
			buf := new(bytes.Buffer)
			binary.Write(buf, binary.LittleEndian, m.GetAudio().Data)
			writeClient(buf.Bytes())
		case cloud.MessageTag_Hotword:
			hotwords++
			fmt.Fprintf(os.Stderr, "[mic] HOTWORD #%d\n", hotwords)
		}
	}
}

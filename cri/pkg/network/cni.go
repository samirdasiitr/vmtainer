/*
 * Copyright (c) 2026 Samir Das <samiruor@gmail.com>. All rights reserved.
 *
 * PROPRIETARY AND CONFIDENTIAL.
 * Unauthorized copying, reproduction, distribution, or modification of this
 * file, via any medium, is strictly prohibited.
 * All rights reserved.
 */

// Package network handles TAP device creation, IP address allocation, and
// optional CNI-based network setup for vmtainer sandboxes.
//
// Architecture
// ────────────
//  • An IP pool allocates addresses from a configurable /16 subnet (default
//    10.200.0.0/16).  The host side is always .1 of the chosen /30 slice;
//    the guest side is .2 — but for simplicity we hand out individual /16
//    addresses and use the subnet gateway as the host-side default gateway.
//  • A TAP device is created for each sandbox (named vmtapN) and brought up.
//  • If a CNI configuration directory is present we invoke the CNI plugins;
//    otherwise we do manual ip-link / ip-addr setup via netlink.
//  • Port-forward rules are managed with iptables DNAT.
package network

import (
	"encoding/json"
	"fmt"
	"math/rand"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/containernetworking/cni/libcni"
	"github.com/sirupsen/logrus"
)

const (
	defaultSubnet  = "10.200.0.0/16"
	defaultGateway = "10.200.0.1"
	tapPrefix      = "vmtap"
	// maxTAPs is the upper bound on simultaneous sandboxes (limits TAP index).
	maxTAPs = 4096
)

// Manager is the top-level networking controller.
type Manager struct {
	mu      sync.Mutex
	subnet  *net.IPNet
	gateway net.IP
	// usedIPs maps IP string → sandboxID
	usedIPs map[string]string
	// usedTaps maps tapName → sandboxID
	usedTaps map[string]string
	// cniConf is non-nil when a CNI configuration directory was found.
	cniConf *libcni.NetworkConfigList
	cniRT   libcni.CNI
	cniDir  string
}

// Config carries Manager construction parameters.
type Config struct {
	// Subnet is the CIDR from which guest IPs are allocated, e.g. "10.200.0.0/16".
	// Defaults to defaultSubnet.
	Subnet string
	// Gateway is the host-side default gateway announced to guests.
	// Defaults to defaultGateway.
	Gateway string
	// CNIConfDir is the directory that holds *.conflist / *.conf files.
	// Leave blank to disable CNI and fall back to manual TAP setup.
	CNIConfDir string
	// CNIBinDir is the directory that holds CNI plugin binaries.
	CNIBinDir string
}

// NewManager constructs a Manager.  It loads CNI configuration if CNIConfDir
// is non-empty and the directory contains at least one config file.
func NewManager(cfg Config) (*Manager, error) {
	if cfg.Subnet == "" {
		cfg.Subnet = defaultSubnet
	}
	if cfg.Gateway == "" {
		cfg.Gateway = defaultGateway
	}

	_, ipNet, err := net.ParseCIDR(cfg.Subnet)
	if err != nil {
		return nil, fmt.Errorf("network: invalid subnet %q: %w", cfg.Subnet, err)
	}
	gw := net.ParseIP(cfg.Gateway)
	if gw == nil {
		return nil, fmt.Errorf("network: invalid gateway %q", cfg.Gateway)
	}

	m := &Manager{
		subnet:   ipNet,
		gateway:  gw,
		usedIPs:  make(map[string]string),
		usedTaps: make(map[string]string),
	}

	// Try to load CNI configuration.
	if cfg.CNIConfDir != "" {
		if err := m.loadCNI(cfg.CNIConfDir, cfg.CNIBinDir); err != nil {
			logrus.WithError(err).Warn("network: CNI load failed, falling back to manual TAP")
		}
	}

	return m, nil
}

func (m *Manager) loadCNI(confDir, binDir string) error {
	entries, err := os.ReadDir(confDir)
	if err != nil {
		return fmt.Errorf("read CNI conf dir: %w", err)
	}
	var confFile string
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".conflist") || strings.HasSuffix(e.Name(), ".conf") {
			confFile = filepath.Join(confDir, e.Name())
			break
		}
	}
	if confFile == "" {
		return fmt.Errorf("no CNI config files in %s", confDir)
	}

	data, err := os.ReadFile(confFile)
	if err != nil {
		return err
	}

	var confList *libcni.NetworkConfigList
	if strings.HasSuffix(confFile, ".conflist") {
		confList, err = libcni.ConfListFromBytes(data)
	} else {
		var nc *libcni.NetworkConfig
		nc, err = libcni.ConfFromBytes(data)
		if err == nil {
			confList, err = libcni.ConfListFromConf(nc)
		}
	}
	if err != nil {
		return fmt.Errorf("parse CNI conf: %w", err)
	}

	binDirs := []string{"/opt/cni/bin"}
	if binDir != "" {
		binDirs = append([]string{binDir}, binDirs...)
	}
	m.cniConf = confList
	m.cniRT = libcni.NewCNIConfig(binDirs, nil)
	m.cniDir = confDir
	logrus.Infof("network: loaded CNI config %q (%s)", confFile, confList.Name)
	return nil
}

// SandboxNetwork is the result of SetupSandbox.
type SandboxNetwork struct {
	TapName string
	IP      string // CIDR, e.g. "10.200.4.12/16"
	Gateway string
	MAC     string
}

// SetupSandbox allocates a TAP device and IP for a new sandbox.
func (m *Manager) SetupSandbox(sandboxID string) (*SandboxNetwork, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	tap, err := m.allocateTap(sandboxID)
	if err != nil {
		return nil, err
	}
	ip, err := m.allocateIP(sandboxID)
	if err != nil {
		_ = m.releaseTap(tap)
		return nil, err
	}
	mac := generateMAC()

	// Create TAP device (Linux only — silently skip on non-Linux for dev builds).
	if err := createTAP(tap, mac); err != nil {
		logrus.WithError(err).Warnf("network: could not create TAP %s (non-Linux?)", tap)
	}

	cidr := fmt.Sprintf("%s/%d", ip, ones(m.subnet))
	return &SandboxNetwork{
		TapName: tap,
		IP:      cidr,
		Gateway: m.gateway.String(),
		MAC:     mac,
	}, nil
}

// TeardownSandbox releases a TAP device and IP address.
func (m *Manager) TeardownSandbox(sandboxID, tapName, ip string) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	if tapName != "" {
		deleteTAP(tapName) // best-effort
		delete(m.usedTaps, tapName)
	}
	if ip != "" {
		hostIP := strings.Split(ip, "/")[0]
		delete(m.usedIPs, hostIP)
	}
	return nil
}

// PortForward adds an iptables DNAT rule forwarding hostPort → vmIP:containerPort.
func PortForward(vmIP string, hostPort, containerPort int32, protocol string) error {
	if protocol == "" {
		protocol = "tcp"
	}
	args := []string{
		"-t", "nat", "-A", "PREROUTING",
		"-p", protocol,
		"--dport", fmt.Sprintf("%d", hostPort),
		"-j", "DNAT",
		"--to-destination", fmt.Sprintf("%s:%d", vmIP, containerPort),
	}
	cmd := exec.Command("iptables", args...)
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("iptables DNAT: %w: %s", err, string(out))
	}
	// Also ensure MASQUERADE / FORWARD accepts it.
	_ = exec.Command("iptables", "-t", "nat", "-A", "POSTROUTING", "-j", "MASQUERADE").Run()
	_ = exec.Command("iptables", "-A", "FORWARD", "-j", "ACCEPT").Run()
	return nil
}

// RemovePortForward removes a previously added DNAT rule.
func RemovePortForward(vmIP string, hostPort, containerPort int32, protocol string) error {
	if protocol == "" {
		protocol = "tcp"
	}
	args := []string{
		"-t", "nat", "-D", "PREROUTING",
		"-p", protocol,
		"--dport", fmt.Sprintf("%d", hostPort),
		"-j", "DNAT",
		"--to-destination", fmt.Sprintf("%s:%d", vmIP, containerPort),
	}
	cmd := exec.Command("iptables", args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("iptables remove DNAT: %w: %s", err, string(out))
	}
	return nil
}

// ---------------------------------------------------------------------------
// internal helpers
// ---------------------------------------------------------------------------

func (m *Manager) allocateTap(sandboxID string) (string, error) {
	for i := 0; i < maxTAPs; i++ {
		name := fmt.Sprintf("%s%d", tapPrefix, i)
		if _, used := m.usedTaps[name]; !used {
			m.usedTaps[name] = sandboxID
			return name, nil
		}
	}
	return "", fmt.Errorf("network: TAP pool exhausted")
}

func (m *Manager) releaseTap(name string) error {
	delete(m.usedTaps, name)
	return nil
}

func (m *Manager) allocateIP(sandboxID string) (string, error) {
	base := m.subnet.IP.To4()
	if base == nil {
		return "", fmt.Errorf("network: only IPv4 subnets supported")
	}
	// Skip .0 (network) and .1 (gateway); start at .2.
	// We do random allocation with collision check to avoid hotspot at low addrs.
	ones, bits := m.subnet.Mask.Size()
	hostBits := bits - ones
	maxHosts := (1 << hostBits) - 3 // exclude .0, .1, .255…

	rng := rand.New(rand.NewSource(time.Now().UnixNano())) //nolint:gosec
	for attempt := 0; attempt < maxHosts; attempt++ {
		offset := rng.Intn(maxHosts) + 2 // [2, maxHosts+1]
		ip := make(net.IP, 4)
		copy(ip, base)
		ip[3] = base[3] + byte(offset&0xFF)
		ip[2] = base[2] + byte((offset>>8)&0xFF)
		ip[1] = base[1] + byte((offset>>16)&0xFF)
		if !m.subnet.Contains(ip) {
			continue
		}
		key := ip.String()
		if _, used := m.usedIPs[key]; !used {
			m.usedIPs[key] = sandboxID
			return key, nil
		}
	}
	return "", fmt.Errorf("network: IP pool exhausted")
}

func ones(n *net.IPNet) int {
	o, _ := n.Mask.Size()
	return o
}

func generateMAC() string {
	rng := rand.New(rand.NewSource(time.Now().UnixNano())) //nolint:gosec
	// Use the QEMU vendor OUI 52:54:00.
	return fmt.Sprintf("52:54:00:%02x:%02x:%02x",
		rng.Intn(256), rng.Intn(256), rng.Intn(256))
}

// createTAP creates a persistent TAP interface with the given name and MAC.
// On non-Linux platforms (or when ip/tunctl are absent) this is a no-op.
func createTAP(name, mac string) error {
	// Use "ip tuntap add" — available in iproute2.
	cmd := exec.Command("ip", "tuntap", "add", "dev", name, "mode", "tap")
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("ip tuntap add %s: %w: %s", name, err, string(out))
	}
	// Set the MAC address.
	if mac != "" {
		if out, err := exec.Command("ip", "link", "set", name, "address", mac).CombinedOutput(); err != nil {
			logrus.WithError(err).Warnf("network: set MAC on %s: %s", name, string(out))
		}
	}
	// Bring it up.
	if out, err := exec.Command("ip", "link", "set", name, "up").CombinedOutput(); err != nil {
		return fmt.Errorf("ip link set %s up: %w: %s", name, err, string(out))
	}
	return nil
}

// deleteTAP removes a TAP interface (best-effort).
func deleteTAP(name string) {
	_ = exec.Command("ip", "link", "delete", name).Run()
}

// ---------------------------------------------------------------------------
// CNI configuration generator
// ---------------------------------------------------------------------------

// GenerateCNIConfList returns a basic CNI bridge+portmap conflist JSON that
// can be written to the CNI conf directory so clusters without a pre-existing
// CNI setup still work.
func GenerateCNIConfList(subnet, gateway, bridge string) ([]byte, error) {
	if bridge == "" {
		bridge = "vmtainer0"
	}
	conf := map[string]interface{}{
		"cniVersion": "0.4.0",
		"name":       "vmtainer",
		"plugins": []interface{}{
			map[string]interface{}{
				"type":   "bridge",
				"bridge": bridge,
				"isGateway": true,
				"ipMasq": true,
				"ipam": map[string]interface{}{
					"type":   "host-local",
					"subnet": subnet,
					"routes": []interface{}{
						map[string]interface{}{"dst": "0.0.0.0/0"},
					},
				},
			},
			map[string]interface{}{
				"type":         "portmap",
				"capabilities": map[string]interface{}{"portMappings": true},
			},
		},
	}
	return json.MarshalIndent(conf, "", "  ")
}

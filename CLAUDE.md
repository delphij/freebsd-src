# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This is the FreeBSD operating system source tree. FreeBSD is a Unix-like OS with a monolithic kernel and integrated userland, maintained as a single cohesive repository. The primary languages are C, Assembly, Shell, and Lua (for boot loader).

**IMPORTANT**: For comprehensive AI collaboration guidelines, coding standards, testing procedures, and contribution requirements, see [AGENTS.md](AGENTS.md). This file focuses on practical commands and architecture.

## Essential Build Commands

### Basic Build Workflow

```bash
# Build everything (world = userland + libraries)
make buildworld

# Build kernel (default: GENERIC config)
make buildkernel

# Build specific kernel configuration
make buildkernel KERNCONF=GENERIC-NODEBUG

# Install userland (usually in single-user mode)
make installworld

# Install kernel
make installkernel KERNCONF=GENERIC

# Combined kernel build + install
make kernel KERNCONF=GENERIC
```

### Cross-Compilation

```bash
# Cross-build for different architecture
make buildworld TARGET=arm64 TARGET_ARCH=aarch64

# Using external toolchain (e.g., from ports)
make buildworld CROSS_TOOLCHAIN=amd64-gcc13

# Build kernel-toolchain only (faster, for kernel development)
make kernel-toolchain TARGET=arm64 TARGET_ARCH=aarch64
```

### Testing and Validation

```bash
# Run test suite (requires installed world with MK_TESTS=yes)
kyua test -k /usr/tests/Kyuafile

# View test results
kyua report

# Run specific test suite
kyua test -k /usr/tests/bin/cat/Kyuafile

# Test build coverage across all architectures
make tinderbox

# Build universe (all architectures, all kernels)
make universe
```

### Common Development Tasks

```bash
# Quick kernel rebuild (skip config/clean/obj)
make buildkernel KERNFAST=1

# Clean build artifacts
make cleanworld
make cleankernel

# Check for obsolete files after upgrade
make check-old
make delete-old

# Run style checker (Perl script)
./tools/build/checkstyle9.pl <files>

# Cross-build from non-FreeBSD host
./tools/build/make.py --cross-bindir=/path/to/llvm/bin buildworld TARGET=amd64 TARGET_ARCH=amd64
```

### Build Configuration

Build behavior is controlled by:
- `/etc/src.conf` (or `src.conf` in source tree root) - src.conf(5)
- `/etc/make.conf` - make.conf(5)
- `MAKEOBJDIRPREFIX` environment variable (default: `/usr/obj`)

Common build options:
```bash
# Build with debug symbols
make buildkernel KERNCONF=GENERIC WITH_DEBUG=yes

# Build without Clang (if using external toolchain)
make buildworld WITHOUT_CLANG=yes

# Build tests
make buildworld WITH_TESTS=yes
```

## Architecture & Directory Structure

### Top-Level Organization

| Directory | Purpose |
|-----------|---------|
| `sys/` | Kernel sources (see [sys/README.md](sys/README.md)) |
| `bin/`, `sbin/` | Essential system utilities |
| `usr.bin/`, `usr.sbin/` | User and administrative utilities |
| `lib/`, `libexec/` | System libraries and daemons |
| `contrib/` | Third-party imported software (do not modify directly) |
| `crypto/`, `secure/` | Cryptographic components |
| `share/` | Architecture-independent files, documentation, examples |
| `stand/` | Boot loaders (uses Lua) |
| `tests/` | Test suite infrastructure and cross-functional tests |
| `tools/` | Build and development tools |
| `release/` | Release engineering scripts |

### Kernel Architecture (`sys/`)

Key kernel directories:
- `sys/kern/` - Core kernel (process, scheduling, syscalls)
- `sys/vm/` - Virtual memory system
- `sys/net*/` - Networking stack (IPv4, IPv6, 802.11, IPsec)
- `sys/dev/` - Device drivers (architecture-independent)
- `sys/fs/` - Filesystems (except UFS, NFS, ZFS)
- `sys/amd64/`, `sys/arm64/`, etc. - Architecture-specific code
- `sys/conf/` - Kernel build glue and configuration
- `sys/modules/` - Kernel module infrastructure
- `sys/contrib/` - Third-party kernel components (e.g., OpenZFS)

Kernel configurations are in `sys/<arch>/conf/`:
- `GENERIC` - Default config for releases
- `GENERIC-NODEBUG` - Production config without debugging
- `LINT` - Compile-only config for maximum coverage
- `NOTES` - Documentation of all kernel options

### Build System

- **Make tool**: FreeBSD's bmake (NOT GNU make)
- **Make infrastructure**: `share/mk/*.mk` files define build rules
- **Key makefiles**:
  - `Makefile` - Top-level targets (universe, tinderbox, etc.)
  - `Makefile.inc1` - Main build orchestration
  - `sys/conf/kern.mk` - Kernel build rules

### Testing Infrastructure

Tests follow the source tree structure:
```
/usr/src/bin/cp/     -> /usr/tests/bin/cp/
/usr/src/lib/libc/   -> /usr/tests/lib/libc/
```

- **Framework**: Kyua + ATF (Automated Testing Framework)
- **Test types**: Shell scripts (`ATF_TESTS_SH`) or C programs (`ATF_TESTS_C`)
- **Adding tests**: See AGENTS.md section "Adding Tests for Existing Utilities"
  - Update `etc/mtree/BSD.tests.dist`
  - Add `HAS_TESTS=` and `SUBDIR.${MK_TESTS}+= tests` to utility Makefile
  - Create `tests/Makefile` with test definitions

Example test Makefile:
```make
ATF_TESTS_SH= utility_test
.include <bsd.test.mk>
```

## Development Workflow Notes

### Style and Standards

- **C code**: Follow style(9) - use `.clang-format` in repo root
- **Lua code**: Follow style.lua(9) - used in boot loader
- **Makefiles**: Follow style.Makefile(5) - use BSD make features
- **Man pages**: Follow style.mdoc(5) - use mdoc(7) format, one sentence per line

### Upstream Management

Directories `contrib/`, `crypto/`, `sys/contrib/`, `sys/crypto/` contain imported third-party code:
- Check `git log --merges` to find last upstream merge
- If upstream is active (merged within 5 years), submit patches upstream first
- Coordinate changes with maintainers listed in MAINTAINERS file

### Kernel Development

1. Edit kernel source in `sys/`
2. Optionally modify config: `sys/<arch>/conf/MYKERNEL`
3. Build: `make buildkernel KERNCONF=MYKERNEL`
4. Install: `make installkernel KERNCONF=MYKERNEL`
5. Reboot to test
6. Check kernel messages: `dmesg` or `cat /var/log/messages`

Kernel debugging:
- `options DDB` - Interactive kernel debugger
- `options INVARIANTS` - Runtime consistency checks
- `options WITNESS` - Lock order verification (performance impact)

### Common Gotchas

- **Never set `MAKEOBJDIRPREFIX` in make.conf** - use as environment variable only
- **BSD make ≠ GNU make** - syntax differs, use BSD make features
- **Don't modify `/contrib` directly** - submit to upstream first
- **Manual pages**: Don't bump .Dd date until merge
- **Test suite**: Requires `WITH_TESTS=yes` during buildworld
- **Cross-building**: May require `TARGET` and `TARGET_ARCH` both set

### CI/CD

- **.cirrus.yml**: Cirrus CI configuration (official FreeBSD CI)
- **.github/workflows/**: GitHub Actions for cross-platform builds
- Pull requests trigger automated builds for multiple architectures

## Quick Reference

**Get help with build system**: `man build`, `man src.conf`, `man make.conf`

**Architecture-specific info**: `man arch` (7)

**Update workflow**: See top of `Makefile` or [UPDATING](UPDATING) file

**Kernel source docs**: `man intro 9` (section 9 man pages)

**Submit patches**: Use Phabricator (https://reviews.freebsd.org/) or GitHub PR

For detailed contribution guidelines, coding standards, and AI-specific instructions, always refer to [AGENTS.md](AGENTS.md).

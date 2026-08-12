/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 *    @file
 *          Example project configuration file for CHIP.
 *
 *          This is a place to put application or project-specific overrides
 *          to the default configuration values for general CHIP features.
 *
 */

#pragma once

/* Number of bridged (dynamic) endpoints. Driven by CONFIG_BRIDGE_MAX_ENDPOINTS
 * so the bridge slot table and Matter's endpoint storage stay in sync - there
 * is a static_assert in app_task.cpp that fails loudly if they ever diverge. */
#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT CONFIG_BRIDGE_MAX_ENDPOINTS

/* Keep more than one resolved address for a peer.
 *
 * The default is 1: only the highest-scoring address survives resolution, and
 * IPAddressSorter ranks global unicast (4) above a ULA (3). This house's ISP
 * rotates its delegated prefix, so the hub keeps announcing a global address
 * that is already dead while its ULA stays reachable. With a single result the
 * CASE retry path - OperationalSessionSetup::TryNextResult, which is already
 * enabled - has nothing to fall back to: every handshake times out and the
 * bridge shows as offline while the radio is perfectly fine.
 *
 * This was diagnosed on the old hardware by pinging from the board: its own
 * ULA reached the border router in 13 ms, the hub's advertised global address
 * was unreachable from anywhere on the LAN, and the Matter log showed it being
 * chosen with "new best score: 4".
 *
 * Costs roughly 150 bytes per lookup handle. */
#define CHIP_CONFIG_MDNS_RESOLVE_LOOKUP_RESULTS 4

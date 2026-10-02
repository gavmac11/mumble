# Bare metal hosting launch plan

Updated October 2, 2026. Supplier research was collected September 30, 2026. Application pilot base commit: 1772f827f12018472511e82b20c1cb4ba38108b1, with the documented client pacing patch.

Evaluate rented dedicated servers as primary supply: Proxmox/KVM, one VM per community, customer root access, and automated checkout. Share CPU and network statistically, reserve RAM conservatively, and use cloud/reseller VPSs for overflow and extra regions. The initial target is 5–10 concurrent people using voice, webcams, and screensharing.

The user selected shared public IPv4 with separate Mumble and SSH ports; dedicated IPv4 will be a paid upgrade. Offer hourly/day rentals and monthly/yearly subscriptions, with a starter below $10/month. Support covers the standard deployment; customer root modifications are outside managed support. Preserve self-hosting and portable community-state exports.

Bare metal can improve margins against ordinary cloud VPS rates if measured density supports it. It does not automatically beat the $11/year VPS promotion. A [preliminary application pilot](pilot-results-2026-10-01.md) now runs on a supplied VM; its [pacing follow-up](pilot-pacing-results-2026-10-02.md) passed short encoded-media tests after a client change. No host has been purchased, supplier contacted, automated provisioning validated, or KVM density benchmark run. Capacity figures here remain calculations and pilot proposals.

## Hardware to qualify

These are advertised starting prices or public tariffs, not final procurement quotes. Confirm stock, exact configuration, setup fees, tax, renewal price, network terms, location, and permission to resell KVM guests with root access. Maximum RAM and disk options are not included at a starting price unless explicitly quoted.

| Candidate | Published price and specification | Purpose |
| --- | --- | --- |
| OVH US SYS-1 | Starts at $30/month; Xeon E-2136, 6 cores/12 threads; RAM starts at 32 GB and storage at 2 × 512 GB NVMe; 1 Gbps guaranteed. | First low-cost pilot candidate if configuration, stock, and sustained traffic terms qualify. |
| OVH US RISE-GAME-2 | Starts at $104/month; Ryzen 7 5800X, 8 cores/16 threads; RAM starts at 64 GB, 2 × 960 GB NVMe; 1 Gbps guaranteed unmetered. | Larger comparison candidate. More RAM does not increase the network budget. |
| OVH US ADVANCE-1 | Starts at $147/month; EPYC 4244P, 6 cores/12 threads; RAM starts at 32 GB, storage at 2 × 960 GB NVMe; advertised bandwidth range 3–5 Gbps guaranteed. | Quote if network limits density; verify the bandwidth included at the selected price. |
| Hetzner AX42-1 in Falkenstein or Helsinki | Tariff €97.30/month plus €49 setup, excluding IPv4 and VAT. | EU candidate; quote current hardware and network configuration. |

Sources: [OVH US deals](https://us.ovhcloud.com/deals/) and [Hetzner June 2026 tariff](https://docs.hetzner.com/general/infrastructure-and-availability/price-adjustment/). OVH's deals and generic catalog expose different configurations; exact selections remain unverified. Request an equivalent OVH EU quote as well.

Budget hypervisor support explicitly. Proxmox publishes Community at €120/year per occupied CPU socket without enterprise technical support, and Basic at €370/year per occupied socket with three tickets and a one-business-day response target. These are annual commitments; use invoice currency and actual exchange costs. [Proxmox subscriptions](https://proxmox.com/en/products/proxmox-virtual-environment/pricing).

## Media and resource budgets

This fork relays voice and video through the server. A video sender's packets are copied to each other participant in the channel. Client targets are 2 Mbps per screen share and 1.5 Mbps per webcam. The default aggregate server video cap is 20 Mbps and drops packets above its limit. See [relay implementation](../../src/murmur/Server.cpp), [client profiles](../../src/mumble/VideoQualityProfile.cpp), and [server defaults](../../src/murmur/Meta.cpp).

| Ten-person workload | Calculated outgoing video payload | Decimal transfer per hour |
| --- | ---: | ---: |
| One screen share | 18 Mbps | 8.10 GB |
| All webcams | 135 Mbps | 60.75 GB |
| One screen share plus nine webcams | 139.5 Mbps | 62.78 GB |

These figures exclude voice and protocol overhead and assume every other participant receives each stream at target rate. They are calculations. The encoded webcam fixture actually averaged about 1.64 Mbps per sender, so its ten-stream run represented about 147.54 Mbps of outgoing video payload. The [client pacing tests](pilot-pacing-results-2026-10-02.md) passed with a 200 Mbps aggregate allowance and 3.5 Mbps per sender. These are provisional managed settings; the repository's server bandwidth defaults are unchanged. Use measured traffic and peaks in density decisions.

For a 1 Gbps host, suppose we reserve 20% network headroom and add a provisional 15% allowance to video payload for other traffic and overhead:

| Simultaneous demand | Modeled egress | Within the 800 Mbps planning budget |
| --- | ---: | --- |
| One screen-share community | 20.7 Mbps | Up to 38 such communities by bandwidth alone |
| One all-webcam community | 155.25 Mbps | Up to five such communities by bandwidth alone |
| Two all-webcam and six screen-share communities | 434.7 Mbps | Yes |
| Four all-webcam and 36 screen-share communities | 1,366.2 Mbps | No |

Formula: 1.15 × (135 × webcam communities + 18 × screen-share communities). The 15% allowance and 20% headroom are assumptions. CPU, packet rate, ingress, NAT tracking, and client behavior may impose lower limits.

Forty sold communities may be viable when few are simultaneously busy. Forty simultaneous video communities can exceed the port while CPU remains lightly used. Measure correlated evenings, weekends, and events; average utilization alone cannot establish a safe admission limit.

Start the pilot with 1 vCPU, 1 GiB fixed guest RAM, and a 15 GiB disk per VM. Enforce CPU, RAM, network, disk size, and I/O limits outside the guest. Describe CPU and bandwidth as shared resources. Count physical cores separately from logical threads; neither a 5:1 allocation example nor eighty configured vCPUs establishes usable capacity.

Proxmox provides CPU caps and relative weights. Its virtual-NIC rate field is in megabytes per second: rate=25 is approximately 200 Mbps. Test the configured ceiling in both directions and verify voice under congestion. [CPU controls](https://github.com/proxmox/pve-docs/blob/master/qm.adoc) and [VM configuration reference](https://github.com/proxmox/pve-docs/blob/master/generated/qm.conf.5-opts.adoc).

Thin provisioning saves initial allocation; it does not protect a full storage pool. At launch, keep all sold disk quotas within the physical budget, including templates and metadata. Monitor thin-pool data and metadata separately. Reserve host memory and measured VM overhead; do not depend on swap or guest-memory reclamation to fit sold capacity. [Host requirements](https://proxmox.com/en/products/proxmox-virtual-environment/requirements).

An illustrative 64 GiB node with 8 GiB host reserve and 0.15 GiB additional overhead per 1 GiB guest has floor((64 − 8) / 1.15) = 48 RAM slots. Two mirrored 960 GB disks give about 894 GiB before formatting; an 80% allocation budget fits about 47 disks of 15 GiB before further measured overhead. These are rough ceilings, not a sellable capacity claim. A 32 GiB node with a 4 GiB reserve gives 24 RAM slots under the same assumptions.

## Shared IPv4 and provisioning

Each guest receives a private address. Reserve a unique Mumble port on the public address for both TCP and UDP, plus a separate SSH port forwarded to guest TCP 22. Invite links must include the assigned Mumble port. Customers get root in their own guest and their assigned inbound ports; arbitrary inbound ports need an explicit allocation or a dedicated-IP upgrade.

Keep Proxmox management and API access private. Enforce tenant isolation and anti-spoofing outside the guests, prevent access to management networks, and keep provider credentials in the control service. Persist port assignments, delete forwarding rules on expiry, and generate fresh credentials and guest identities on each allocation. Verify that reused storage does not expose a former tenant's data.

Quote dedicated IPv4 as the supplier's full recurring cost plus handling and margin; current supplier IP prices are unverified. Shared addresses also share exposure to filtering, attack traffic, and reputation issues, so confirm provider protection and permitted NAT usage.

The control service needs two supply adapters: Proxmox placement and external VPS provisioning. Track orders, leases, capacity reservations, provider IDs, job IDs, port assignments, and export status. Retried payment webhooks must not create duplicate resources. Reconcile failed setup, refunds, expiry, and orphan resources.

Customer flow: region → duration → payment → setup status → connection link and root-access details. Offer renew/cancel, export, and reinstall. Start rental time when the service is ready. Hour/day rentals consume the same peak capacity as subscriptions during their reserved periods.

Cloud overflow accepts new placements when local capacity is exhausted. It cannot instantly move ongoing calls off a saturated host. Preserve headroom, prearrange available overflow capacity, and use planned moves for existing communities when necessary.

## Costs and occupancy

Test $9.99/month, subject to measured capacity. The following model assumes a US merchant paying 2.9% + $0.30 for domestic cards and 0.7% subscription billing, leaving $9.33036 after fees. Actual fees depend on merchant location and payment products. [Stripe pricing](https://stripe.com/pricing).

For illustration, reserve $2 per paying community-month for application support and $50 per active node-month for allocated platform, state storage, hypervisor support, and other fixed operations. These are planning allowances, not measured costs. Shared IPv4 is assumed.

| Scenario | Fixed monthly model cost | Paying customers | Modeled monthly contribution |
| --- | ---: | ---: | ---: |
| $30 host plus $50 allowance | $80 | 16 | $37.29 |
| $104 host plus $50 allowance | $154 | 20 | −$7.39 |
| $104 host plus $50 allowance | $154 | 40 | $139.21 |
| Same 40 customers plus an otherwise empty $104 spare host | At least $258 | 40 | At most $35.21 |

Formula: customers × (9.99 × 0.964 − 0.30 − 2.00) − fixed monthly cost.

This is contribution under assumptions, not net profit. It excludes unquoted setup amortization, taxes, refunds, chargebacks, transfer/IP extras, and labor exceeding the reserves. The spare-host row excludes additional licensing and operations for that spare, so it is an optimistic upper bound. Forty customers is an economic scenario, not a validated density.

The $154 model breaks even at 22 subscriptions. Adding only $104 of spare-host rent raises this to at least 36. Calculate committed costs and occupancy separately in each region. Budget spare room on active nodes as well as any purchased idle nodes.

At a proposed $99/year, the same fees and support reserve leave $5.928 per customer-month toward fixed costs, requiring 26 customers for the $154 model. Annual cash arrives upfront but commits us to a year of service. Keep annual pricing provisional until costs are measured.

The $11/year VPS promotion equals about $0.92/month of amortized supply with IPv4. A $104 host requires 114 customers merely to match that hardware cost; a $30 host requires 33, before our virtualization and operations. The example RAM budgets cannot support these counts. Keep the promotion as a serious alternative if bulk terms, automation, renewals, and performance qualify.

Hourly customer billing does not require hourly upstream billing. Monthly rented bare metal can serve short rentals, but the price must cover peak reservations, fees, support, and overflow. Use a minimum order rather than tiny separate card charges. Select public hourly/day prices after measuring media usage and obtaining complete overflow quotes.

## Overflow suppliers

The [VPS research](vps-supply-research.md) contains source links and unresolved terms. Its earlier annual-VPS-first strategy is superseded by this plan.

| Supplier | Research basis | Qualification needed |
| --- | --- | --- |
| VPSHostingService / ServerHost | User screenshot: $11/year for 1 CPU, 1 GB, 15 GB NVMe and IPv4; discount labeled recurring; Buffalo selected. | Bulk eligibility, renewal, EU availability, resale permission, provision/reimage API, sustained media performance. |
| UpCloud | Premium 1 GB $6/month, hourly billing; $3.50 Starter has a five-deployment limit. Explicit white-label program and APIs. | Wholesale and transfer agreement. Small plans add 0.5 TB to a prorated pool; fair-use throttling to 100 Mbps would constrain full ten-webcam load. |
| Cloudzy | Public 1 GB price $6.95/month; 50% discount requires annual prepayment. Provisioning API and business program. | Resale terms and written traffic example resolving marketing and billing-document discrepancies. |
| Hetzner EU cloud | Published CX23 tariff $6.49/month or $0.0104/hour, excluding IPv4 and tax. | Current stock, region/configuration, transfer, IP, and commercial terms. |

Ask for one-hour, four-hour, and twenty-four-hour quotes around 63 GB/hour for mixed media, plus 8 GB/hour screen-only, adding measured overhead. Confirm prorating, pooling, throttling, excess charges, and delete behavior. Stopping a VM may not stop charges; retained disks, IPs, and backups must also be reconciled.

## Recovery and portability

Keep lightweight community-state archives instead of paid full-VM backups. The server stores channels, users, permissions, bans, settings, operational logs, and potentially TLS identity. It has no built-in conversation archive; client recordings and external integrations are separate. Detailed code references are in the [application storage findings](vps-supply-research.md#what-the-application-stores-and-relays).

For persistent managed communities, take consistent encrypted state exports daily, before upgrades, and before expiry. Keep the latest valid export for seven days after service ends and then delete it. Offer active download and a tested restore into self-hosting. An explicit disposable opt-out may omit preservation.

Root changes can prevent export; show failures and the last successful archive. Recovery covers the managed application's state, not arbitrary installed software. Validate export/import on the pinned build, including accounts, permissions, and TLS identity.

A host failure affects every VM on it. An off-host archive enables rebuilding; it is not live failover. Establish a recovery-time target only after a timed restore and include replacement capacity in its cost.

## Next work and evidence required

1. Obtain exact US and EU pilot quotes, including setup, network, stock, IPv4 options, console/reinstall access, resale permission, and hardware replacement terms. No supplier messages have been sent.
2. Prepare a pinned Debian guest image, host-side limits, port allocation, monitoring, expiry, and portable state restore. Validate one VM first.
3. Run the [host density pilot](host-density-pilot.md), including correlated media bursts, tenant resource pressure, and recovery. Existing protocol/audio-buffer microbenchmarks do not validate this workload.
4. Replace modeled overhead and occupancy with measured results. Set admission below the highest repeatably passing density, reserve capacity for overlapping rentals, and qualify every region sold.
5. Connect checkout and run a small private paid pilot. Measure support time, peak concurrency, provisioning success, expiry, and realized contribution before public signup.

The reproduced encoded-webcam burst case passed after the [client pacing change](pilot-pacing-results-2026-10-02.md). The next gates are actual GUI media quality, a combined screen/webcam workload, sustained external traffic, and density on a suitable bare-metal/hypervisor host. The supplied test machine is itself a VM. Shared-IPv4 provisioning and portable state recovery also need validation. There is no validated oversell ratio, production density, provisioning-time guarantee, or recovery guarantee yet.

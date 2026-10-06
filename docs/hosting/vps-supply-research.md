# VPS supply research

Historical VPS research from September 30, 2026. The current [launch plan](launch-research.md) evaluates bare metal as primary supply and uses this research for overflow and additional regions. The annual-VPS-first recommendation and two-VPS pilot below are superseded; supplier facts and application findings remain reference material, subject to their stated qualifications.

Research date: September 30, 2026. Code inspected at commit `1772f827f12018472511e82b20c1cb4ba38108b1`.

Use a hybrid supply model: discounted annual VPSs for persistent communities and hourly cloud servers for temporary rentals. Each customer receives a dedicated VPS with root access, our application installed, and support for the standard managed setup. Keep self-hosting and export available. The initial capacity target is 5–10 concurrent people using the current voice, webcam, and screensharing features.

Skip paid full-VPS backups for the initial offer. Preserve community settings with a small application-state archive where recovery is promised. The main unresolved technical cost is relayed video traffic. No provider agreement, authenticated provisioning test, VPS purchase, or capacity benchmark has been completed in this research.

## What the application stores and relays

The current fork uses a server to relay voice and video. `Server::processVideoMsg` forwards a separate copy to each other user in the channel; the voice handler also builds a list of recipients. This makes VPS bandwidth part of the service cost. Calling this architecture purely peer to peer would be inaccurate. See [video and voice routing](../../src/murmur/Server.cpp) around lines 1185–1295.

The server's text-message handler forwards messages and emits a callback for RPC consumers. It does not write a built-in conversation history. The database contains community state: channels, registered users and password hashes, groups, permissions, bans, configuration, and operational logs. Server TLS certificates and private keys may also be stored in configuration. See [text routing](../../src/murmur/Messages.cpp) around line 1626, [database tables](../../src/murmur/database/ServerDatabase.cpp) around line 33, and [certificate persistence](../../src/murmur/Cert.cpp) around line 181. Client recordings and third-party RPC integrations can retain content independently; the hosting offer must describe the standard server's behavior accurately.

Recommended recovery policy:

- Rebuild the OS and application from a pinned release instead of paying for whole-machine snapshots.
- For persistent managed communities, export application state daily, before managed upgrades, and immediately before expiry or cancellation. Archive a consistent SQLite copy, the active INI, the application version, and any externally configured TLS identity files. Encrypt archives because they contain authentication material. Do not copy a live SQLite file casually; use a consistent backup or stop the service for the final export.
- Keep the latest valid application-state archive for seven days after service ends, then delete it. This preserves the previously discussed recovery window without keeping a billable VPS alive. Recovery recreates community configuration; it cannot recreate past conversations or arbitrary software installed through root access.
- Allow active customers to download their own export. A customer who opts out of state preservation receives a disposable service, with the loss of configuration explained before purchase.
- Root modifications can prevent collection or restoration. Support covers the standard setup; show the last successful export and any failure rather than promising recovery of an altered machine.

The fork already exposes `--db-json-dump` and `--db-json-import`. These are useful export/import candidates, but a whole-database dump can include operational logs and secrets. Validate a round trip on the pinned build before choosing an export format. See [CLI administration options](../../src/murmur/main.cpp) around line 263 and [database export](../../src/murmur/DBWrapper.cpp) around line 1582. No restore workflow has been tested here.

## Media capacity affects the launch promise

The [client profiles](../../src/mumble/VideoQualityProfile.cpp) target 2 Mbps for a screen share and 1.5 Mbps for a webcam. The [server defaults](../../src/murmur/Meta.cpp) permit 2.5 Mbps per video sender but cap aggregate relayed video at 20 Mbps. The aggregate guard drops packets when the ceiling is exceeded; it does not negotiate a lower client encoding rate.

These are calculated payload estimates assuming every other participant receives each stream at its configured target rate. They exclude protocol overhead, voice, retransmission behavior, and changes in actual encoder output; they are not benchmarks.

| Workload | Outgoing video payload | Transfer per hour |
| --- | ---: | ---: |
| 10 people, one 2 Mbps screen share | 18 Mbps | 8.10 GB |
| 5 people, all webcams at 1.5 Mbps | 30 Mbps | 13.50 GB |
| 10 people, all webcams at 1.5 Mbps | 135 Mbps | 60.75 GB |
| 10 people, one screen share and nine webcams | 139.5 Mbps | 62.78 GB |

Formula: sender Mbps × receiving participants; decimal GB/hour = outgoing Mbps × 0.45.

The existing 20 Mbps default is insufficient for all five or ten participants to use their webcams at target rates. A full-feature pilot should explicitly test `videobandwidthaggregate=200000000`, retain the existing per-sender limit, and set `users=10`. This is a proposed hosting configuration, not an applied change. Require sufficient provider throughput and transfer terms before adopting it. Do not solve the capacity problem by silently removing the agreed webcam or screensharing features.

## Supplier comparison and selection

| Supplier | Verified pricing basis | Automation and resale evidence | Decision |
| --- | --- | --- | --- |
| VPSHostingService / ServerHost | User checkout screenshot: 1 GB RAM, 1 Xeon core, 15 GB NVMe, one IPv4, $84/year less a $73 discount labeled recurring, yielding $11/year. Equivalent amortized cost $0.92/month; prepaid annually. | Public site advertises white-label hosting with root and WHM. This does not establish an API or an agreement covering resale of individual VPSs with customer root access. Public material advertises unmetered ports. | First annual-supply pilot candidate. Confirm coupon eligibility for multiple VPSs, renewal amount, API/reimage access, sustained media traffic, and regional availability. The screenshot quotes Buffalo; equivalent EU pricing is unverified. |
| UpCloud | Starter 1 GB: $3.50/month, limited to five concurrent deployments per account. Premium 1 GB: $6/month. Hourly billing uses a 28-day monthly cap; $6/672 is approximately $0.00893/hour. | Explicit white-label reseller program, contractual wholesale pricing, Partner API, and public create/delete APIs. Partner access still requires enrollment. | Preferred hourly automation pilot candidate. Use the Premium price for scale planning instead of depending on the five-instance Starter allowance. Obtain a transfer quote before committing to full video capacity. |
| Cloudzy | Standard 1 GB: $6.95/month; displayed $3.48/month requires twelve months prepaid. Derived standard hourly rate is approximately $6.95/720 = $0.00965, subject to the authenticated product quote. | Public API exposes instance create, delete, resize, and OS change, plus regions, products, and SSH keys. B2B program offers tailored terms; VPS resale permission and our rates are unconfirmed. | Alternative annual supplier, particularly if the $11 offer is unavailable in the EU, and an hourly comparison candidate. Resolve inconsistent marketing and billing documentation first. |
| Hetzner EU | Published June 2026 CX23 tariff: $6.49/month or $0.0104/hour, excluding IPv4 and tax. Public specification: 2 vCPUs, 4 GB RAM, 40 GB disk, 20 TB included traffic. | Public cloud API and hourly billing. Current stock, complete IPv4 quote, and applicable commercial arrangement are unverified. | Additional EU candidate if video transfer makes the other hourly options unsuitable. This is a tariff comparison, not a confirmed available deployment quote. |

Sources: [ServerHost reseller page](https://serverhost.com/reseller-vps.php), [UpCloud pricing](https://upcloud.com/global/pricing/), [UpCloud reseller program](https://upcloud.com/global/white-label-hub/), [Cloudzy pricing](https://cloudzy.com/pricing/), [Cloudzy B2B program](https://cloudzy.com/business-program/), [Cloudzy public API schema](https://api.cloudzy.com/developers/openapi.json), [Hetzner tariff adjustment](https://docs.hetzner.com/general/infrastructure-and-availability/price-adjustment/), and [Hetzner EU specifications](https://www.hetzner.com/cloud/cost-optimized/).

UpCloud's small plans contribute 0.5 TB of fair transfer, proportionally to time deployed. Allowances are shared across the account and regions. After the account limit, UpCloud may reduce throughput to 100 Mbps. That is below the calculated ten-webcam target, so free egress alone does not establish suitability for our full workload. Its public policy also describes an optional paid excess-transfer model. [Transfer accounting](https://upcloud.com/docs/products/networking/network-transfer/) and [fair-transfer policy](https://upcloud.com/fair-transfer-policy/).

Cloudzy marketing says no egress fees, while its operational documentation describes duration-proportional quotas, $0.01/GB excess transfer, and a combined monthly-price cap. Do not model the most favorable interpretation as a contractual guarantee. Ask for a written worked example covering one hour and one day of the workloads above. [Cloudzy transfer billing](https://cloudzy.com/kb/bandwidth-payg-billing/).

Expiry automation must release billable resources. UpCloud Starter/Premium and Cloudzy continue billing existing powered-off instances. UpCloud deletion can leave storage unless instructed to delete it; retained backups can also remain. Hetzner additionally bills retained Primary IPv4 resources. Verify deletion and subsequent billing, not only a stopped application. [UpCloud billing behavior](https://upcloud.com/docs/products/cloud-servers/configurations/), [UpCloud deletion API](https://developers.upcloud.com/api/1.3/server), [Cloudzy PAYG rules](https://cloudzy.com/kb/payg-billing-in-cloudzy/), and [Hetzner billing FAQ](https://docs.hetzner.com/cloud/billing/faq/).

## Proposed pilot pricing and economics

Test a starter price of $9.99/month. Test $99/year where annual supplier terms support it. These are proposed customer prices, not published offers. Hourly and daily rental prices require the media-transfer quote before selection; compute-only hourly prices understate our workload's cost.

The following estimates assume a US merchant using domestic cards at Stripe's 2.9% + $0.30, plus 0.7% Stripe Billing for subscriptions. Actual fees depend on the merchant's country, payment method, and enabled products. Contributions exclude support, shared platform costs, state-archive storage, excess traffic, taxes, refunds, and idle annual inventory. [Stripe fees](https://stripe.com/pricing).

| Monthly supplier basis | VPS cost per active month | Payment and subscription fees on $9.99 | Contribution before other costs |
| --- | ---: | ---: | ---: |
| $11 annual VPS, fully utilized | $0.92 | $0.66 | $8.41 |
| UpCloud Premium 1 GB retail | $6.00 | $0.66 | $3.33 |
| Cloudzy standard 1 GB retail | $6.95 | $0.66 | $2.38 |

At $99/year, the $11 annual VPS leaves approximately $84.14 before other costs. Twelve months of the $6 UpCloud retail plan leave approximately $23.14. Annual discounts or wholesale terms therefore matter even when monthly pricing looks viable. Annual VPS procurement also requires the full $11 upfront; amortization does not remove that cash commitment.

Suggested commercial gate: reserve at least $3 per active customer-month for management and shared costs before choosing an under-$10 recurring price. At these fee assumptions, $9.99/month permits about $6.33/month of VPS cost; $99/year permits about $59.14/year after a $36 management reserve. These reserves are planning assumptions, not measured support costs. Include purchased but idle servers when measuring fleet contribution.

For temporary rentals, obtain all-in quotes for one hour, four hours, and twenty-four hours with measured transfer. Charge for a selected duration upfront, with a minimum order to cover fixed payment fees. Start the customer's rental clock when the service is ready. Select the public hourly/day rates only after the worst permitted workload has an acceptable cost and throughput envelope.

## Supplier questionnaire ready to send

Subject: VPS supply for managed Mumble communities with customer root access

We are preparing a managed hosting service for an open-source Mumble fork with voice, webcam, and screensharing. Customers will receive a dedicated Linux VPS and root access. We will handle application setup, customer billing, and support for our standard configuration. We need annual capacity for persistent communities and hourly capacity for short rentals, initially in the US and EU.

Please confirm:

1. May we resell individual VPSs under our brand and provide each customer root access? What agreement, account type, minimum spend, or volume commitment is required?
2. What are the all-in 1 GB and 2 GB prices, including IPv4, in each proposed US/EU location? Provide hourly and annual terms, renewal amounts, availability limits, and applicable discounts. For ServerHost, please confirm whether the quoted $11/year recurring promotion applies to multiple VPSs and which regions.
3. Which APIs support creation, reinstall/reimage, status, reboot, deletion, SSH keys, and initial setup scripts? Can we test them without a large commitment? What quotas, rate limits, or partner onboarding steps apply?
4. Our calculated ten-person mixed-media workload can approach 140 Mbps of outgoing payload. Is sustained traffic at this level allowed? What fair-use rules, transfer prorating, pooling, throttling, and excess charges apply? Please price one hour and twenty-four hours at approximately 63 GB/hour, as well as a single screen share at approximately 8 GB/hour.
5. What must be deleted to stop all charges, including disks, IPs, and backups? How are partial hours billed? What costs remain during failed provisioning or reinstalls?
6. Can a root-access customer's VPS be reimaged to a fresh trusted OS with old disks, guest changes, credentials, and tenant data removed before assignment to another customer? What capacity and deployment times can we rely on?

UpCloud follow-up: confirm partner wholesale rates, the Starter limit under partner accounts, and a transfer arrangement that keeps the ten-person media workload usable after fair-transfer limits. Cloudzy follow-up: reconcile no-egress-fee marketing with the documented excess-transfer rules and monthly cap.

This questionnaire is a draft. It has not been submitted or emailed.

## Two server pilot and acceptance criteria

Start with one US annual 1 GB VPS on the $11 offer and one EU UpCloud Premium 1 GB VPS, subject to the supplier answers above. Use a 48-hour test window. If the annual promotion and hourly tariff apply, base VPS cost is approximately $11 + 48 × ($6/672) = $11.43, excluding tax and additional resources. This is a proposed purchase, not an incurred cost. Request the 2 GB quote and retest that size if measured resource headroom fails.

Pin the fork's Debian server build and checksum. The repository already has a Debian 12 packaging workflow; validate the built artifact's version and compatibility with the matching client. Install it as the standard managed service, expose TCP and UDP 64738, keep administrative interfaces private, and test public connectivity from another network. No GPU or server-side video transcoding is assumed.

| Test | Required evidence before public launch |
| --- | --- |
| Provision and connect | Five fresh deployments/assignments reach a working voice connection within a proposed ten-minute setup target. Track failures separately. A process health check alone is insufficient. |
| Voice, screen, and webcam | Exercise 5 and 10 real clients, voice-only, one screen share, all webcams, and one screen plus remaining webcams. Run the mixed workload for two hours and collect CPU, memory, throughput, loss, and client-observed quality. Test normal UDP and TCP fallback. |
| Resource headroom | Proposed gate: sustained CPU below 70% of allocated CPU capacity, peak memory below 70% of RAM, no OOM or service restarts, and stable voice during video load. Record network conditions and investigate any audible dropout or persistent visual corruption. These thresholds are pilot targets, not results. |
| Bandwidth rules | Test and obtain supplier confirmation for the workload after fair-transfer limits. Match measured traffic to billing records. A nominal port speed or a successful short run does not establish sustained capacity or final cost. |
| State recovery and export | Create channels, registered users, permissions, and bans; export and restore on a fresh instance; verify login, permissions, and TLS identity. Verify active download, seven-day retention after expiry, and deletion after retention. |
| Root access and tenant handover | Deliver only the customer's credentials. Exercise reinstall after root modifications. Allocate a used server to another customer only after verified reimage, new credentials and host identity, and removal of old tenant state. Quarantine failed resets. |
| Payment and expiry | Repeated payment webhooks create one service. Failed setup has a recorded refund outcome and no orphan VM. The expiry worker exports state, stops and deletes the hourly VM and chargeable attachments, and retries/reconciles partial failures. Confirm charges cease. |

The two-server test establishes feasibility, not regional certification. Before advertising US and EU service, repeat connectivity and capacity validation for each provider/region combination actually sold. Then run a private paid pilot with a few communities, record setup success, support time, transfer per community, expiry behavior, and realized contribution. Open public signup only after the contracted terms and observed workload support the advertised capacity.

## Minimum implementation after qualification

The repository contains the desktop client and media server, not the hosting checkout or billing service. Add a separate hosting control service and website. Keep provider credentials and payments there; send media directly between clients and their assigned Mumble server.

The first customer flow is plan → US/EU region → duration → payment → provisioning status → connection details/invite → optional root access. Provide renewal/cancellation, configuration export, and reinstall with a clear explanation of state loss. Support covers our standard setup; customers remain responsible for unrelated root modifications.

Use two supply adapters: an annual inventory allocator and an hourly provider adapter. Annual inventory may be acquired manually during the pilot, while customer assignment and application setup remain automatic. Supplier APIs or verified operator reimages must replenish safe inventory. Never reuse a root-access tenant's machine by merely deleting its Mumble database. Persist provider resource IDs and job/payment IDs so retries reconcile existing resources instead of purchasing duplicates.

The next external evidence needed is the suppliers' written commercial and transfer answers plus authenticated provisioning and workload results. The immediate engineering work after that evidence is the pinned server installer and lifecycle automation, followed by checkout and the private pilot.

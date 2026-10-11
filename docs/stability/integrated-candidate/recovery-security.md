# Recovery security scan follow-up

The historical local readiness probe explicitly requires TLS 1.2 or newer while retaining CA validation and exact archived-leaf comparison. All 59 hosting Python checks pass without skips with the compiled native crypto/pacer bridges configured. The new probe also passes against the owned Mac loopback TLS server; this is a readiness smoke check, not a new guest recovery qualification.

The recovery fixture’s SHA1 value is the legacy Mumble wire-protocol certificate-ban identifier. Production Server.cpp constructs that identifier from the DER certificate with QCryptographicHash::Sha1. Replacing only the fixture digest with SHA256 would stop the fixture from verifying the actual stored/restored ban. The call now declares `usedforsecurity=False` and documents compatibility use; the separate server identity record uses SHA256. This does not upgrade the protocol’s certificate identifier or justify a clean security gate until the exact-source scan is reviewed.

The baseline CodeQL workflow completed but its combined check failed with two annotations. Hosted analysis of this new source remains pending. Recovery readiness across all tools, non-default historical SSH port, frozen dependency inventories and exact-candidate guest drills remain open work.

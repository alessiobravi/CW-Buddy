# ADIF conformance policy

CW Buddy targets the current released ADIF specification and is intended to
remain eligible for the conformance or certification process available at the
time of each release. The current baseline is ADIF 3.1.7 (2026-03-22).

## Release rules

- Track the published ADIF release and review specification changes before each
  CW Buddy release. Updating the version is a reviewed code/data change,
  never only a documentation edit.
- Export the recommended QSO identity fields: `QSO_DATE`, `TIME_ON`, `CALL`,
  `MODE`, and `FREQ` and/or `BAND`. The record serializer emits those together
  with `BAND_RX`, `FREQ_RX`, `PROP_MODE`, `SAT_NAME`, `SAT_MODE`, `RST_SENT`,
  `RST_RCVD`, `STATION_CALLSIGN`, `MY_RIG` and `MY_ANTENNA`, omits every field
  whose value is empty, and terminates the record with `<EOR>`.
- Enforce field types, enumerations, dependencies, valid date ranges, and the
  distinction between current, import-only/deprecated, and deleted values. What
  the record builder enforces today is narrower: it resolves both bands from
  the actual RF frequencies and refuses to modify the record at all when either
  band, the frequency calculation, or a satellite's name and mode is missing.
  The remaining checks belong to the field model described below.
- Never export import-only values. Preserve unknown supported input data where
  the selected storage/export path permits round-tripping.
- Satellite QSOs export `PROP_MODE=SAT`, `SAT_NAME`, `SAT_MODE`, station transmit
  `FREQ`/`BAND`, and station receive `FREQ_RX`/`BAND_RX` as one consistent set.
- Station equipment selected by actual-RF band exports as `MY_RIG` and
  `MY_ANTENNA`. Contacted-station `RIG` is never inferred from local profiles.
- Frequencies are calculated with integer hertz. ADI MHz numbers use six
  fractional digits so transverter conversion does not lose precision.
- ADI record streaming for logger integrations is produced from a single
  record structure. A full ADI file export must reuse that same structure and
  its validation rather than growing a second field model beside it; neither
  the file export nor the validated model exists yet. ADX support will use the
  official versioned XML schema rather than a separately interpreted model, and
  has no writer today.

## Verification

- Unit tests cover the length-prefixed field encoding and record termination,
  band resolution at the edges of the band table, signed transverter offsets in
  both directions, split semantics, the satellite field set, and rejection of
  frequencies that fall outside every band or that underflow. Length prefixes
  make escaping unnecessary in ADI, and that is now asserted rather than
  assumed: an independent length-honouring reader, sharing no code with the
  writer, round-trips a callsign containing `<`, an antenna field containing a
  newline and a rig field containing a NUL, and checks that the fields after
  each one still parse. The same case pins the reverse direction, so a future
  escaping layer that altered a byte inside a callsign fails it.
- CI will download a pinned checksum-verified copy of the official versioned
  table exports, schemas, and test QSO archive; it does not do so yet. Fixtures
  are never uploaded to a live logging or award service.
- ADX output is to be schema-validated, and ADI output parsed by an independent
  test parser and round-tripped through the project's own parser. Neither
  parser exists yet.
- Release candidates are to be tested against Log4OM's inbound ADIF UDP
  service, which the record format already targets although no transport sends
  to it yet, and a conformance report will record specification version,
  fixtures, validator output, known optional fields, and any certification
  submission result.

Official references:

- [Current ADIF specification](https://adif.org/adif)
- [ADIF 3.1.7 specification](https://www.adif.org/317/ADIF_317.htm)
- [ADIF resources and implementation notes](https://www.adif.org/317/ADIF_317_Resources.htm)

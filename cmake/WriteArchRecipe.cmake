file(SHA256 "${PAYLOAD}" payload_hash)
file(READ "${TEMPLATE}" recipe)
string(REPLACE "%PAYLOAD_SHA256%" "${payload_hash}" recipe "${recipe}")
file(WRITE "${OUTPUT}" "${recipe}")

v_cc_library(
  NAME security
  SRCS
    scram_algorithm.cc
    scram_credential.cc
    scram_authenticator.cc
    acl_store.cc
    mtls.cc
    license.cc
  DEPS
    v::bytes
    absl::flat_hash_map
    absl::flat_hash_set
    cryptopp
 )

add_subdirectory(tests)

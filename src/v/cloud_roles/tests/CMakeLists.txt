rp_test(
    UNIT_TEST
    BINARY_NAME
        signature_test
    SOURCES
        signature_test.cc
    DEFINITIONS BOOST_TEST_DYN_LINK
    LIBRARIES
    v::seastar_testing_main
    Boost::unit_test_framework
    v::http
    v::cloud_roles
    ARGS "-- -c 1"
    LABELS cloud_roles
)

rp_test(
    UNIT_TEST
    BINARY_NAME
    test_cloud_roles
    SOURCES
    role_client_tests.cc
    categorization_tests.cc
    fetch_credentials_tests.cc
    credential_tests.cc
    DEFINITIONS BOOST_TEST_DYN_LINK
    LIBRARIES
    v::seastar_testing_main
    v::cloud_roles
    Boost::unit_test_framework
    ARGS "-- -c 1"
    LABELS cloud_roles
)

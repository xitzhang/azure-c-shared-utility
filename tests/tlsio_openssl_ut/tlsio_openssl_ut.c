// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "testrunnerswitcher.h"
#include "azure_c_shared_utility/tlsio.h"
#include "azure_c_shared_utility/tlsio_openssl.h"

BEGIN_TEST_SUITE(tlsio_openssl_ut)

TEST_FUNCTION(create_rejects_null_hostname)
{
    TLSIO_CONFIG config = {0};
    config.port = 443;

    ASSERT_IS_NULL(tlsio_openssl_create(&config));
}

END_TEST_SUITE(tlsio_openssl_ut)

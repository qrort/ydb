#include "tls.h"
#include <util/generic/string.h>

#include <library/cpp/testing/common/env.h>
#include <string.h>

namespace NInterconnect {

// Fake CA and cert for IC tests

TString GetPrivateKeyForTest()
{
    return TString(R"(-----BEGIN PRIVATE KEY-----
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
rJ98v+ZO1xZec9kMwz0v9iUBkSYBXkg=
-----END PRIVATE KEY-----)");

}

TString GetCertificateForTest()
{
    return TString(R"(-----BEGIN CERTIFICATE-----
MIIFPzCCAycCFF/7eZiR10RwN8JRGcAAyZC8Xu39MA0GCSqGSIb3DQEBCwUAMHIx
CzAJBgNVBAYTAlVWMREwDwYDVQQIDAhNaWxreVdheTERMA8GA1UEBwwIT3Jpb25B
cm0xDjAMBgNVBAoMBUVhcnRoMRkwFwYDVQQLDBBFYXJ0aERldmVsb3BtZW50MRIw
EAYDVQQDDAlsb2NhbGhvc3QwIBcNMjUwNTIyMTE0NjMzWhgPMjA1MjEwMDcxMTQ2
MzNaMEQxCzAJBgNVBAYTAlVWMREwDwYDVQQIDAhNaWxreVdheTEOMAwGA1UECgwF
RWFydGgxEjAQBgNVBAMMCWxvY2FsaG9zdDCCAiIwDQYJKoZIhvcNAQEBBQADggIP
ADCCAgoCggIBAOSAEAz3u3Sr90OWuCc7oABTbAQxr0UcfGlnTEqNszjjjpfZkux4
Av51qG9q9HKS2gt+zT7VNTP4VOf0N736uCDxjkeQfE6C3s1Cx5iE464UOV9Htzt9
WRirFwFyZcNa57Kf5Cq9ztZwwXkcxN66s2viRVP3NDhhcD5V0rNmAR/cG7XCea+0
PiGznUrUJUmKvvPIdA8Gb3aZ+9mlTUaQMm2GoQsuOEuuIgOGP52EAb8hhJnGGdbI
/uBYwSzh/A93gtKcsSwGvE7jPrNYG/kQg5dGZ6ogWUnkZO3roUljeA+MqgZyTHhQ
oTP9VOgSmP4QXJXxknV9RkOXPbKC364b6hcI4bAb/OJ/lZdapbzczaSdWcujSu+K
cMaTC/aE0GVRjxZaD1WdlvwY2+VUWjbBZybwp0b+W4CS2qXyHQ0EYbJFV1jGjm5V
twx4B/mvcVHxv7MGuEYIq+NXAzQ9wqwg2XHMOMaa0RFzv+HcEEQBD/OyPRm+JClS
as5bb6poKWjPoCRUSr3vLs+Euh+DX0y6IUHg/ICEd/BvD1HX34ppmlM8l6yXzQp7
ws7xuoWXb7r5yLqZyQ7XeTn0XYUp3gNr2gATqSTie3GrT+nEeDdA+qYFuHUeNZ2+
Tbo4vwNANRZgSJbjtQPUGhG6ZbpwiA497738QLki96QQrF2WsYQAYp8rAgMBAAEw
DQYJKoZIhvcNAQELBQADggIBAGkxQD3jWmdkmiefblrPtslDKdGpo3Mu18QiUrDk
vf7BrTwPbPCm4/zTYPKkxfWKbSZRJr6Fg0Glc0d5HczjDisqh519rqDif3NTGQGK
/eaVi7JQkKClIM7DUNb3nNmWwpoloDsgVbIQ9eJed1vTctcVMa0OowpL5JD6fhE+
oRHSBo8f5VZkViSA7TRoFsqLfgmZZDHp+Hvv8Z47la0AGRynnwFybea7DIRoE+6J
lNA7LPYkwKUkfz88ayGapbqldmyj0rlX74DqyS3G5pS7JYqGPC8MWtpmaklJnP+Y
J8XnEtjIZFx1j6E4ocI/pfSFCXQU2v6ICNpEOLo3IPAtpgjpCGOPXX/ppSo9/N8+
JOpT+3OlYPXKuXftCu57WD1f62nsTp/2jySN3I2Ej6xyzGSHh4Dejzvh0bNEOFMy
9Z2YFY90jgM6qLfNhrfbb/nENADusMsyjdIKX9sq/aCaAy8FKasGgukuAHFdlg2N
4fovR1GiOK7ALaUiDarytEutINLx/g9FK1S9AXL8Rs1ho3ARsvtuoiU8WILAhWcr
wCvCu8XZcCG7ZaOvTdsbxuqqBOf0+oT4g9kJmFjq4f5ogcZ41hyjvMjejmlAxJ7M
un1GLHp0TMSAhnTeWN1ImAJZDh81b0q1TbRArP9KU/DTaKTdeWwWbSiNw0zMoEN8
kUJ9
-----END CERTIFICATE-----)");
}

TString GetTempCaPathForTest()
{
    TString ca = R"(-----BEGIN CERTIFICATE-----
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
-----END CERTIFICATE-----
    )";
    char name[32] = {0};
    const char* namemask = "ydb-temp-ca-XXXXXX";
    strncpy(name, namemask, sizeof(name));
    int fd = mkstemp(name);
    Y_ABORT_UNLESS(fd > 0);
    Y_ABORT_UNLESS(write(fd, ca.data(), ca.size()) == (ssize_t)ca.size());
    return TString(name);
}

}

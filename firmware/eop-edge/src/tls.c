/*
 * The broker's CA, from the file given at build time (CMakeLists.txt). Mbed TLS parses PEM only
 * when the buffer ends with a NUL, which the inc file does not have.
 */

#include <errno.h>
#include <stddef.h>

#include <zephyr/net/tls_credentials.h>

#include "eop_tls.h"

static const unsigned char ca_certificate[] = {
#include "eop_ca_cert.inc"
	0x00,
};

int eop_tls_init(void)
{
	int ret = tls_credential_add(EOP_TLS_CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, ca_certificate,
				     sizeof(ca_certificate));

	return ret == -EEXIST ? 0 : ret;
}

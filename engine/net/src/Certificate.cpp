#include "sr/net/Connection.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <format>
#include <memory>

namespace sr::net {

namespace {

template <typename T, void (*Free)(T*)>
struct Deleter {
    void operator()(T* p) const { Free(p); }
};
using KeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using CertPtr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;

std::string OpenSslError(const char* what) {
    char text[256] = {};
    ERR_error_string_n(ERR_get_error(), text, sizeof(text));
    return std::format("{}: {}", what, text);
}

bool AddExtension(X509* cert, int nid, const std::string& value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
    if (!extension) return false;
    const bool ok = X509_add_ext(cert, extension, -1) == 1;
    X509_EXTENSION_free(extension);
    return ok;
}

bool ToPem(BIO* bio, std::string& out) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    if (size <= 0 || !data) return false;
    out.assign(data, std::size_t(size));
    return true;
}

} // namespace

bool MakeSelfSignedCertificate(const std::string& name, int validDays, std::string& certificatePem,
                               std::string& privateKeyPem, std::string& error) {
    KeyPtr key(EVP_EC_gen("P-256"));
    if (!key) {
        error = OpenSslError("key generation failed");
        return false;
    }
    CertPtr cert(X509_new());
    unsigned char serial[8] = {};
    if (!cert || RAND_bytes(serial, sizeof(serial)) != 1) {
        error = OpenSslError("certificate setup failed");
        return false;
    }
    serial[0] &= 0x7F; // a positive serial number
    BIGNUM* serialNumber = BN_bin2bn(serial, sizeof(serial), nullptr);
    BN_to_ASN1_INTEGER(serialNumber, X509_get_serialNumber(cert.get()));
    BN_free(serialNumber);

    X509_set_version(cert.get(), 2); // X.509 v3
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), long(validDays) * 24 * 3600);
    X509_set_pubkey(cert.get(), key.get());
    X509_NAME* subject = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0);
    X509_set_issuer_name(cert.get(), subject);

    // Self-signed: the certificate is its own trust anchor, so it is a CA that signs itself.
    const bool isAddress = !name.empty() && name.find_first_not_of("0123456789.:") == std::string::npos;
    if (!AddExtension(cert.get(), NID_basic_constraints, "critical,CA:TRUE") ||
        !AddExtension(cert.get(), NID_key_usage, "critical,digitalSignature,keyCertSign") ||
        !AddExtension(cert.get(), NID_subject_key_identifier, "hash") ||
        !AddExtension(cert.get(), NID_subject_alt_name, (isAddress ? "IP:" : "DNS:") + name)) {
        error = OpenSslError("certificate extensions failed");
        return false;
    }
    if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) {
        error = OpenSslError("certificate signing failed");
        return false;
    }

    BioPtr certBio(BIO_new(BIO_s_mem()));
    BioPtr keyBio(BIO_new(BIO_s_mem()));
    if (!certBio || !keyBio || PEM_write_bio_X509(certBio.get(), cert.get()) != 1 ||
        PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
        !ToPem(certBio.get(), certificatePem) || !ToPem(keyBio.get(), privateKeyPem)) {
        error = OpenSslError("PEM encoding failed");
        return false;
    }
    return true;
}

} // namespace sr::net

#pragma once
#include <string>
#include <vector>
#include <memory>

using namespace std;

// Forward declarations to avoid including OpenSSL headers here
typedef struct rsa_st RSA;
typedef struct evp_pkey_st EVP_PKEY;

class Crypto {
private:
    static inline string keyDir = "../keys/";
    static inline string privKeyFile = keyDir + "private.pem";
    static inline string pubKeyFile = keyDir + "public.pem";

public:
    // Key generation
    static bool generateKeyPair(int bits = 2048);
    static bool keysExist();
    
    // Key loading
    static string loadPublicKey();
    static string loadPrivateKey();
    static string getPublicKeyFingerprint();  // Short identifier for device
    
    // Directory management
    static void ensureKeyDir();
    static string getKeyDir() { return keyDir; }
    
    // Encryption/Decryption (will add in Step 3.4)
    // Signing/Verification (will add in Step 3.3)
};
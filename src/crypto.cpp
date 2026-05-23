#include "crypto.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstring>

// OpenSSL includes
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

using namespace std;

void Crypto::ensureKeyDir() {
    namespace fs = std::filesystem;
    if (!fs::exists(keyDir)) {
        fs::create_directories(keyDir);
        cout << "Created key directory: " << keyDir << "\n";
    }
}

bool Crypto::keysExist() {
    namespace fs = std::filesystem;
    return fs::exists(privKeyFile) && fs::exists(pubKeyFile);
}

bool Crypto::generateKeyPair(int bits) {
    ensureKeyDir();
    
    cout << "Generating " << bits << "-bit RSA key pair...\n";
    
    // Modern OpenSSL 3.x way
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    
    if (!ctx) {
        cerr << "Failed to create context\n";
        return false;
    }
    
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        cerr << "Failed to initialize keygen\n";
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0) {
        cerr << "Failed to set key size\n";
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        cerr << "Failed to generate key\n";
        EVP_PKEY_CTX_free(ctx);
        return false;
    }
    
    EVP_PKEY_CTX_free(ctx);
    
    // Save private key
    FILE* privFile = fopen(privKeyFile.c_str(), "wb");
    if (!privFile) {
        cerr << "Cannot open private key file for writing\n";
        EVP_PKEY_free(pkey);
        return false;
    }
    
    if (!PEM_write_PrivateKey(privFile, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
        cerr << "Failed to write private key\n";
        fclose(privFile);
        EVP_PKEY_free(pkey);
        return false;
    }
    fclose(privFile);
    
    // Save public key
    FILE* pubFile = fopen(pubKeyFile.c_str(), "wb");
    if (!pubFile) {
        cerr << "Cannot open public key file for writing\n";
        EVP_PKEY_free(pkey);
        return false;
    }
    
    if (!PEM_write_PUBKEY(pubFile, pkey)) {
        cerr << "Failed to write public key\n";
        fclose(pubFile);
        EVP_PKEY_free(pkey);
        return false;
    }
    fclose(pubFile);
    
    EVP_PKEY_free(pkey);
    
    cout << "✓ Key pair generated successfully!\n";
    cout << "  Private key: " << privKeyFile << "\n";
    cout << "  Public key:  " << pubKeyFile << "\n";
    
    return true;
}

string Crypto::loadPublicKey() {
    FILE* file = fopen(pubKeyFile.c_str(), "rb");
    if (!file) {
        throw runtime_error("Public key not found. Run 'lanbox keygen' first.");
    }
    
    EVP_PKEY* pkey = PEM_read_PUBKEY(file, nullptr, nullptr, nullptr);
    fclose(file);
    
    if (!pkey) {
        throw runtime_error("Failed to read public key");
    }
    
    // Convert to PEM string
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, pkey);
    
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    string keyStr(data, len);
    
    BIO_free(bio);
    EVP_PKEY_free(pkey);
    
    return keyStr;
}

string Crypto::loadPrivateKey() {
    FILE* file = fopen(privKeyFile.c_str(), "rb");
    if (!file) {
        throw runtime_error("Private key not found. Run 'lanbox keygen' first.");
    }
    
    EVP_PKEY* pkey = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
    fclose(file);
    
    if (!pkey) {
        throw runtime_error("Failed to read private key");
    }
    
    // Convert to PEM string
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    string keyStr(data, len);
    
    BIO_free(bio);
    EVP_PKEY_free(pkey);
    
    return keyStr;
}

string Crypto::getPublicKeyFingerprint() {
    try {
        string pubKey = loadPublicKey();
        
        // Calculate SHA256 hash of public key
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256((unsigned char*)pubKey.c_str(), pubKey.length(), hash);
        
        // Convert to hex string (first 16 bytes for readability)
        char fingerprint[33];
        for (int i = 0; i < 16; i++) {
            sprintf(fingerprint + (i * 2), "%02x", hash[i]);
        }
        fingerprint[32] = '\0';
        
        return string(fingerprint);
        
    } catch (const exception& e) {
        return "NO_KEY";
    }
}
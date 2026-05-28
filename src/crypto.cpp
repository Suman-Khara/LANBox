#include "crypto.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <sstream>
#include <iomanip>

// OpenSSL includes
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

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

vector<unsigned char> Crypto::signMessage(const string& message) {
    // Load private key
    FILE* file = fopen(privKeyFile.c_str(), "rb");
    if (!file) {
        throw runtime_error("Private key not found. Run 'lanbox keygen' first.");
    }
    
    EVP_PKEY* pkey = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
    fclose(file);
    
    if (!pkey) {
        throw runtime_error("Failed to read private key");
    }
    
    // Create signing context
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to create signing context");
    }
    
    // Initialize signing
    if (EVP_DigestSignInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to initialize signing");
    }
    
    // Update with message data
    if (EVP_DigestSignUpdate(md_ctx, message.c_str(), message.length()) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to update signing");
    }
    
    // Get signature length
    size_t sig_len = 0;
    if (EVP_DigestSignFinal(md_ctx, nullptr, &sig_len) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to get signature length");
    }
    
    // Get actual signature
    vector<unsigned char> signature(sig_len);
    if (EVP_DigestSignFinal(md_ctx, signature.data(), &sig_len) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to create signature");
    }
    
    signature.resize(sig_len);
    
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    
    return signature;
}

bool Crypto::verifySignature(const string& message, 
                            const vector<unsigned char>& signature,
                            const string& public_key_pem) {
    // Parse public key from PEM string
    BIO* bio = BIO_new_mem_buf(public_key_pem.c_str(), public_key_pem.length());
    if (!bio) {
        return false;
    }
    
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    
    if (!pkey) {
        return false;
    }
    
    // Create verification context
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }
    
    // Initialize verification
    if (EVP_DigestVerifyInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    
    // Update with message data
    if (EVP_DigestVerifyUpdate(md_ctx, message.c_str(), message.length()) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    
    // Verify signature
    int result = EVP_DigestVerifyFinal(md_ctx, signature.data(), signature.size());
    
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    
    return (result == 1);
}

string Crypto::toHex(const vector<unsigned char>& data) {
    ostringstream oss;
    for (unsigned char byte : data) {
        oss << hex << setfill('0') << setw(2) << (int)byte;
    }
    return oss.str();
}

vector<unsigned char> Crypto::fromHex(const string& hex) {
    vector<unsigned char> data;
    for (size_t i = 0; i < hex.length(); i += 2) {
        string byteStr = hex.substr(i, 2);
        unsigned char byte = (unsigned char)strtol(byteStr.c_str(), nullptr, 16);
        data.push_back(byte);
    }
    return data;
}

// ============================================================================
// AES ENCRYPTION/DECRYPTION
// ============================================================================

vector<unsigned char> Crypto::generateAESKey(int bits) {
    int bytes = bits / 8;  // 256 bits = 32 bytes
    vector<unsigned char> key(bytes);
    
    // Generate random bytes
    if (RAND_bytes(key.data(), bytes) != 1) {
        throw runtime_error("Failed to generate random AES key");
    }
    
    return key;
}

vector<unsigned char> Crypto::generateIV() {
    // AES block size is always 128 bits (16 bytes)
    vector<unsigned char> iv(16);
    
    if (RAND_bytes(iv.data(), 16) != 1) {
        throw runtime_error("Failed to generate random IV");
    }
    
    return iv;
}

bool Crypto::encryptFileAES(const string& input_file,
                           const string& output_file,
                           const vector<unsigned char>& aes_key,
                           const vector<unsigned char>& iv) {
    // Open input file
    ifstream infile(input_file, ios::binary);
    if (!infile.is_open()) {
        cerr << "Cannot open input file: " << input_file << "\n";
        return false;
    }
    
    // Open output file
    ofstream outfile(output_file, ios::binary);
    if (!outfile.is_open()) {
        cerr << "Cannot open output file: " << output_file << "\n";
        return false;
    }
    
    // Create encryption context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    
    // Initialize encryption (AES-256-CBC)
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, aes_key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    
    // Encrypt file in chunks
    const int BUFFER_SIZE = 64 * 1024;  // 64KB chunks
    vector<unsigned char> inbuf(BUFFER_SIZE);
    vector<unsigned char> outbuf(BUFFER_SIZE + EVP_CIPHER_block_size(EVP_aes_256_cbc()));
    
    while (infile.read((char*)inbuf.data(), BUFFER_SIZE) || infile.gcount() > 0) {
        int bytes_read = infile.gcount();
        int outlen = 0;
        
        if (EVP_EncryptUpdate(ctx, outbuf.data(), &outlen, inbuf.data(), bytes_read) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }
        
        outfile.write((char*)outbuf.data(), outlen);
    }
    
    // Finalize encryption (handles padding)
    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx, outbuf.data(), &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    
    if (final_len > 0) {
        outfile.write((char*)outbuf.data(), final_len);
    }
    
    EVP_CIPHER_CTX_free(ctx);
    infile.close();
    outfile.close();
    
    return true;
}

bool Crypto::decryptFileAES(const string& input_file,
                           const string& output_file,
                           const vector<unsigned char>& aes_key,
                           const vector<unsigned char>& iv) {
    // Open encrypted file
    ifstream infile(input_file, ios::binary);
    if (!infile.is_open()) {
        cerr << "Cannot open encrypted file: " << input_file << "\n";
        return false;
    }
    
    // Open output file
    ofstream outfile(output_file, ios::binary);
    if (!outfile.is_open()) {
        cerr << "Cannot open output file: " << output_file << "\n";
        return false;
    }
    
    // Create decryption context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    
    // Initialize decryption
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, aes_key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    
    // Decrypt file in chunks
    const int BUFFER_SIZE = 64 * 1024;
    vector<unsigned char> inbuf(BUFFER_SIZE);
    vector<unsigned char> outbuf(BUFFER_SIZE + EVP_CIPHER_block_size(EVP_aes_256_cbc()));
    
    while (infile.read((char*)inbuf.data(), BUFFER_SIZE) || infile.gcount() > 0) {
        int bytes_read = infile.gcount();
        int outlen = 0;
        
        if (EVP_DecryptUpdate(ctx, outbuf.data(), &outlen, inbuf.data(), bytes_read) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }
        
        outfile.write((char*)outbuf.data(), outlen);
    }
    
    // Finalize decryption
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx, outbuf.data(), &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    
    if (final_len > 0) {
        outfile.write((char*)outbuf.data(), final_len);
    }
    
    EVP_CIPHER_CTX_free(ctx);
    infile.close();
    outfile.close();
    
    return true;
}

// ============================================================================
// RSA ENCRYPTION/DECRYPTION (for encrypting AES keys)
// ============================================================================

vector<unsigned char> Crypto::encryptRSA(const vector<unsigned char>& data,
                                        const string& public_key_pem) {
    // Parse public key
    BIO* bio = BIO_new_mem_buf(public_key_pem.c_str(), public_key_pem.length());
    if (!bio) {
        throw runtime_error("Failed to create BIO for public key");
    }
    
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    
    if (!pkey) {
        throw runtime_error("Failed to read public key");
    }
    
    // Create encryption context
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    if (!ctx) {
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to create encryption context");
    }
    
    if (EVP_PKEY_encrypt_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to initialize encryption");
    }
    
    // Set padding
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to set padding");
    }
    
    // Get output size
    size_t outlen;
    if (EVP_PKEY_encrypt(ctx, nullptr, &outlen, data.data(), data.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to determine output size");
    }
    
    // Encrypt
    vector<unsigned char> encrypted(outlen);
    if (EVP_PKEY_encrypt(ctx, encrypted.data(), &outlen, data.data(), data.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("RSA encryption failed");
    }
    
    encrypted.resize(outlen);
    
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    
    return encrypted;
}

vector<unsigned char> Crypto::decryptRSA(const vector<unsigned char>& encrypted_data) {
    // Load private key
    FILE* file = fopen(privKeyFile.c_str(), "rb");
    if (!file) {
        throw runtime_error("Private key not found");
    }
    
    EVP_PKEY* pkey = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
    fclose(file);
    
    if (!pkey) {
        throw runtime_error("Failed to read private key");
    }
    
    // Create decryption context
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    if (!ctx) {
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to create decryption context");
    }
    
    if (EVP_PKEY_decrypt_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to initialize decryption");
    }
    
    // Set padding
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to set padding");
    }
    
    // Get output size
    size_t outlen;
    if (EVP_PKEY_decrypt(ctx, nullptr, &outlen, encrypted_data.data(), encrypted_data.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("Failed to determine output size");
    }
    
    // Decrypt
    vector<unsigned char> decrypted(outlen);
    if (EVP_PKEY_decrypt(ctx, decrypted.data(), &outlen, encrypted_data.data(), encrypted_data.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw runtime_error("RSA decryption failed");
    }
    
    decrypted.resize(outlen);
    
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    
    return decrypted;
}

// ============================================================================
// HYBRID ENCRYPTION (Convenience functions)
// ============================================================================

Crypto::EncryptedFileInfo Crypto::encryptFile(const string& input_file,
                                              const string& output_file,
                                              const string& recipient_public_key) {
    EncryptedFileInfo info;
    
    // Step 1: Generate random AES key and IV
    vector<unsigned char> aes_key = generateAESKey(256);
    vector<unsigned char> iv = generateIV();
    
    // Step 2: Encrypt file with AES
    if (!encryptFileAES(input_file, output_file, aes_key, iv)) {
        throw runtime_error("AES file encryption failed");
    }
    
    // Step 3: Encrypt AES key with recipient's RSA public key
    info.encrypted_aes_key = encryptRSA(aes_key, recipient_public_key);
    info.iv = iv;
    info.encrypted_file_path = output_file;
    
    return info;
}

bool Crypto::decryptFile(const string& encrypted_file,
                        const string& output_file,
                        const vector<unsigned char>& encrypted_aes_key,
                        const vector<unsigned char>& iv) {
    try {
        // Step 1: Decrypt AES key using our private key
        vector<unsigned char> aes_key = decryptRSA(encrypted_aes_key);
        
        // Step 2: Decrypt file using AES key
        return decryptFileAES(encrypted_file, output_file, aes_key, iv);
        
    } catch (const exception& e) {
        cerr << "Decryption failed: " << e.what() << "\n";
        return false;
    }
}